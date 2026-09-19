#include "networkmanager.h"
#include "mathsplit.h"
#include <QRegularExpression>
#include <QDebug>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFileInfo>
#include <QPair>
#include <QSet>
#include <algorithm>

// ---------------------------------------------------------------------------
// Вспомогательные функции для extractTaskContent (файлово-локальные)
// ---------------------------------------------------------------------------

// Находит открывающий тег <div ...>, у которого в атрибуте class есть ТОЧНО
// класс `cls` (как отдельный токен, не подстрока типа math100-spoiler-title).
// Возвращает позицию '<' или -1.
static int findDivByExactClass(const QString &html, int from, const QString &cls)
{
    QRegularExpression re{R"(<div\b[^>]*?class\s*=\s*["']([^"']*)["'][^>]*>)"};
    QRegularExpressionMatchIterator it = re.globalMatch(html, from);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const QStringList classes =
            m.captured(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (classes.contains(cls)) {
            return m.capturedStart(0);
        }
    }
    return -1;
}

// Balanced-сканирование: дана позиция '<' открывающего тега <tag ...>,
// возвращает позицию (exclusive) конца соответствующего закрывающего тега
// </tag> с учётом вложенности. Возвращает -1, если закрывающий тег не найден.
// closeTagStart (опц.) — позиция НАЧАЛА закрывающего тега.
static int balancedTagEnd(const QString &html, int openPos, const QString &tag,
                          int *closeTagStart = nullptr)
{
    QRegularExpression tagRe{
        QStringLiteral("<%1\\b|</%1\\s*>").arg(tag),
        QRegularExpression::CaseInsensitiveOption};
    int depth = 1;
    int searchFrom = openPos + tag.length() + 1; // сразу после "<tag"
    QRegularExpressionMatchIterator it = tagRe.globalMatch(html, searchFrom);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        if (m.captured(0).startsWith(QStringLiteral("</"))) {
            --depth;
            if (depth == 0) {
                if (closeTagStart) {
                    *closeTagStart = m.capturedStart(0);
                }
                return m.capturedEnd(0);
            }
        } else {
            ++depth;
        }
    }
    return -1;
}

// task-11: объявления (определения ниже, после findTagByExactClass).
static int findTagByExactClass(const QString &html, int from,
                               const QString &tag, const QString &cls);
static QString stripTrailingMathSlash(const QString &html);

// ---------------------------------------------------------------------------
// NetworkManager
// ---------------------------------------------------------------------------

NetworkManager::NetworkManager(QObject *parent)
    : QObject(parent)
{
    m_networkManager.setTransferTimeout(30000);
    connect(&m_networkManager, &QNetworkAccessManager::finished,
            this, &NetworkManager::onReplyFinished);
}

void NetworkManager::fetchVariantPage(const QString &variantUrl)
{
    m_variantBaseUrl = variantUrl;
    updateProgress(5);

    QNetworkRequest request{QUrl(variantUrl)};
    request.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    auto *reply = m_networkManager.get(request);
    reply->setProperty("type", "variant");
    m_activeReplies[variantUrl] = reply;
}

void NetworkManager::fetchTaskPage(const QString &taskUrl)
{
    updateProgress(10);

    QNetworkRequest request{QUrl(taskUrl)};
    request.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    auto *reply = m_networkManager.get(request);
    reply->setProperty("type", "task");
    m_activeReplies[taskUrl] = reply;
}

void NetworkManager::clearCache()
{
    QString cacheDir = QCoreApplication::applicationDirPath() + "/cache";
    QDir(cacheDir + "/pages").removeRecursively();
    QDir(cacheDir + "/images").removeRecursively();
    QDir(cacheDir + "/pages").mkpath(".");
    QDir(cacheDir + "/images").mkpath(".");
    m_pageCache.clear();
    emit downloadProgress(0);
}

void NetworkManager::onReplyFinished(QNetworkReply *reply)
{
    QString url = reply->url().toString();
    QString type = reply->property("type").toString();

    if (reply->error() != QNetworkReply::NoError) {
        QString error = QString("Ошибка: %1 (%2)")
            .arg(reply->errorString())
            .arg(url);
        emit errorOccurred(error);
        m_activeReplies.remove(url);
        reply->deleteLater();
        return;
    }

    QByteArray data = reply->readAll();

    if (type == "variant") {
        updateProgress(50);
        m_activeReplies.remove(url);
        reply->deleteLater();

        QString html = QString::fromUtf8(data);
        QStringList taskUrls = extractTaskUrls(html, m_variantBaseUrl);
        // task-9: кап mid(0, 30) УБРАН — возвращаем все найденные задачи
        // (на live-страницах вариантов их может быть > 30, например 33).

        // Pre-cache task pages sequentially.
        // (task-7: «скачать+почистить+в кэш» вынесен в fetchAndCacheTaskPage —
        //  тем же путём пользуется buildExportDocument.)
        for (const QString &taskUrl : taskUrls) {
            QString cleanTaskHtml;
            fetchAndCacheTaskPage(taskUrl, cleanTaskHtml);
        }

        updateProgress(80);
        emit pageListFetched(taskUrls);
    } else if (type == "task") {
        m_activeReplies.remove(url);
        reply->deleteLater();

        QString html = QString::fromUtf8(data);
        // Чистим страницу: только контент задачи (post-content), без рекламы и сайта.
        QString taskTitle;
        QString cleanHtml = NetworkManager::extractTaskContent(html, &taskTitle);
        if (!taskTitle.isEmpty()) {
            qInfo() << "Task title:" << taskTitle;
        }
        QString fixedHtml = fixImageUrls(cleanHtml, url);
        saveToCache(url, "pages", fixedHtml.toUtf8());
        emit taskPageFetched(fixedHtml, url);
    }
}

// ---------------------------------------------------------------------------
// task-7: «скачать + почистить + в кэш» (общий для прекаша и экспорта)
// ---------------------------------------------------------------------------

bool NetworkManager::fetchAndCacheTaskPage(const QString &taskUrl, QString &cleanHtmlOut)
{
    // Уже в кэше (память или диск) — сеть не используем.
    QByteArray cached;
    if (loadFromCache(taskUrl, "pages", cached) && !cached.isEmpty()) {
        cleanHtmlOut = QString::fromUtf8(cached);
        return true;
    }

    QNetworkRequest req{QUrl(taskUrl)};
    req.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    auto *taskReply = m_networkManager.get(req);
    taskReply->setProperty("type", "cached_task");

    // ВАЖНО (task-7): QNAM::finished -> onReplyFinished вызывает
    // reply->readAll() на ЛЮБОЙ успешной репли (до ветвления по type) и
    // потребляет буфер — читать reply после loop.exec() бессмысленно (0 байт).
    // Собираем тело сами по readyRead (контекст = loop: соединение
    // отключается при выходе из области видимости до освобождения taskData).
    QByteArray taskData;
    QEventLoop loop;
    QObject::connect(taskReply, &QNetworkReply::readyRead, &loop,
                     [taskReply, &taskData]() { taskData += taskReply->readAll(); });
    QObject::connect(taskReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    bool ok = false;
    // 4xx/5xx у QNAM — NoError, поэтому смотрим и HTTP-статус:
    // на 404-странице math100.ru тоже есть div.post-content, без проверки
    // статуса в кэш лег бы «Контент не найден».
    const int httpStatus =
        taskReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (taskReply->error() == QNetworkReply::NoError && httpStatus == 200
        && !taskData.isEmpty()) {
        // В кэш кладём ТОЛЬКО чистый контент задачи (без рекламы/сайта),
        // спойлеры уже сконвертированы в видимые блоки task-answer-*.
        QString cleanTask = extractTaskContent(QString::fromUtf8(taskData));
        QString fixedHtml = fixImageUrls(cleanTask, taskUrl);
        if (!fixedHtml.trimmed().isEmpty()) {
            saveToCache(taskUrl, "pages", fixedHtml.toUtf8());
            cleanHtmlOut = fixedHtml;
            ok = true;
        }
    }
    taskReply->deleteLater();
    return ok;
}

QString NetworkManager::resolveUrl(const QString &relativeUrl, const QString &baseUrl) const
{
    QUrl url{relativeUrl};
    if (!url.scheme().isEmpty()) {
        return url.toString();
    }
    QUrl base{baseUrl};
    return base.resolved(url).toString();
}

QStringList NetworkManager::extractTaskUrls(const QString &html, const QString &baseUrl) const
{
    QStringList urls;

    // task-9 ОСНОВНОЙ паттерн: якорь math100.ru с текстом «Задача N».
    // Работает и для PROF (ege_profil_*) и для BAZ (baza_*), и отбрасывает
    // чужие/служебные якоря без текстовой подписи (например
    // ege_profil_5_1-1 с пустым текстом на странице базового варианта).
    // Референс (python):
    //   re.findall(r'href=["\'](https?://math100\.ru/[^"\']+)["\'][^>]*>\s*Задача\s*(\d+)\s*<',
    //              html, re.I)
    QRegularExpression taskNRe{
        R"(<a[^>]*href=["'](https?://math100\.ru/[^"']+)["'][^>]*>\s*Задача\s*(\d+)\s*(?:</a>|<))",
        QRegularExpression::CaseInsensitiveOption};
    QVector<QPair<QString, int>> found;
    QSet<QString> seen;
    QRegularExpressionMatchIterator it = taskNRe.globalMatch(html);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const QString url = m.captured(1);
        if (!seen.contains(url)) {
            seen.insert(url);
            found.append({url, m.captured(2).toInt()});
        }
    }
    if (!found.isEmpty()) {
        // Стабильная сортировка по номеру N: при равных/не распознанных N
        // сохраняется порядок появления в документе.
        std::stable_sort(found.begin(), found.end(),
                         [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
                             return a.second < b.second;
                         });
        for (const auto &p : found) {
            urls.append(p.first);
        }
        return urls;
    }

    // Паттерн 1 (fallback): ссылки с "ege_profil" в URL
    QRegularExpression taskPattern{R"(href=["'](https?://math100\.ru/ege_profil_[^"']+)["'])"};
    QRegularExpressionMatchIterator taskIt = taskPattern.globalMatch(html);
    while (taskIt.hasNext()) {
        QRegularExpressionMatch match = taskIt.next();
        QString fullUrl = match.captured(1);
        if (!urls.contains(fullUrl)) {
            urls.append(fullUrl);
        }
    }

    // Паттерн 2: ссылки с текстом "Задача" рядом
    if (urls.isEmpty()) {
        QRegularExpression linkPattern{R"(href=["'](https?://math100\.ru/[^"']+)["'][^>]*>([^<]*(?:Задача|ege)[^<]*)<)"};
        QRegularExpressionMatchIterator linkIt = linkPattern.globalMatch(html);
        while (linkIt.hasNext()) {
            QRegularExpressionMatch linkMatch = linkIt.next();
            QString href = linkMatch.captured(1);
            QString text = linkMatch.captured(2);
            if (href.contains("math100.ru") && (text.contains("Задача") || href.contains("ege"))) {
                if (!urls.contains(href)) {
                    urls.append(href);
                }
            }
        }
    }

    // Паттерн 3: fallback — генерация по шаблону
    if (urls.isEmpty()) {
        QRegularExpression variantPattern{R"(prof-ege-\d+-\d+-(?:\d+)/)"};
        if (variantPattern.match(html).hasMatch()) {
            for (int i = 1; i <= 34; ++i) {
                urls.append(QString("https://math100.ru/ege_profil_8_1-%1/")
                    .arg(i));
            }
        }
    }

    return urls;
}

QString NetworkManager::extractTaskContent(const QString &html, QString *titleOut)
{
    // 1. Название задачи (для title)
    if (titleOut) {
        QRegularExpression titleRe{
            R"(<span\s+class="entry-title[^"]*"[^>]*>([^<]+)</span>)"};
        QRegularExpressionMatch m = titleRe.match(html);
        *titleOut = m.hasMatch() ? m.captured(1).trimmed() : QString();
    }

    // 2. Находим контейнер: div.post-content, fallback <main id="main">,
    //    fallback — весь html как есть (не ронять приложение).
    int containerStart = -1;
    QString containerTag;
    QRegularExpression postRe{
        R"(<div[^>]*class=["'][^"']*post-content[^"']*["'][^>]*>)"};
    QRegularExpressionMatch pm = postRe.match(html);
    if (pm.hasMatch()) {
        containerStart = pm.capturedStart(0);
        containerTag = QStringLiteral("div");
    } else {
        QRegularExpression mainRe{R"(<main[^>]*id=["']main["'][^>]*>)"};
        QRegularExpressionMatch mm = mainRe.match(html);
        if (mm.hasMatch()) {
            containerStart = mm.capturedStart(0);
            containerTag = QStringLiteral("main");
        }
    }

    QString content = html;
    if (containerStart >= 0) {
        int end = balancedTagEnd(html, containerStart, containerTag);
        if (end > containerStart) {
            content = html.mid(containerStart, end - containerStart);
        }
        // Если balanced-скан не дошёл до конца — оставляем исходный html.
    }

    // 3. Убираем script / iframe / noscript (case-insensitive, dot-matches-everything)
    const auto opts = QRegularExpression::CaseInsensitiveOption
        | QRegularExpression::DotMatchesEverythingOption;
    content.replace(QRegularExpression{R"(<script\b[^>]*>.*?</script\s*>)", opts}, QString());
    content.replace(QRegularExpression{R"(<script\b[^>]*/\s*>)", opts}, QString());
    content.replace(QRegularExpression{R"(<iframe\b[^>]*>.*?</iframe\s*>)", opts}, QString());
    content.replace(QRegularExpression{R"(<iframe\b[^>]*/\s*>)", opts}, QString());
    content.replace(QRegularExpression{R"(<noscript\b[^>]*>.*?</noscript\s*>)", opts}, QString());

    // 4. Убираем div'ы с yandex_rtb (balanced — корректно и при вложенном содержимом)
    for (int guard = 0; guard < 1000; ++guard) {
        QRegularExpression rtbRe{R"(<div[^>]*yandex_rtb[^>]*>)", QRegularExpression::CaseInsensitiveOption};
        QRegularExpressionMatchIterator it = rtbRe.globalMatch(content);
        if (!it.hasNext()) {
            break;
        }
        int idx = it.next().capturedStart(0);
        int end = balancedTagEnd(content, idx, QStringLiteral("div"));
        if (end < 0) {
            content.remove(idx, content.length() - idx); // битая вложенность — вырезаем до конца
            break;
        }
        content.remove(idx, end - idx);
    }

    // 5. Конвертация спойлеров math100-spoiler в видимые блоки task-answer-*.
    //    Классы НЕ содержат подстроку "spoiler" (app.js::removeSpoilers прячет
    //    любой div с "spoiler" в class).
    for (int guard = 0; guard < 1000; ++guard) {
        int sp = findDivByExactClass(content, 0, QStringLiteral("math100-spoiler"));
        if (sp < 0) {
            break;
        }
        int end = balancedTagEnd(content, sp, QStringLiteral("div"));
        if (end <= sp) {
            break; // некорректная вложенность — не зацикливаться
        }
        const QString spHtml = content.mid(sp, end - sp);

        // Заголовок спойлера
        QString title = QStringLiteral("Спойлер");
        QRegularExpression spTitleRe{
            R"(<span[^>]*class=["'][^"']*math100-spoiler-title-text[^"']*["'][^>]*>\s*([^<]*?)\s*</span>)"};
        QRegularExpressionMatch tm = spTitleRe.match(spHtml);
        if (tm.hasMatch() && !tm.captured(1).trimmed().isEmpty()) {
            title = tm.captured(1).trimmed();
        }

        // Содержимое content-div'а (balanced, без его style-атрибута и
        // без собственного закрывающего тега)
        QString body;
        int cStart = findDivByExactClass(spHtml, 0, QStringLiteral("math100-spoiler-content"));
        if (cStart >= 0) {
            int cCloseStart = -1;
            int cEnd = balancedTagEnd(spHtml, cStart, QStringLiteral("div"), &cCloseStart);
            int tagEnd = spHtml.indexOf(QLatin1Char('>'), cStart) + 1;
            if (cCloseStart >= 0 && cCloseStart > tagEnd) {
                body = spHtml.mid(tagEnd, cCloseStart - tagEnd);
            } else if (cEnd > tagEnd) {
                // Закрывающий тег не найден — берём всё до конца и срезаем
                // возможный хвостовой </div>
                body = spHtml.mid(tagEnd, cEnd - tagEnd);
                body.remove(QRegularExpression{
                    QStringLiteral("</div\\s*>)\\s*$"),
                    QRegularExpression::CaseInsensitiveOption});
            }
        }

        const QString replacement =
            QStringLiteral("<div class=\"task-answer-block\">"
                           "<p class=\"task-answer-label\"><strong>%1</strong></p>"
                           "<div class=\"task-answer-body\">%2</div></div>")
                 .arg(title.toHtmlEscaped(), body);
        content.replace(sp, end - sp, replacement);
    }

    // 6. task-11: хвостовой «/» — перенос одиночного косолапого с конца
    //    формулы из span.math наружу (сразу после </span>). Применяется к
    //    всему чистому контенту (тело + блоки Ответ/Решение) — одна точка
    //    покрывает превью, прекаш и экспорт.
    content = stripTrailingMathSlash(content);

    return content;
}

QString NetworkManager::fixImageUrls(const QString &html, const QString &taskUrl) const
{
    QString result = html;

    // Находим все img с math100.ru (картинки живут в /images/ и /wp-content/uploads/)
    QRegularExpression imgPattern{R"(<img([^>]*)src=["'](https?://math100\.ru/[A-Za-z0-9_\-./]+\.(?:png|jpg|jpeg|gif|webp))["']([^>]*)>)"};

    // Скачиваем все изображения синхронно (по очереди)
    QRegularExpressionMatchIterator imgIt = imgPattern.globalMatch(result);
    QVector<QString> imageUrls;
    while (imgIt.hasNext()) {
        QRegularExpressionMatch imgMatch = imgIt.next();
        QString fullImageUrl = imgMatch.captured(2);
        if (!imageUrls.contains(fullImageUrl)) {
            imageUrls.append(fullImageUrl);
        }
    }

    for (const QString &fullImageUrl : imageUrls) {
        QNetworkRequest imgReq{QUrl(fullImageUrl)};
        auto *imgReply = m_networkManager.get(imgReq);

        // task-7: тело собираем по readyRead — onReplyFinished (QNAM::finished)
        // уже спотребил бы readAll(), и чтение после loop.exec() дало бы 0 байт
        // (в кэш попадал бы пустой файл, а в HTML — пустой data-URL).
        QByteArray imgData;
        QEventLoop loop;
        QObject::connect(imgReply, &QNetworkReply::readyRead, &loop,
                         [imgReply, &imgData]() { imgData += imgReply->readAll(); });
        QObject::connect(imgReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (imgReply->error() == QNetworkReply::NoError && !imgData.isEmpty()) {
            saveToCache(fullImageUrl, "images", imgData);
        }
        imgReply->deleteLater();
    }

    // Заменяем URL на data URLs
    QRegularExpressionMatchIterator imgIt2 = imgPattern.globalMatch(result);
    while (imgIt2.hasNext()) {
        QRegularExpressionMatch imgMatch = imgIt2.next();
        QString fullImageUrl = imgMatch.captured(2);
        QString attrsOpen = imgMatch.captured(1);
        QString attrsClose = imgMatch.captured(3);

        QString cachePath = cacheFilePath(fullImageUrl, "images");
        QFile file{cachePath};
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray imgData = file.readAll();
            file.close();
            // Пустой кэш-файл (ошибка скачивания ранее) — не пишем битый
            // data-URL: оставляем исходный URL картинки.
            if (imgData.isEmpty()) {
                continue;
            }
            QString ext = QFileInfo(fullImageUrl).suffix().toLower();
            QString mime = (ext == "jpg" || ext == "jpeg") ? "image/jpeg"
                         : ext == "gif" ? "image/gif"
                         : ext == "webp" ? "image/webp"
                         : "image/png";
            QString dataUrl = QString("data:%1;base64,%2")
                .arg(mime, QString(imgData.toBase64()));

            result.replace(imgMatch.captured(0),
                QString("<img%1src=\"%2\"%3>")
                    .arg(attrsOpen, dataUrl, attrsClose));
        }
    }

    // Спойлеры math100-spoiler здесь НЕ вырезаются: они уже сконвертированы
    // в видимые блоки task-answer-* функцией extractTaskContent().
    // (Старый негребедийный spoilerPattern ломал вложенные div и терял
    //  Ответ/Решение — он удалён.)

    // Удаляем рекламу
    QRegularExpression adsPattern{
        R"(<(div|script|iframe|a)[^>]*(?:adsbygoogle|math100-promo-bar|promo)[^>]*>.*?<\/\1>)",
        QRegularExpression::DotMatchesEverythingOption};
    result.replace(adsPattern, "");

    // Удаляем cookie banner
    QRegularExpression cookiePattern{
        R"(<div[^>]*class=["'][^"']*?cookie[^"']*?["'][^>]*>.*?<\/div>)",
        QRegularExpression::DotMatchesEverythingOption};
    result.replace(cookiePattern, "");

    return result;
}

QString NetworkManager::cacheFilePath(const QString &url, const QString &subdir) const
{
    QString filename = urlToFilename(url);
    QString cacheDir = QCoreApplication::applicationDirPath();
    return QString("%1/cache/%2/%3").arg(cacheDir, subdir, filename);
}

QString NetworkManager::urlToFilename(const QString &url) const
{
    QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5);
    return QString("%1.png").arg(hash.toHex());
}

bool NetworkManager::loadFromCache(const QString &url, const QString &subdir, QByteArray &data) const
{
    QMutexLocker locker{&m_cacheMutex};
    if (m_pageCache.contains(url)) {
        data = m_pageCache[url];
        return true;
    }

    QString path = cacheFilePath(url, subdir);
    QFile file{path};
    if (file.open(QIODevice::ReadOnly)) {
        data = file.readAll();
        file.close();
        return true;
    }
    return false;
}

void NetworkManager::saveToCache(const QString &url, const QString &subdir, const QByteArray &data) const
{
    QMutexLocker locker{&m_cacheMutex};
    m_pageCache[url] = data;

    QString path = cacheFilePath(url, subdir);
    QDir dir;
    dir.mkpath(QFileInfo(path).path());

    QFile file{path};
    if (file.open(QIODevice::WriteOnly)) {
        file.write(data);
        file.close();
    }
}

void NetworkManager::updateProgress(int percent)
{
    percent = qBound(0, percent, 100);
    emit downloadProgress(percent);
}

// ---------------------------------------------------------------------------
// task-7: сборка мультитаск PDF-документа (экспорт)
// ---------------------------------------------------------------------------

// Общий вариант findDivByExactClass для любого тега (span/div/…).
// task-9: нужен для поиска <span class="math"> при конвертации в display.
static int findTagByExactClass(const QString &html, int from,
                               const QString &tag, const QString &cls)
{
    QRegularExpression re{
        QStringLiteral("<%1\\b[^>]*?class\\s*=\\s*[\"']([^\"']*)[\"'][^>]*>").arg(tag)};
    QRegularExpressionMatchIterator it = re.globalMatch(html, from);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const QStringList classes =
            m.captured(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (classes.contains(cls)) {
            return m.capturedStart(0);
        }
    }
    return -1;
}

// task-9: нумерация задач в экспорте. Если последний сегмент пути URL
// оканчивается на «-<число>» (ege_profil_8_1-5 -> 5, baza_2025_17_1-18 -> 18),
// это и есть N; иначе — fallback (индекс+1).
static int taskNumberFromUrl(const QString &url, int fallback)
{
    const QStringList segments = QUrl(url).path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty()) {
        return fallback;
    }
    const QRegularExpression re{R"(^(?:.*-)?(\d+)$)"};
    const QRegularExpressionMatch m = re.match(segments.last());
    return m.hasMatch() ? m.captured(1).toInt() : fallback;
}

// task-9: «полная форма» экспорта — во всём содержимом span.math
// инлайн-разделители \( … \) превращаются в display \[ … \] (формула
// на отдельной строке, по центру). ТОЛЬКО внутри span.math (balanced-скан
// span), остальной HTML не трогаем; уже display \[…\] не меняется
// (заменяется точечно последовательность «бэкслеш+скобка»).
static QString convertMathSpansToDisplay(const QString &html)
{
    QString result = html;
    int from = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        const int pos = findTagByExactClass(result, from,
                                             QStringLiteral("span"), QStringLiteral("math"));
        if (pos < 0) {
            break;
        }
        int closeStart = -1;
        const int end = balancedTagEnd(result, pos, QStringLiteral("span"), &closeStart);
        if (end < 0 || closeStart < 0 || closeStart <= pos) {
            break; // некорректная вложенность — не трогаем остаток
        }
        const int tagEnd = result.indexOf(QLatin1Char('>'), pos) + 1;
        if (tagEnd <= 0 || tagEnd >= closeStart) {
            break;
        }
        QString inner = result.mid(tagEnd, closeStart - tagEnd);
        inner.replace(QStringLiteral("\\("), QStringLiteral("\\["));
        inner.replace(QStringLiteral("\\)"), QStringLiteral("\\]"));
        // task-13: длинные цепочки (\,\,\, ⇔ \,\,\, …) -> по строке на шаг.
        // Только здесь, потому что формула уже display (\[ … \]) — перенос
        // «\\» допустим. Короткие формулы и \left/\right — функция не трогает.
        inner = splitWideMathChains(inner);
        result.replace(tagEnd, closeStart - tagEnd, inner);
        from = tagEnd + inner.length();
    }
    return result;
}

// task-11b: «одиночные» формулы TABLE-режима — см. объявление в .h.
// Алгоритм:
//   1. Balanced-скан всех <p>/<li>-блоков (по порядку, не вложенный повтор:
//      после блока — переход за его конец).
//   2. Регион блока = содержимое до первого вложенного <p>/<div> (спека).
//   3. В регионе — ВСЕ span.math (balanced, точный токен класса «math»).
//   4. Остаточный текст = регион минус math-спаны, минус все теги,
//      &nbsp;/&#160; считаются пробелом. Если в нём > 20 не-пробельных
//      символов — это предложение: блок не трогаем.
//   5. Иначе конвертируем каждый math-спан региона: \( -> \[, \) -> \].
//      Замены length-preserving — позиции скана не сдвигаются, документ
//      перебирается за один проход.
QString NetworkManager::convertSoleMathSpansToDisplay(const QString &html)
{
    const QRegularExpression blockRe{
        R"(<(p|li)\b[^>]*>)", QRegularExpression::CaseInsensitiveOption};
    const QRegularExpression nestedBlockRe{
        R"(<(?:p|div)\b)", QRegularExpression::CaseInsensitiveOption};
    const QRegularExpression tagRe{R"(<[^>]*>)"};
    const int maxTail = 20; // <= этого числа не-пробельных знаков — «хвост»

    QString result = html;
    int from = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        const QRegularExpressionMatch bm = blockRe.match(result, from);
        if (!bm.hasMatch()) {
            break;
        }
        const int blockStart = bm.capturedStart(0);
        const QString blockTag = bm.captured(1).toLower();
        int closeStart = -1;
        const int blockEnd =
            balancedTagEnd(result, blockStart, blockTag, &closeStart);
        if (blockEnd <= blockStart || closeStart <= blockStart) {
            // Некорректная вложенность — пропускаем сам открывающий тег
            // (дальнейший скан может найти следующий блок).
            from = blockStart + bm.captured(0).size();
            continue;
        }
        const int tagEnd = result.indexOf(QLatin1Char('>'), blockStart) + 1;
        if (tagEnd <= 0 || tagEnd > closeStart) {
            from = blockEnd;
            continue;
        }

        // Регион: до первого вложенного <p>/<div> (spec). Сканируем от
        // tagEnd по всему хвосту; совпадение ЗА закрытием блока — не наше.
        int regionEnd = closeStart;
        const QRegularExpressionMatch nm = nestedBlockRe.match(result, tagEnd);
        if (nm.hasMatch() && nm.capturedStart(0) < closeStart) {
            regionEnd = nm.capturedStart(0);
        }

        // Все math-спаны в регионе (balanced, точный токен «math»).
        QVector<QPair<int, int>> spans; // [start, end) в координатах result
        int mf = tagEnd;
        while (mf < regionEnd) {
            const int pos = findTagByExactClass(result, mf,
                                                QStringLiteral("span"),
                                                QStringLiteral("math"));
            if (pos < 0 || pos >= regionEnd) {
                break;
            }
            int mClose = -1;
            const int mEnd =
                balancedTagEnd(result, pos, QStringLiteral("span"), &mClose);
            if (mEnd < 0 || mEnd > regionEnd) {
                break; // не закрывается внутри региона — не трогаем
            }
            spans.append({pos, mEnd});
            mf = mEnd;
        }
        if (spans.isEmpty()) {
            from = blockEnd;
            continue;
        }

        // Остаточный текст блока (без math-спанов и без тегов).
        QString leftover = result.mid(tagEnd, regionEnd - tagEnd);
        for (int i = spans.size() - 1; i >= 0; --i) {
            leftover.remove(spans[i].first - tagEnd,
                            spans[i].second - spans[i].first);
        }
        leftover.remove(tagRe);
        leftover.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
        leftover.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
        leftover.replace(QStringLiteral("&#xa0;"), QStringLiteral(" "));
        leftover.replace(QStringLiteral("&#xA0;"), QStringLiteral(" "));
        int nonSpace = 0;
        for (const QChar &c : leftover) {
            if (!c.isSpace()) {
                ++nonSpace;
            }
        }
        if (nonSpace > maxTail) {
            from = blockEnd; // текст предложения вокруг — оставляем inline
            continue;
        }

        // Конвертация: \( -> \[ и \) -> \] (length-preserving).
        for (const auto &sp : spans) {
            int s = sp.first;
            while (s < sp.second) {
                const int p1 = result.indexOf(QLatin1String("\\("), s);
                if (p1 < 0 || p1 >= sp.second) {
                    break;
                }
                result.replace(p1, 2, QLatin1String("\\["));
                s = p1 + 2;
            }
            s = sp.first;
            while (s < sp.second) {
                const int p1 = result.indexOf(QLatin1String("\\)"), s);
                if (p1 < 0 || p1 >= sp.second) {
                    break;
                }
                result.replace(p1, 2, QLatin1String("\\]"));
                s = p1 + 2;
            }
        }

        // task-13: цепочки -> переносы. splitWideMathChains меняет ДЛИНУ,
        // поэтому спаны идут ОБРАТНО (координаты предшествующих не сдвигаются).
        // Конвертация только что сделана -> спаны display: «\\» допустим.
        for (int si = spans.size() - 1; si >= 0; --si) {
            const int spFirst = spans[si].first;
            const int spSecond = spans[si].second;
            const int innerStart = result.indexOf(QLatin1Char('>'), spFirst) + 1;
            const int innerEnd = result.lastIndexOf(QLatin1Char('<'), spSecond - 1);
            if (innerStart <= 0 || innerEnd <= innerStart || innerEnd >= spSecond) {
                continue; // некорректная геометрия — не трогаем
            }
            const QString original = result.mid(innerStart, innerEnd - innerStart);
            const QString fixed = splitWideMathChains(original);
            if (fixed != original) {
                result.replace(innerStart, innerEnd - innerStart, fixed);
            }
        }
        from = blockEnd;
    }
    return result;
}

// task-11b: вырезание «одиночных» formula-блоков — см. объявление в .h.
// Тот же блок-скан и тот же порог (<= 20 не-пробельных в остаточном тексте),
// что у convertSoleMathSpansToDisplay; различие: подходящий блок НЕ
// переписывается на месте, а ВЫРЕЗАЕТСЯ (result строится по частям — html
// неизменен, координаты стабильны) и, с сконвертированными в display
// формулами, дописывается в *extracted (в порядке следования).
QString NetworkManager::extractSoleMathBlocks(const QString &html,
                                              QStringList *extracted)
{
    const QRegularExpression blockRe{
        R"(<(p|li)\b[^>]*>)", QRegularExpression::CaseInsensitiveOption};
    const QRegularExpression nestedBlockRe{
        R"(<(?:p|div)\b)", QRegularExpression::CaseInsensitiveOption};
    const QRegularExpression tagRe{R"(<[^>]*>)"};
    const int maxTail = 20;

    if (extracted) {
        extracted->clear();
    }
    QString result;
    int pos = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        const QRegularExpressionMatch bm = blockRe.match(html, pos);
        if (!bm.hasMatch()) {
            result += html.mid(pos);
            break;
        }
        const int blockStart = bm.capturedStart(0);
        const QString blockTag = bm.captured(1).toLower();
        int closeStart = -1;
        const int blockEnd =
            balancedTagEnd(html, blockStart, blockTag, &closeStart);
        if (blockEnd <= blockStart || closeStart <= blockStart
            || html.indexOf(QLatin1Char('>'), blockStart) < 0) {
            // Некорректная вложенность — копируем как есть до конца тега.
            const int copyEnd = blockStart + bm.captured(0).size();
            result += html.mid(pos, copyEnd - pos);
            pos = copyEnd;
            continue;
        }
        const int tagEnd = html.indexOf(QLatin1Char('>'), blockStart) + 1;
        if (tagEnd > closeStart) {
            result += html.mid(pos, blockEnd - pos);
            pos = blockEnd;
            continue;
        }

        // Регион: до первого вложенного <p>/<div> (spec).
        int regionEnd = closeStart;
        const QRegularExpressionMatch nm = nestedBlockRe.match(html, tagEnd);
        if (nm.hasMatch() && nm.capturedStart(0) < closeStart) {
            regionEnd = nm.capturedStart(0);
        }

        // Math-спаны региона (balanced, точный токен «math»).
        QVector<QPair<int, int>> spans; // координаты в `html`
        int mf = tagEnd;
        while (mf < regionEnd) {
            const int sp = findTagByExactClass(html, mf,
                                               QStringLiteral("span"),
                                               QStringLiteral("math"));
            if (sp < 0 || sp >= regionEnd) {
                break;
            }
            int mClose = -1;
            const int mEnd =
                balancedTagEnd(html, sp, QStringLiteral("span"), &mClose);
            if (mEnd < 0 || mEnd > regionEnd) {
                break;
            }
            spans.append({sp, mEnd});
            mf = mEnd;
        }

        result += html.mid(pos, blockStart - pos);
        if (spans.isEmpty()) {
            result += html.mid(blockStart, blockEnd - blockStart);
            pos = blockEnd;
            continue;
        }

        // Остаточный текст (без math-спанов и тегов); &nbsp; — пробел.
        QString leftover = html.mid(tagEnd, regionEnd - tagEnd);
        for (int i = spans.size() - 1; i >= 0; --i) {
            leftover.remove(spans[i].first - tagEnd,
                            spans[i].second - spans[i].first);
        }
        leftover.remove(tagRe);
        leftover.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
        leftover.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
        leftover.replace(QStringLiteral("&#xa0;"), QStringLiteral(" "));
        leftover.replace(QStringLiteral("&#xA0;"), QStringLiteral(" "));
        int nonSpace = 0;
        for (const QChar &c : leftover) {
            if (!c.isSpace()) {
                ++nonSpace;
            }
        }
        if (nonSpace > maxTail) {
            // Предложение — блок остаётся в ячейке (формулы inline).
            result += html.mid(blockStart, blockEnd - blockStart);
            pos = blockEnd;
            continue;
        }

        // «Одиночный» formula-блок: формулы -> display, блок -> *extracted.
        QString block = html.mid(blockStart, blockEnd - blockStart);
        for (const auto &sp : spans) {
            const int sEnd = sp.second - blockStart;
            int s = sp.first - blockStart;
            while (s < sEnd) {
                const int p1 = block.indexOf(QLatin1String("\\("), s);
                if (p1 < 0 || p1 >= sEnd) {
                    break;
                }
                block.replace(p1, 2, QLatin1String("\\["));
                s = p1 + 2;
            }
            s = sp.first - blockStart;
            while (s < sEnd) {
                const int p1 = block.indexOf(QLatin1String("\\)"), s);
                if (p1 < 0 || p1 >= sEnd) {
                    break;
                }
                block.replace(p1, 2, QLatin1String("\\]"));
                s = p1 + 2;
            }
        }
        if (extracted) {
            extracted->append(block);
        }
        pos = blockEnd;
    }
    return result;
}

// task-11: «хвостовой /» — типография math100.ru: формула в span.math
// заканчивается одиночным косолапым, а единица измерения стоит ВНЕ span
// (напр. <span class="math">\(\omega  = {20^ \circ }/\)</span> мин —
// рендерится «ω = 20°/» + отдельно «мин»). Правило чистки:
//   * только <span class="math"> (точный токен класса, balanced-скан);
//   * только ПОСЛЕДНЕЕ math-сегмент \(…\) внутри span (между последним
//     \( перед последним \) и этим последним \));
//   * только если формула (после trim пробелов в конце) оканчивается
//     одиночным / (двойное // не трогаем) либо \/ (экранированный слаг:
//     переносим / наружу, а служебный backslash убираем — вне формулы он
//     превратил бы результат в «\/» и оставил бы формулу на «\)»);
//   * сам / ПЕРЕНОСИМ сразу после </span> (не удаляем: смысл «20°/мин»
//     сохраняется);
//   * остальной HTML не трогаем; каждый span обрабатывается один раз
//     (скач строго вперёд — негритый матчинг).
// Точный вход/выход:
//   <span class="math">\(\omega  = {20^ \circ }/\)</span> мин
//   ->
//   <span class="math">\(\omega  = {20^ \circ }\)</span>/ мин
static QString stripTrailingMathSlash(const QString &html)
{
    QString result = html;
    int from = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        const int pos = findTagByExactClass(result, from,
                                            QStringLiteral("span"),
                                            QStringLiteral("math"));
        if (pos < 0) {
            break;
        }
        int closeStart = -1;
        const int end = balancedTagEnd(result, pos, QStringLiteral("span"),
                                       &closeStart);
        if (end < 0 || closeStart < 0 || closeStart <= pos) {
            break; // некорректная вложенность — не трогаем остаток
        }
        const int tagEnd = result.indexOf(QLatin1Char('>'), pos) + 1;
        if (tagEnd <= 0 || tagEnd >= closeStart) {
            break;
        }
        const QString inner = result.mid(tagEnd, closeStart - tagEnd);

        // Последнее math-сегмент: между последним \( до последнего \)
        // и этим последним \).
        const int lastClose = inner.lastIndexOf(QLatin1String("\\)"));
        const int lastOpen =
            lastClose >= 0 ? inner.lastIndexOf(QLatin1String("\\("), lastClose)
                           : -1;

        int cutPos = -1; // позиция среза внутри inner (exclusive)
        if (lastOpen >= 0 && lastClose > lastOpen + 2) {
            const int mathStart = lastOpen + 2;
            const QString mathText = inner.mid(mathStart, lastClose - mathStart);
            int trailingSpaces = 0;
            while (trailingSpaces < mathText.size()
                   && QChar::isSpace(
                          mathText.at(mathText.size() - 1 - trailingSpaces).unicode())) {
                ++trailingSpaces;
            }
            const int tailPos = mathText.size() - trailingSpaces;
            const QString trimmedText = mathText.left(tailPos);
            if (tailPos >= 2 && trimmedText.endsWith(QLatin1String("\\/"))) {
                // …\/ — переносим / наружу, backslash-экранирование сбрасываем
                cutPos = mathStart + tailPos - 2;
            } else if (tailPos >= 1
                       && trimmedText.at(tailPos - 1) == QLatin1Char('/')
                       && (tailPos < 2
                           || trimmedText.at(tailPos - 2) != QLatin1Char('/'))) {
                // …/ — одиночный косолапый (не //)
                cutPos = mathStart + tailPos - 1;
            }
        }

        if (cutPos > 0) {
            // Формула без хвостового слэша; / встаёт сразу после </span>.
            // replace сдвигает хвост влево на removed символов — позицию
            // </span> корректируем, иначе / встанет после пробела/в слово.
            const QString newInner =
                inner.left(cutPos) + inner.mid(lastClose);
            const int removed = (closeStart - tagEnd) - newInner.length();
            result.replace(tagEnd, closeStart - tagEnd, newInner);
            result.insert(end - removed, QLatin1Char('/'));
            from = end - removed + 1;
        } else {
            from = end;
        }
    }
    return result;
}

// task-9: содержимое div.task-answer-body внутри блока task-answer-block
// (без обёрток и лейбла) — для ячеек «Ответ»/«Решение» табличного режима.
static QString taskAnswerBodyOf(const QString &blockHtml)
{
    const int pos = findTagByExactClass(blockHtml, 0,
                                        QStringLiteral("div"),
                                        QStringLiteral("task-answer-body"));
    if (pos < 0) {
        return QString();
    }
    int closeStart = -1;
    const int end = balancedTagEnd(blockHtml, pos, QStringLiteral("div"), &closeStart);
    const int tagEnd = blockHtml.indexOf(QLatin1Char('>'), pos) + 1;
    if (tagEnd <= 0) {
        return QString();
    }
    if (closeStart >= 0 && closeStart > tagEnd) {
        return blockHtml.mid(tagEnd, closeStart - tagEnd);
    }
    if (end > tagEnd) {
        return blockHtml.mid(tagEnd, end - tagEnd);
    }
    return QString();
}

void NetworkManager::splitTaskContent(const QString &cleanHtml, QString &bodyOut,
                                      QStringList &answers, QStringList &solutions,
                                      QStringList &others)
{
    // 1. Снимаем внешний div.post-content (balanced) — оставляем inner-контент.
    QString content = cleanHtml;
    int pc = findDivByExactClass(content, 0, QStringLiteral("post-content"));
    if (pc >= 0) {
        int closeStart = -1;
        int end = balancedTagEnd(content, pc, QStringLiteral("div"), &closeStart);
        const int tagEnd = content.indexOf(QLatin1Char('>'), pc) + 1;
        if (closeStart >= 0 && closeStart > tagEnd) {
            content = content.mid(tagEnd, closeStart - tagEnd);
        } else if (end > tagEnd) {
            // Закрывающий тег не найден — берём до конца и срезаем хвостовой
            // </div> (как в extractTaskContent для spoiler-content).
            content = content.mid(tagEnd, end - tagEnd);
            content.remove(QRegularExpression{
                QStringLiteral("</div\\s*>)\\s*$"),
                QRegularExpression::CaseInsensitiveOption});
        }
    }

    // 2. Находим ВСЕ div.task-answer-block (balanced-скан, точный токен класса),
    //    запоминаем диапазоны.
    QVector<QPair<int, int>> ranges;
    int from = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        int pos = findDivByExactClass(content, from, QStringLiteral("task-answer-block"));
        if (pos < 0) {
            break;
        }
        int end = balancedTagEnd(content, pos, QStringLiteral("div"));
        if (end <= pos) {
            // Некорректная вложенность — вырезаем до конца (как yandex_rtb в
            // extractTaskContent), чтобы не зациклиться.
            ranges.append({pos, content.length()});
            break;
        }
        ranges.append({pos, end});
        from = end;
    }

    // 3. Классификация блоков по лейблу (<strong> внутри .task-answer-label):
    //    содержит «ответ» (case-insensitive) -> answers; «решение» -> solutions;
    //    иное -> others.
    const QRegularExpression labelRe{
        R"(<p[^>]*class=["'][^"']*task-answer-label[^"']*["'][^>]*>\s*<strong[^>]*>\s*([^<]*?)\s*</strong>)",
        QRegularExpression::CaseInsensitiveOption};
    for (const auto &r : ranges) {
        const QString blockHtml = content.mid(r.first, r.second - r.first);
        QString label;
        const QRegularExpressionMatch lm = labelRe.match(blockHtml);
        if (lm.hasMatch()) {
            label = lm.captured(1).trimmed();
        }
        const QString lower = label.toLower();
        if (lower.contains(QStringLiteral("ответ"))) {
            answers.append(blockHtml);
        } else if (lower.contains(QStringLiteral("решение"))) {
            solutions.append(blockHtml);
        } else {
            others.append(blockHtml);
        }
    }

    // 4. body — контент с УДАЛЁННЫМИ всеми task-answer-block (вырезаем с конца,
    //    чтобы диапазоны впереди не сдвигались).
    for (int i = ranges.size() - 1; i >= 0; --i) {
        content.remove(ranges[i].first, ranges[i].second - ranges[i].first);
    }
    bodyOut = content;
}

QString NetworkManager::buildTaskSection(int number, const QString &body,
                                         const QStringList &answers,
                                         const QStringList &solutions,
                                         const QStringList &others,
                                         bool withTask, bool withAnswer,
                                         bool withSolution)
{
    QString s = QStringLiteral("<section class=\"pdf-task\">\n")
        + QStringLiteral("<h2 class=\"task-heading\">Задача %1</h2>\n").arg(number);
    if (withTask) {
        s += body;
    }
    if (withAnswer) {
        for (const QString &block : answers) {
            s += block;
        }
    }
    if (withSolution) {
        for (const QString &block : solutions) {
            s += block;
        }
    }
    for (const QString &block : others) {   // «other»-блоки — всегда, если есть
        s += block;
    }
    s += QStringLiteral("\n</section>\n");
    return s;
}

QString NetworkManager::buildMissingSection(int number, const QString &url)
{
    return QStringLiteral(
        "<section class=\"pdf-task\">"
        "<h2 class=\"task-heading\">Задача %1</h2>"
        "<p class=\"task-missing\">Не удалось загрузить задачу (%2)</p>"
        "</section>\n")
        .arg(number)
        .arg(url.toHtmlEscaped());
}

QString NetworkManager::buildExportPageHtml(const QString &bodyHtml, bool useTable)
{
    // MathJax-конфиг — ТОЧНО как в WebEngineHost::createFullHtml
    // (webenginehost.cpp); стили блоков Ответ/Решение скопированы оттуда же.
    // task-9: useTable — табличный (краткий) режим: body 11pt + CSS таблицы.
    const QString bodyFont = useTable ? QStringLiteral("11pt") : QStringLiteral("12pt");
    const QString tableCss = useTable ? QStringLiteral(
        "        /* task-9: табличный (краткий) режим экспорта */\n"
        "        table.export-table { width: 100%; border-collapse: collapse; table-layout: auto; }\n"
        "        .export-table td, .export-table th { border: 1px solid #999; padding: 4px 6px;\n"
        "            vertical-align: top; font-size: 10.5pt; text-align: left; }\n"
        "        .export-table th { background: #eee; }\n"
        "        tr.task-row { page-break-inside: avoid; }\n"
         "        .task-num { text-align: center; width: 20px; }\n"
         "        .task-missing { color: #a33; }\n")
                                      : QString();
    // task-11: КАНОНИЧЕСКИЙ fitWideMath (используется ПОСЛОВНО — поведение
    // должно совпадать с window.fitWideMath в frontend/js/app.js).
    const QString fitWideMathJs = R"(
        window.fitWideMath = function () {
          var body = document.body;
          if (!body) return;
          var maxW = body.clientWidth;
          if (!maxW) return;
          document.querySelectorAll('mjx-container[display="true"]').forEach(function (m) {
            var w = m.scrollWidth;
            if (!w || w <= maxW + 2) {
              if (m.__fitWrap && m.__fitWrap.parentNode) {
                var pw = m.__fitWrap, pp = pw.parentNode;
                pp.insertBefore(m, pw); pp.removeChild(pw);
                m.style.transform = ''; m.style.transformOrigin = '';
                m.style.display = ''; m.style.margin = '';
                m.__fitWrap = null;
              }
              return;
            }
            var s = maxW / w;
            var h = m.offsetHeight;
            if (!m.__fitWrap) {
              var wrap = document.createElement('div');
              m.__fitWrap = wrap;
              m.parentNode.insertBefore(wrap, m);
              wrap.appendChild(m);
            }
            m.__fitWrap.style.overflow = 'hidden';
            m.__fitWrap.style.height = Math.ceil(h * s) + 'px';
            m.__fitWrap.style.textAlign = 'center';
            m.style.transform = 'scale(' + s + ')';
            m.style.transformOrigin = 'top center';
            m.style.display = 'block';
            m.style.margin = '0 auto';
          });
          window.__mathFitted = true;
        };
)";
    // task-11d: MathJax 3.2.2 зашит в бинарник (resources/mathjax, см.
    // resources.qrc) — простой <script src="qrc:/..."> в шаблоне ниже.
    // CDN (jsdelivr/unpkg/cdnjs) и loader/watchdog из task-11c удалены.
    return QString{R"(<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="utf-8">
    <title>Экспорт вариантов math100</title>
    <script>
        window.MathJax = {
            tex: {
                inlineMath: [['\\(', '\\)']],
                displayMath: [['\\[', '\\]']],
                processEscapes: true,
                processEnvironments: true
            },
            options: {
                skipHtmlTags: ['script', 'noscript', 'style', 'textarea', 'pre']
            },
            startup: {
                ready: function() {
                    MathJax.startup.defaultReady();
                    // task-11: после typeset масштабируем широкие
                    // display-формулы под ширину страницы (fitWideMath ниже).
                    MathJax.startup.promise.then(function() {
                        window.fitWideMath();
                    });
                }
            }
        };
    </script>
    <script src="qrc:/mathjax/es5/tex-mml-chtml.js" async></script>
    <script>
        // task-11: fitWideMath — канонический JS (идентичен app.js):
        // display-формулы, шире колонки, масштабируются transform: scale()
        // в обёртке фиксированной высоты. Идемпотентна (безопасна повторные
        // вызовы, в т.ч. на resize из app.js).
%3
    </script>
    <style>
        /* task-11: поля страницы — в @page (повторяются на КАЖДОЙ странице
           при печати/PDF; padding body не повторяется и убран, чтобы не было
           двойных полей). */
        body { font-family: 'Times New Roman', Times, serif; font-size: %2; line-height: 1.5; margin: 0; padding: 0; color: #111; }
        @page { size: A4; margin: 10mm 10mm 12mm 10mm; }
        /* task-11: шапка таблицы повторяется на каждой странице (2+ страницы) */
        thead { display: table-header-group; }
        .pdf-task { page-break-before: always; }
        .pdf-task:first-of-type { page-break-before: auto; }
        .task-heading { font-size: 14pt; margin: 0 0 8px; border-bottom: 2px solid #333; padding-bottom: 4px; }
        .task-missing { color: #a33; }
        /* Блоки Ответ/Решение (конвертация из спойлеров math100.ru) */
        .task-answer-block { margin: 12px 0; padding: 10px 14px; background: #f7f7f7; border: 1px solid #ddd; border-radius: 6px; }
        .task-answer-label { margin: 0 0 6px; font-size: 13pt; }
        .task-answer-body p { margin: 6px 0; }
        @media print { .task-answer-block { background: #fff; border: 1px solid #bbb; } }
%1
    </style>
</head>
<body>
)"}
        .arg(tableCss, bodyFont, fitWideMathJs)
        + bodyHtml + QStringLiteral("\n</body>\n</html>\n");
}

QString NetworkManager::buildExportDocument(const QStringList &allTaskUrls,
                                            const QList<int> &selectedIndices,
                                            bool withTask, bool withAnswer,
                                            bool withSolution, bool useTable)
{
    if (selectedIndices.isEmpty() || allTaskUrls.isEmpty()) {
        return QString();
    }

    // Валидные индексы (не <0 / не >= size), дубликаты убраны, порядок
    // возрастания.
    QList<int> indices;
    for (int idx : selectedIndices) {
        if (idx >= 0 && idx < allTaskUrls.size() && !indices.contains(idx)) {
            indices.append(idx);
        }
    }
    std::sort(indices.begin(), indices.end());
    if (indices.isEmpty()) {
        return QString();
    }

    const int total = indices.size();
    QString content;

    if (useTable) {
        // ---- task-9: КРАТКАЯ форма — все задачи в одной HTML-таблице,
        //      формулы остаются inline (без конвертации в display). ----
        const int colCount = 1
            + (withTask ? 1 : 0)
            + (withAnswer ? 1 : 0)
            + (withSolution ? 1 : 0);
        content += QStringLiteral("<table class=\"export-table\">\n<thead><tr>"
                                  "<th>№</th>");
        if (withTask) {
            content += QStringLiteral("<th>Задание</th>");
        }
        if (withAnswer) {
            content += QStringLiteral("<th>Ответ</th>");
        }
        if (withSolution) {
            content += QStringLiteral("<th>Решение</th>");
        }
        content += QStringLiteral("</tr></thead>\n<tbody>\n");

        for (int i = 0; i < total; ++i) {
            const QString url = allTaskUrls.at(indices[i]);
            // task-9: N = хвост URL («…-N»), если есть; иначе индекс+1.
            const int number = taskNumberFromUrl(url, indices[i] + 1);

            QString cleanHtml;
            if (fetchAndCacheTaskPage(url, cleanHtml)) {
                QString body;
                QStringList answers, solutions, others;
                splitTaskContent(cleanHtml, body, answers, solutions, others);

                // task-11b: «одиночные» (широкие) формулы -> display.
                // Механизм: display-формулу fitWideMath масштабирует в
                // фикс-высотной блок-обёртке (не растягивает страницу),
                // тогда как широкая INLINE-формула задаёт layout-ширину
                // колонки (mjx-math: white-space: nowrap) -> таблица
                // шире A4 -> PDF-срез. PAGE-ветка ниже делает ПОЛНУЮ
                // конвертацию (convertMathSpansToDisplay) — не дублируем.
                body = convertSoleMathSpansToDisplay(body);
                for (int k = 0; k < answers.size(); ++k) {
                    answers[k] = convertSoleMathSpansToDisplay(answers[k]);
                }
                for (int k = 0; k < solutions.size(); ++k) {
                    solutions[k] = convertSoleMathSpansToDisplay(solutions[k]);
                }
                for (int k = 0; k < others.size(); ++k) {
                    others[k] = convertSoleMathSpansToDisplay(others[k]);
                }

                content += QStringLiteral("<tr class=\"task-row\">"
                                           "<td class=\"task-num\">%1</td>")
                    .arg(number);
                if (withTask) {
                    // Краткое задание: тело + «other»-блоки (чтобы ничего
                    // не терялось, они в отдельные колонки не входят).
                    QString cell = body;
                    for (const QString &b : others) {
                        cell += b;
                    }
                    content += QStringLiteral("<td class=\"task-cell\">%1</td>")
                        .arg(cell);
                }
                if (withAnswer) {
                    // Только содержимое task-answer-body (без обёрток
                    // .task-answer-block и лейблов — заголовок колонки
                    // уже говорит, что это).
                    QStringList parts;
                    for (const QString &b : answers) {
                        parts << taskAnswerBodyOf(b);
                    }
                    content += QStringLiteral("<td class=\"task-cell\">%1</td>")
                        .arg(parts.join(QStringLiteral("<br>")));
                }
                if (withSolution) {
                    QStringList parts;
                    for (const QString &b : solutions) {
                        parts << taskAnswerBodyOf(b);
                    }
                    content += QStringLiteral("<td class=\"task-cell\">%1</td>")
                        .arg(parts.join(QStringLiteral("<br>")));
                }
                content += QStringLiteral("</tr>\n");
            } else {
                // Плейсхолдер: одна строка, colspan на все колонки.
                content += QStringLiteral(
                    "<tr class=\"task-row\">"
                    "<td class=\"task-missing\" colspan=\"%1\">"
                    "Не удалось загрузить задачу %2 (%3)</td></tr>\n")
                    .arg(colCount).arg(number).arg(url.toHtmlEscaped());
            }
            emit downloadProgress((i + 1) * 100 / total);
        }
        content += QStringLiteral("</tbody>\n</table>\n");
    } else {
        // ---- ПОЛНАЯ форма (текущий вид): одна задача = секция .pdf-task
        //      с page-break. task-9: «полная форма» = формулы на отдельных
        //      строках — внутри span.math инлайн \(…\) -> display \[…\]. ----
        for (int i = 0; i < total; ++i) {
            const QString url = allTaskUrls.at(indices[i]);
            const int number = taskNumberFromUrl(url, indices[i] + 1);

            QString cleanHtml;
            if (fetchAndCacheTaskPage(url, cleanHtml)) {
                QString body;
                QStringList answers, solutions, others;
                splitTaskContent(cleanHtml, body, answers, solutions, others);
                body = convertMathSpansToDisplay(body);
                for (int k = 0; k < answers.size(); ++k) {
                    answers[k] = convertMathSpansToDisplay(answers[k]);
                }
                for (int k = 0; k < solutions.size(); ++k) {
                    solutions[k] = convertMathSpansToDisplay(solutions[k]);
                }
                for (int k = 0; k < others.size(); ++k) {
                    others[k] = convertMathSpansToDisplay(others[k]);
                }
                content += buildTaskSection(number, body, answers, solutions, others,
                                            withTask, withAnswer, withSolution);
            } else {
                content += buildMissingSection(number, url);
            }
            emit downloadProgress((i + 1) * 100 / total);
        }
    }

    return buildExportPageHtml(content, useTable);
}
