// console_main.cpp
//
// Headless CLI-харнес Math100 PDF Generator: проверяет ЯДРО функционала
// (извлечение URL задач, инлайнинг картинок в data-URL, удаление
// спойлеров/рекламы/cookie, кэш по MD5) БЕЗ GUI — только QCoreApplication
// + QNetwork (QNetworkAccessManager, асинхронные запросы через event loop).
//
// Логика regex'ов (extractTaskUrls, паттерны img/spoiler/ads/cookie, MD5-имена)
// скопирована из эталонного /projects/math100_generator/networkmanager.cpp.
//
// task-4 (раунд 2): команда `task` выводит ТОЛЬКО текст задачи, без рекламы:
//   - extractTaskContent() делает balanced-извлечение div.post-content
//     (fallback: <main id="main">, затем вся страница) + заголовок из
//     span.entry-title;
//   - из извлечённого удаляются ВСЕ <script>/<iframe>/<noscript> и
//     yandex_rtb-блоки (раньше сохранялась ЦЕЛАЯ страница math100.ru:
//     шапка, меню, сайдбар, yandex-метрика, adsbygoogle);
//   - math100-spoiler (Ответ/Решение) ПЕРЕВОДЯТСЯ в всегда-видимые блоки
//     .task-answer-block (раньше негребедийный regex вырезал спойлер на
//     первом </div> — Ответ/Решение терялись);
//   - картинки встраиваются как data-URL (как раньше);
 //   - результат оборачивается в standalone HTML с MathJax (task-12c:
 //     КАНОНИЧЕСКИЙ CDN-фолбэк loader jsdelivr -> unpkg -> cdnjs):
 //     span.math конвертируются, и в браузере формулы рендерятся как на сайте.
//
// АДАПТАЦИИ относительно эталона (зафиксированы в отчёте):
//   1) Паттерн картинок расширен: эталон ищет только https?://math100.ru/images/...
//      но на живой странице math100.ru картинки лежат в /wp-content/uploads/...
//      Поэтому здесь матчим https?://math100.ru/<любой путь>/<ext>.
//   2) MIME type data-URL выводится из расширения файла (эталон всегда image/png).
//   3) У img с уже data-URL src вырезаются атрибуты srcset/sizes (иначе браузер
//      может взять удалённый srcset вместо встроенного src — нарушается автономность).
//   4) fixImageUrls разбит на оффлайн-часть fixImageUrlsOffline(html, map url->bytes)
//      (используется и selftest без сети, и fetch/task после загрузки картинок).
//      ЛЕГАСИ-очищение (spoiler/ads/cookie regex) в fixImageUrlsOffline оставлено:
//      в новом пайплайне task-4 оно не срабатывает (спойлеры уже сконвертированы,
//      мусор — вне post-content), но полезно как предохранитель.
//   5) Кэш: явные baseDir (консоль пишет в console/out), без in-memory QHash+mutex
//      (для CLI достаточно файлового кэша).
//   6) task-4: balanced-скан div'ов (findDivSpan) вместо regex `.*?</div>` —
//      вложенные div'ы (spoiler-title/spoiler-content) больше не ломают разбор.

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <QDateTime>
#include <QTextStream>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QStringList>

#include <algorithm>
#include <iostream>

// ---------------------------------------------------------------------------
// Константы
// ---------------------------------------------------------------------------
static const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

static const int DEFAULT_TIMEOUT_MS = 30000;

// ---------------------------------------------------------------------------
// Пути. Бинарник лежит в console/build/math100_console, поэтому корень
// консольного проекта — parent от applicationDirPath().
// ---------------------------------------------------------------------------
static QString consoleRoot()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/..");
}
static QString outDir()      { return consoleRoot() + "/out"; }
static QString testDataDir() { return consoleRoot() + "/testdata"; }

// ---------------------------------------------------------------------------
// Вывод в консоль (кириллица, UTF-8) и лог
// ---------------------------------------------------------------------------
static void say(const QString &s)
{
    std::cout << s.toUtf8().constData() << "\n";
    std::cout.flush();
}

static void logEvent(const QString &msg)
{
    const QString dir = outDir();
    QDir().mkpath(dir);
    QFile f(dir + "/run.log");
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f); // в Qt6 QTextStream по умолчанию кодирует в UTF-8
        ts << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
           << " " << msg << "\n";
        f.close();
    }
}

// ---------------------------------------------------------------------------
// Файловые утилиты
// ---------------------------------------------------------------------------
static bool writeUtf8File(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(data);
    f.close();
    return true;
}

static QString readTextFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// ---------------------------------------------------------------------------
// Кэш (MD5-имена файлов), как в эталоне: <baseDir>/<subdir>/<md5(url)>.png
// ---------------------------------------------------------------------------
static QString urlToFilename(const QString &url)
{
    QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5);
    return QString("%1.png").arg(hash.toHex());
}

static QString cacheFilePath(const QString &baseDir, const QString &url, const QString &subdir)
{
    return baseDir + "/" + subdir + "/" + urlToFilename(url);
}

static bool saveToCache(const QString &baseDir, const QString &url,
                        const QString &subdir, const QByteArray &data)
{
    const QString path = cacheFilePath(baseDir, url, subdir);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(data);
    f.close();
    return true;
}

static bool loadFromCache(const QString &baseDir, const QString &url,
                          const QString &subdir, QByteArray &data)
{
    const QString path = cacheFilePath(baseDir, url, subdir);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    data = f.readAll();
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// extractTaskUrls — копия логики из networkmanager.cpp (task-10/worker-a:
// новый основной паттерн + старые fallback'и).
//
// task-10: ОСНОВНОЙ паттерн — якорь на math100.ru с ТЕКСТОМ «Задача N»
// (проверен python-референсом на живых страницах):
//   href=["'](https?://math100\.ru/[^"']+)["'][^>]*>\s*Задача\s*(\d+)\s*<
// Работает И для проф-вариантов (ege_profil_*), И для базовых (baza_*):
// на базовой странице чужие якоря ege_profil_* с пустым/иным текстом больше
// не блокируют остальные задачи (баг: старый паттерн «ege_profil по href»
// находил 1 чужой якорь и блокировал fallback'и → «1 из 18»).
// Дедупликация по URL (первое вхождение), сортировка по N (stable: при равных
// N — порядок появления в HTML).
// Если основной паттерн не нашёл ничего — старые fallback'и как есть:
//   F1: href содержит "ege_profil"; F2: текст «Задача|ege»; F3: шаблон.
// ---------------------------------------------------------------------------
static QStringList extractTaskUrls(const QString &html, const QString &baseUrl)
{
    Q_UNUSED(baseUrl);
    QStringList urls;

    // Основной паттерн: якорь math100.ru с текстом «Задача N»
    {
        QRegularExpression taskRef{
            R"(href=["'](https?://math100\.ru/[^"']+)["'][^>]*>\s*Задача\s*(\d+)\s*<)",
            QRegularExpression::CaseInsensitiveOption};
        struct Pending { QString url; int n; };
        QVector<Pending> found;
        QSet<QString> seen;
        auto it = taskRef.globalMatch(html);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            const QString u = m.captured(1);
            if (seen.contains(u))
                continue; // дедупликация по URL: первое вхождение
            seen.insert(u);
            found.append(Pending{u, m.captured(2).toInt()});
        }
        // Сортировка по номеру задачи N (stable — при равных N сохраняем
        // порядок появления в HTML)
        std::stable_sort(found.begin(), found.end(),
                         [](const Pending &a, const Pending &b) { return a.n < b.n; });
        for (const Pending &p : found)
            urls.append(p.url);
    }

    // Fallback F1 (старый паттерн 1): ссылки с "ege_profil" в URL
    if (urls.isEmpty()) {
        QRegularExpression taskPattern{R"(href=["'](https?://math100\.ru/ege_profil_[^"']+)["'])"};
        QRegularExpressionMatchIterator it = taskPattern.globalMatch(html);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            const QString fullUrl = match.captured(1);
            if (!urls.contains(fullUrl)) {
                urls.append(fullUrl);
            }
        }
    }

    // Fallback F2 (старый паттерн 2): ссылки с текстом "Задача" рядом
    if (urls.isEmpty()) {
        QRegularExpression linkPattern{R"(href=["'](https?://math100\.ru/[^"']+)["'][^>]*>([^<]*(?:Задача|ege)[^<]*)<)"};
        QRegularExpressionMatchIterator linkIt = linkPattern.globalMatch(html);
        while (linkIt.hasNext()) {
            QRegularExpressionMatch linkMatch = linkIt.next();
            const QString href = linkMatch.captured(1);
            const QString text = linkMatch.captured(2);
            if (href.contains("math100.ru") && (text.contains("Задача") || href.contains("ege"))) {
                if (!urls.contains(href)) {
                    urls.append(href);
                }
            }
        }
    }

    // Fallback F3 (старый паттерн 3): генерация по шаблону
    if (urls.isEmpty()) {
        QRegularExpression variantPattern{R"(prof-ege-\d+-\d+-(?:\d+)/)"};
        if (variantPattern.match(html).hasMatch()) {
            for (int i = 1; i <= 34; ++i) {
                urls.append(QString("https://math100.ru/ege_profil_8_1-%1/").arg(i));
            }
        }
    }

    return urls;
}

// ---------------------------------------------------------------------------
// MIME type по расширению
// ---------------------------------------------------------------------------
static QString mimeFromUrl(const QString &url)
{
    const QString lower = url.toLower();
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
    if (lower.endsWith(".gif"))  return "image/gif";
    if (lower.endsWith(".webp")) return "image/webp";
    return "image/png";
}

// ---------------------------------------------------------------------------
// Собираем уникальные URL картинок math100.ru (с расширением) из HTML.
// Адаптация #1: матчим не только /images/, а любой путь на math100.ru.
// ---------------------------------------------------------------------------
static QVector<QString> collectImageUrls(const QString &html)
{
    QRegularExpression imgPattern{
        R"(<img\b[^>]*?\ssrc=["'](https?://math100\.ru/[^"']+\.(?:png|jpg|jpeg|gif|webp))["'])",
        QRegularExpression::CaseInsensitiveOption};
    QSet<QString> seen;
    QVector<QString> urls;
    auto it = imgPattern.globalMatch(html);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const QString u = m.captured(1);
        if (!seen.contains(u)) {
            seen.insert(u);
            urls.append(u);
        }
    }
    return urls;
}

// ---------------------------------------------------------------------------
// Вырезаем srcset/sizes у <img>, у которых src уже data-URL (адаптация #3)
// ---------------------------------------------------------------------------
static QString stripSrcsetFromDataImgs(const QString &html)
{
    QRegularExpression imgTagPattern{R"(<img\b[^>]*>)"};
    QRegularExpression srcsetAttr{"\\s+srcset=\"[^\"]*\""};
    QRegularExpression sizesAttr{"\\s+sizes=\"[^\"]*\""};

    QString result;
    int last = 0;
    auto it = imgTagPattern.globalMatch(html);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const int start = m.capturedStart();
        result += html.mid(last, start - last);
        QString tag = m.captured(0);
        if (tag.contains("src=\"data:") || tag.contains("src='data:")) {
            tag.replace(srcsetAttr, "");
            tag.replace(sizesAttr, "");
        }
        result += tag;
        last = m.capturedEnd();
    }
    result += html.mid(last);
    return result;
}

// ---------------------------------------------------------------------------
// ОФФЛАЙН-часть fixImageUrls: замена src на data-URL по карте url->bytes,
// затем легаси-удаление спойлеров/рекламы/cookie (паттерны как в эталоне).
// Сеть здесь НЕ используется.
// Примечание task-4: в новом пайплайне (extractTaskContent перед вызовом)
// спойлеров/рекламы в контенте уже нет — эти паттерны остаются предохранителем.
// ---------------------------------------------------------------------------
static QString fixImageUrlsOffline(const QString &html,
                                   const QHash<QString, QByteArray> &imageBytes,
                                   int *embeddedCount = nullptr)
{
    QString result = html;
    int embedded = 0;

    // 1) Заменяем src картинок на data-URL (из переданной карты)
    QRegularExpression imgPattern{
        R"(<img\b[^>]*?\ssrc=["'](https?://math100\.ru/[^"']+\.(?:png|jpg|jpeg|gif|webp))["'])",
        QRegularExpression::CaseInsensitiveOption};
    QSet<QString> done;
    auto it = imgPattern.globalMatch(html);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const QString url = m.captured(1);
        if (done.contains(url) || !imageBytes.contains(url))
            continue;
        done.insert(url);
        const QByteArray imgData = imageBytes[url];
        const QString dataUrl =
            QString("data:%1;base64,%2").arg(mimeFromUrl(url),
                                             QString::fromLatin1(imgData.toBase64()));
        result.replace("src=\"" + url + "\"", "src=\"" + dataUrl + "\"");
        result.replace("src='" + url + "'", "src='" + dataUrl + "'");
        ++embedded;
    }

    // 2) Полная автономность: убираем srcset/sizes у data-URL картинок
    result = stripSrcsetFromDataImgs(result);

    // 3) Удаляем спойлеры (ЛЕГАСИ, как в эталоне; в новом пайплайне уже сконвертированы)
    QRegularExpression spoilerPattern{
        R"(<div[^>]*class=["'][^"']*?math100-spoiler[^"']*?["'][^>]*>.*?<\/div>)",
        QRegularExpression::DotMatchesEverythingOption};
    result.replace(spoilerPattern, "");

    // 4) Удаляем рекламу (ЛЕГАСИ, как в эталоне)
    QRegularExpression adsPattern{
        R"(<(div|script|iframe|a)[^>]*(?:adsbygoogle|math100-promo-bar|promo)[^>]*>.*?<\/\1>)",
        QRegularExpression::DotMatchesEverythingOption};
    result.replace(adsPattern, "");

    // 5) Удаляем cookie banner (ЛЕГАСИ, как в эталоне)
    QRegularExpression cookiePattern{
        R"(<div[^>]*class=["'][^"']*?cookie[^"']*?["'][^>]*>.*?<\/div>)",
        QRegularExpression::DotMatchesEverythingOption};
    result.replace(cookiePattern, "");

    if (embeddedCount)
        *embeddedCount = embedded;
    return result;
}

// ---------------------------------------------------------------------------
// task-4: balanced-скан div'ов.
// openTagStart — индекс '<' ОТКРЫВАЮЩЕГО тега <div ...>.
// Результат:
//   openTagEnd — индекс сразу после '>' открывающего тега;
//   innerEnd   — индекс НАЧАЛА закрывающего </div> (внутренний контент режем
//                до начала закрывающего тега, не после);
//   fullEnd    — индекс сразу ПОСЛЕ закрывающего тега.
// Возвращает false, если div не сбалансирован.
// ---------------------------------------------------------------------------
static bool findDivSpan(const QString &html, int openTagStart,
                        int &openTagEnd, int &innerEnd, int &fullEnd)
{
    static const QRegularExpression divTag{
        R"(<div\b|</div\s*>)", QRegularExpression::CaseInsensitiveOption};
    int depth = 0;
    bool openTagSeen = false;
    auto it = divTag.globalMatch(html, openTagStart);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const bool closing = m.captured(0).startsWith("</");
        depth += closing ? -1 : 1;
        if (!closing && !openTagSeen) {
            openTagEnd = m.capturedEnd();
            openTagSeen = true;
        }
        if (depth == 0) {
            innerEnd = m.capturedStart();
            fullEnd = m.capturedEnd();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// task-4: конвертация math100-spoiler в всегда-видимый блок.
// Пока есть div, в class-списке которого точный токен "math100-spoiler"
// (НЕ math100-spoiler-title/-content/-arrow — проверяем точный токен),
// balanced-выделяем его и заменяем на:
//   <div class="task-answer-block"><p class="task-answer-label"><strong>TITLE</strong></p>
//   <div class="task-answer-body">CONTENT</div></div>
// TITLE  — текст span.math100-spoiler-title-text (fallback «Спойлер»).
// CONTENT— внутренний HTML div.math100-spoiler-content (balanced; без его
//           style="display:none" — берём только содержимое).
// ---------------------------------------------------------------------------
static QString convertSpoilers(const QString &html, int *countOut)
{
    int count = 0;
    QString result = html;

    QRegularExpression divOpen{R"(<div\b[^>]*>)", QRegularExpression::CaseInsensitiveOption};
    QRegularExpression classAttr{R"(class\s*=\s*["']([^"']*)["'])",
                                 QRegularExpression::CaseInsensitiveOption};
    QRegularExpression titleRe{
        R"(<span[^>]*class\s*=\s*["'][^"']*math100-spoiler-title-text[^"']*["'][^>]*>([^<]*)</span>)",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption};
    QRegularExpression contentOpen{
        R"(<div\b[^>]*class\s*=\s*["'][^"']*math100-spoiler-content[^"']*["'][^>]*>)",
        QRegularExpression::CaseInsensitiveOption};

    int guard = 0; // защита от зацикливания на сломанном HTML
    while (guard++ < 100) {
        // 1) Найти ближайший div с ТОЧНЫМ токеном math100-spoiler в class
        int spoilerStart = -1;
        auto it = divOpen.globalMatch(result);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QRegularExpressionMatch cm = classAttr.match(m.captured(0));
            if (!cm.hasMatch())
                continue;
            const QStringList tokens =
                cm.captured(1).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (tokens.contains("math100-spoiler")) {
                spoilerStart = m.capturedStart();
                break;
            }
        }
        if (spoilerStart < 0)
            break;

        // 2) Balanced-выделение всего спойлера (вложенные title/content div'ы внутри)
        int openTagEnd = -1, innerEnd = -1, fullEnd = -1;
        if (!findDivSpan(result, spoilerStart, openTagEnd, innerEnd, fullEnd))
            break; // не сбалансирован — не трогаем (не роняем)
        const QString block = result.mid(spoilerStart, fullEnd - spoilerStart);

        // 3) Заголовок спойлера (Ответ/Решение/...)
        QString title = QString::fromUtf8("Спойлер");
        const QRegularExpressionMatch tm = titleRe.match(block);
        if (tm.hasMatch()) {
            const QString t = tm.captured(1).trimmed();
            if (!t.isEmpty())
                title = t;
        }

        // 4) Внутренний HTML content-div'а (без его style-атрибута)
        QString content;
        const QRegularExpressionMatch cm = contentOpen.match(block);
        if (cm.hasMatch()) {
            int coEnd = -1, ciEnd = -1, cFullEnd = -1;
            if (findDivSpan(block, cm.capturedStart(), coEnd, ciEnd, cFullEnd))
                content = block.mid(cm.capturedEnd(), ciEnd - cm.capturedEnd());
        }

        // 5) Замена на видимый блок
        const QString repl =
            QString("<div class=\"task-answer-block\">"
                    "<p class=\"task-answer-label\"><strong>%1</strong></p>"
                    "<div class=\"task-answer-body\">%2</div></div>")
                .arg(title.toHtmlEscaped(), content);
        result.replace(spoilerStart, fullEnd - spoilerStart, repl);
        ++count;
    }

    if (countOut)
        *countOut = count;
    return result;
}

// ---------------------------------------------------------------------------
// task-12: хвостовой «/» в math-формуле — типография сайта (не наш баг):
//   <span class="math">\(\omega  = {20^ \circ }/\)</span> мин  → рендер «20°/»
// Правило (то же внедряет worker-a в networkmanager::extractTaskContent):
// если LaTeX внутри math-спана (между \( и последним \)) оканчивается
// (после trim) ОДИНОЧНЫМ «/» (или экранированным «\/») — ПЕРЕНОСИМ этот
// «/» наружу, сразу после </span> (НЕ удаляем — смысл «20°/мин» сохраняется):
//   ВХОД:  <span class="math">\(\omega  = {20^ \circ }/\)</span> мин
//   ВЫХОД: <span class="math">\(\omega  = {20^ \circ }\)</span>/ мин
// Двойной хвост «//» (не одиночный слеш) и формулы без слеша не трогаем.
// ---------------------------------------------------------------------------
static QString fixTrailingSlashInMathSpans(const QString &html)
{
    // g1: открывающий тег span с class-токеном "math"; g2: «\(";
    // g3: LaTeX; g4: «\)» + </span> (нежадно — до ПЕРВОГО «\)» перед </span>,
    // для корректной формулы это и есть её конец)
    QRegularExpression spanRe{
        R"((<span\b[^>]*class\s*=\s*["'][^"']*math[^"']*["'][^>]*>)(\\\()([\s\S]*?)(\\\)</span>))",
        QRegularExpression::CaseInsensitiveOption};

    QString out;
    int last = 0;
    auto it = spanRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString latex = m.captured(3);
        const QString trimmed = latex.trimmed();

        int slashLen = 0;
        if (trimmed.endsWith("\\/")) {
            slashLen = 2; // экранированный слеш в LaTeX
        } else if (trimmed.endsWith("/")) {
            slashLen = 1;
        }
        if (slashLen == 0)
            continue; // хвостового слеша нет — не трогаем

        // «ОДИНОЧНЫЙ»: перед слешем (после trim) нет ещё одного слеша
        const QString before = trimmed.left(trimmed.size() - slashLen).trimmed();
        if (before.endsWith("/"))
            continue;

        // Срезаем хвост: сначала пробельные символы, затем сам слеш
        int cut = latex.size();
        while (cut > 0) {
            const QChar c = latex.at(cut - 1);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                --cut;
            else
                break;
        }
        cut -= slashLen;

        out += html.mid(last, m.capturedStart() - last);
        out += m.captured(1) + m.captured(2) + latex.left(cut) + m.captured(4);
        out += "/"; // «/» перенесён наружу, сразу после </span>
        last = m.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

// ---------------------------------------------------------------------------
// task-4: извлечение ТОЛЬКО контента задачи из полной страницы math100.ru.
//   a) div.post-content (balanced-скан); fallback: <main id="main">; затем вся html.
//   c) Удаление ВСЕХ <script>/<iframe>/<noscript> и yandex_rtb-блоков.
//   d) Конвертация спойлеров в видимые .task-answer-block (Ответ/Решение сохраняются).
//   e) titleOut — текст span.entry-title (trim).
//   f) task-12: хвостовой одиночный «/» в math-спане переносится наружу
//      сразу после </span> (fixTrailingSlashInMathSpans).
// spoilersConverted — число превращённых spoiler-блоков (если передано).
// ---------------------------------------------------------------------------
static QString extractTaskContent(const QString &html,
                                  QString *titleOut = nullptr,
                                  int *spoilersConverted = nullptr)
{
    if (titleOut)
        *titleOut = {};
    if (spoilersConverted)
        *spoilersConverted = 0;

    // (e) Заголовок: span.entry-title (в живой странице он ВЫШЕ post-content,
    //     поэтому ищем по всей странице; <h1 class="entry-title"> не матчится —
    //     нужен именно <span>).
    if (titleOut) {
        QRegularExpression titleRe{
            R"(<span[^>]*class\s*=\s*["']entry-title[^"']*["'][^>]*>([^<]+)</span>)",
            QRegularExpression::CaseInsensitiveOption};
        const QRegularExpressionMatch m = titleRe.match(html);
        if (m.hasMatch())
            *titleOut = m.captured(1).trimmed();
    }

    // (a)+(b) Balanced-извлечение div.post-content
    QString content;
    {
        QRegularExpression postRe{
            R"(<div\b[^>]*class\s*=\s*["'][^"']*post-content[^"']*["'][^>]*>)",
            QRegularExpression::CaseInsensitiveOption};
        const QRegularExpressionMatch pm = postRe.match(html);
        if (pm.hasMatch()) {
            int openTagEnd = -1, innerEnd = -1, fullEnd = -1;
            if (findDivSpan(html, pm.capturedStart(), openTagEnd, innerEnd, fullEnd))
                content = html.mid(pm.capturedStart(), fullEnd - pm.capturedStart());
        }
    }

    // Fallback 1: <main id="main"> ... </main>
    if (content.isEmpty()) {
        QRegularExpression mainRe{
            R"(<main\b[^>]*id\s*=\s*["']main["'][^>]*>)",
            QRegularExpression::CaseInsensitiveOption};
        const QRegularExpressionMatch mm = mainRe.match(html);
        if (mm.hasMatch()) {
            QRegularExpression closeMain{R"(<\/main\s*>)", QRegularExpression::CaseInsensitiveOption};
            const QRegularExpressionMatch cm = closeMain.match(html, mm.capturedEnd());
            if (cm.hasMatch())
                content = html.mid(mm.capturedStart(), cm.capturedEnd() - mm.capturedStart());
        }
    }

    // Fallback 2: не роняем — вся страница
    if (content.isEmpty())
        content = html;

    // (c) Чистка: ВСЕ script/iframe/noscript (case-insensitive, DOTALL)
    {
        QRegularExpression scriptRe{
            R"(<script\b[^>]*>.*?<\/script\s*>)",
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption};
        content.replace(scriptRe, "");

        QRegularExpression iframeRe{
            R"(<iframe\b[^>]*>.*?<\/iframe\s*>)",
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption};
        content.replace(iframeRe, "");

        QRegularExpression noscriptRe{
            R"(<noscript\b[^>]*>.*?<\/noscript\s*>)",
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption};
        content.replace(noscriptRe, "");

        QRegularExpression yandexRe{
            R"(<div[^>]*yandex_rtb[^>]*>.*?<\/div>)",
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption};
        content.replace(yandexRe, "");
    }

    // (d) Спойлеры → видимые блоки (Ответ/Решение остаются)
    QString converted = convertSpoilers(content, spoilersConverted);

    // (f) task-12: хвостовой одиночный «/» в формуле — наружу span.math
    return fixTrailingSlashInMathSpans(converted);
}

// ---------------------------------------------------------------------------
// task-4: standalone HTML-обёртка для браузера:
//   - <title> = titleOut (fallback «Задача»);
//   - inline <style> (Times New Roman, блоки .task-answer-block,
//     @page A4 15mm для браузерной печати — task-12);
 //   - MathJax 3 (task-12d: ОДИН простой CDN-тег jsdelivr — standalone
 //     HTML открывается во внешнем браузере, qrc недоступен)
 //     + конфиг (inlineMath \( \), displayMath \[ \], processEscapes);
 //     typeset отложен — запускаем сами ПОСЛЕ конвертации
//     span.math, чтобы не было гонки и двойной обработки;
//   - КАНОНИЧЕСКИЙ fitWideMath (task-12, пословно как в app.js и
//     экспорт-документах): автовызов ПОСЛЕ MathJax typeset
//     (typesetPromise().then — подогнан под нашу tryTypeset-структуру)
//     + повтор по resize (debounce ~200ms) — широкие display-формулы
//     не вылезают за правый край при печати/просмотре;
//   - inline-скрипт: span.math → div с \(..\) (inline) / \[..\] (display,
//     если родитель P/DIV/BLOCKQUOTE), с декодированием базовых HTML-сущностей.
//     Если текст span.math УЖЕ окружён \(..\) / \[..\] (так отдаёт math100.ru),
//     дублировать делимитаторы НЕ добавляем — иначе MathJax ломал бы формулу.
// ---------------------------------------------------------------------------
static QString wrapStandaloneHtml(const QString &title, const QString &contentHtml)
{
    QString t = title.trimmed();
    if (t.isEmpty())
        t = QString::fromUtf8("Задача");
    const QString safeTitle = t.toHtmlEscaped();

    return QString(R"HTMLDOC(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<title>)HTMLDOC" + safeTitle + QString(R"HTMLDOC(</title>
<style>
 body { font-family: 'Times New Roman', Times, serif; font-size: 14pt; line-height: 1.5; max-width: 800px; margin: 24px auto; padding: 0 16px; color: #111; }
 .task-answer-block { margin: 12px 0; padding: 10px 14px; background: #f7f7f7; border: 1px solid #ddd; border-radius: 6px; }
 .task-answer-label { margin: 0 0 6px; font-size: 13pt; }
 .task-answer-body p { margin: 6px 0; }
 img { max-width: 100%; }
 /* task-12: поля для браузерной печати (Ctrl+P) — на всех страницах */
 @page { size: A4; margin: 15mm; }
 </style>
 <script>
 window.MathJax = {
   tex: {
     inlineMath: [['\\(', '\\)']],
     displayMath: [['\\[', '\\]']],
     processEscapes: true
   },
   options: { enableMenu: false },
   startup: { typeset: false }
 };
 </script>
 <script>
 // task-12: КАНОНИЧЕСКИЙ fitWideMath (пословно как в app.js и экспорт-документах)
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
 </script>
    <!-- task-12d: standalone HTML открывается во ВНЕШНЕМ браузере — qrc
         там недоступен, поэтому один простой CDN-тег jsdelivr (как до
         task-12c). В самом приложении MathJax локальный (qrc). -->
    <script src="https://cdn.jsdelivr.net/npm/mathjax@3/es5/tex-mml-chtml.js" async></script>
</head>
<body>
)HTMLDOC")
        + contentHtml
        + QString(R"HTMLDOC(
<script>
(function () {
    function decodeEntities(s) {
        return String(s)
            .replace(/&amp;/g, '&')
            .replace(/&lt;/g, '<')
            .replace(/&gt;/g, '>')
            .replace(/&quot;/g, '"')
            .replace(/&#0*39;/g, "'")
            .replace(/&#8211;/g, '\u2013')
            .replace(/&#8212;/g, '\u2014')
            .replace(/&#8230;/g, '\u2026')
            .replace(/&#(\d+);/g, function (_, n) { return String.fromCharCode(parseInt(n, 10)); });
    }
    function isBlockParent(el) {
        var p = el ? el.parentElement : null;
        if (!p) return false;
        var tag = p.tagName.toUpperCase();
        return tag === 'P' || tag === 'DIV' || tag === 'BLOCKQUOTE';
    }
    function convertMathSpans() {
        var spans = Array.prototype.slice.call(document.querySelectorAll('span.math'));
        spans.forEach(function (sp) {
            var text = decodeEntities(sp.textContent).trim();
            var alreadyDelimited = /^\\\(/.test(text) || /^\\\[/.test(text);
            var display = isBlockParent(sp);
            var rendered;
            if (alreadyDelimited) {
                rendered = text;
            } else {
                rendered = (display ? '\\[' : '\\(') + text + (display ? '\\]' : '\\)');
            }
            var d = document.createElement('div');
            d.className = 'math-converted' + (display ? ' math-display' : '');
            d.textContent = rendered;
            sp.parentNode.replaceChild(d, sp);
        });
    }
     function tryTypeset(tries) {
         if (window.MathJax && typeof MathJax.typesetPromise === 'function') {
             MathJax.typesetPromise().then(function () {
                 // task-12: после typeset — подгонка широких display-формул
                 // (автовызов; аналог MathJax.startup.promise.then, подогнанный
                 // под нашу отложенную tryTypeset-структуру)
                 if (window.fitWideMath) {
                     window.fitWideMath();
                 }
             }).catch(function (e) {
                 console.error('MathJax typeset error: ' + e);
             });
             return;
         }
         if (tries < 150) {
             setTimeout(function () { tryTypeset(tries + 1); }, 100);
         }
     }
     // task-12: повторная подгонка при изменении размера окна (debounce ~200ms;
     // fitWideMath идемпотентна)
     var fitResizeTimer = null;
     window.addEventListener('resize', function () {
         if (fitResizeTimer) {
             clearTimeout(fitResizeTimer);
         }
         fitResizeTimer = setTimeout(function () {
             fitResizeTimer = null;
             if (window.fitWideMath) {
                 window.fitWideMath();
             }
         }, 200);
     });
     document.addEventListener('DOMContentLoaded', function () {
         convertMathSpans();
         tryTypeset(0);
     });
})();
</script>
</body>
</html>
)HTMLDOC"));
}

// ---------------------------------------------------------------------------
// Синхронный GET с таймаутом через локальный event loop.
// ---------------------------------------------------------------------------
struct FetchResult {
    bool ok = false;
    int httpStatus = 0;
    QByteArray data;
    QString error;
};

static FetchResult fetchUrl(QNetworkAccessManager &nam, const QString &url,
                            int timeoutMs = DEFAULT_TIMEOUT_MS)
{
    FetchResult r;
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", USER_AGENT);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    bool timedOut = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        timedOut = true;
        if (reply && !reply->isFinished())
            reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    timer.stop();

    r.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError err = reply->error();

    if (err == QNetworkReply::OperationCanceledError) {
        r.error = timedOut
            ? QString("таймаут (> %1 с): %2").arg(timeoutMs / 1000).arg(url)
            : QString("запрос отменён: %1").arg(url);
    } else if (err != QNetworkReply::NoError) {
        r.error = QString("сеть: %1 (%2)").arg(reply->errorString(), url);
    } else if (r.httpStatus < 200 || r.httpStatus >= 300) {
        r.error = QString("HTTP %1: %2").arg(r.httpStatus).arg(url);
    } else {
        r.data = reply->readAll();
        r.ok = true;
    }
    reply->deleteLater();
    return r;
}

// ---------------------------------------------------------------------------
// Поиск каталога testdata (устойчиво к месту запуска)
// ---------------------------------------------------------------------------
static QString findTestDataDir()
{
    QStringList candidates;
    candidates << QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../testdata");
    candidates << QDir::cleanPath(QCoreApplication::applicationDirPath() + "/testdata");
    candidates << QDir::cleanPath(QDir::currentPath() + "/testdata");
    candidates << QDir::cleanPath(QDir::currentPath() + "/console/testdata");
    for (const QString &c : candidates) {
        if (QDir(c).exists() && QFile::exists(c + "/variant_sample.html"))
            return c;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Команда: selftest (ОФЛАЙН, без сети)
// ---------------------------------------------------------------------------
static int cmdSelftest()
{
    struct Test { QString name; bool pass; QString detail; };
    QVector<Test> tests;
    auto add = [&](const QString &name, bool pass, const QString &detail) {
        tests.append({name, pass, detail});
    };

    // Валидный 1x1 прозрачный PNG для оффлайн-встраивания картинок
    static const QByteArray png1x1 = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4"
        "2mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");

    const QString td = findTestDataDir();
    if (td.isEmpty()) {
        add("testdata", false, "каталог testdata с fixture не найден");
        // распечатаем ниже и выйдем
    } else {
        // (a) extractTaskUrls (task-10): основной паттерн «Задача N» по тексту
        // якоря + дедупликация + сортировка по N; cap(30) УБРАН — ВСЕ задачи
        // (в fixture: 33 ege_profil + 3 базовых baza_2025_17_1-1..3 = 36;
        //  чужой якорь ege_profil_5_1-1 БЕЗ текста «Задача N» не попадает).
        {
            const QString html = readTextFile(td + "/variant_sample.html");
            if (html.isEmpty()) {
                add("extractTaskUrls", false, "не удалось прочитать variant_sample.html");
            } else {
                const QStringList urls = extractTaskUrls(html, "https://math100.ru/prof-ege-2027-10-1/");
                QSet<QString> seenSet;
                for (const QString &u : urls)
                    seenSet.insert(u);
                const bool dedup   = (seenSet.size() == urls.size());
                const bool countOk = (urls.size() == 36); // 33 ege_profil + 3 baza, кап убран
                const bool firstOk = !urls.isEmpty() && urls.first() == "https://math100.ru/ege_profil_10_1-1/";
                const bool lastOk  = urls.size() >= 2 && urls.last() == "https://math100.ru/ege_profil_10_1-33/";
                // Базовые ссылки baza_2025_17_1-N найдены
                bool bazaFound = true;
                for (int n = 1; n <= 3; ++n) {
                    if (!urls.contains(QString("https://math100.ru/baza_2025_17_1-%1/").arg(n)))
                        bazaFound = false;
                }
                // Чужой якорь БЕЗ текста «Задача N» НЕ найден как задача
                const bool noForeign = !urls.contains("https://math100.ru/ege_profil_5_1-1/");
                const bool noDistractor = !urls.contains("https://math100.ru/about/")
                                         && !urls.contains("https://math100.ru/");
                // Сортировка по N: номера задач неубывающие
                // (номер = хвост URL после последнего '-', напр. .../ege_profil_10_1-7/ -> 7)
                bool sorted = true;
                int prevN = 0;
                for (const QString &u : urls) {
                    const int dash = u.lastIndexOf('-');
                    const QString tail = (dash >= 0 ? u.mid(dash + 1) : QString()).section('/', 0, 0);
                    bool numOk = false;
                    const int n = tail.toInt(&numOk);
                    if (!numOk || n < prevN) {
                        sorted = false;
                        break;
                    }
                    prevN = n;
                }
                const bool pass = dedup && countOk && firstOk && lastOk && bazaFound
                                && noForeign && noDistractor && sorted;
                add("extractTaskUrls", pass,
                    QString("уникальных=%1 (ожидалось 36, кап убран), дедуп=%2, первый=%3, последний=%4, "
                            "baza_1..3=%5, чужой ege_profil_5_1-1=%6, дистракторы=%7, сортировка-по-N=%8")
                        .arg(urls.size())
                        .arg(dedup ? "ок" : "СБОЙ")
                        .arg(firstOk ? "ок" : "СБОЙ")
                        .arg(lastOk ? "ок" : "СБОЙ")
                        .arg(bazaFound ? "найдены" : "НЕ НАЙДЕНЫ")
                        .arg(noForeign ? "отсутствует" : "НАШЁЛСЯ")
                        .arg(noDistractor ? "отсутствуют" : "НАШЛИСЬ")
                        .arg(sorted ? "ок" : "СБОЙ"));
            }
        }

        // (a2) extractTaskUrls: fallback F1 (href ege_profil) срабатывает,
        // когда текста «Задача N» нет (старый путь сохранён)
        {
            const QString html =
                "<html><body>"
                "<a href=\"https://math100.ru/ege_profil_7_2-1/\">Ссылка 1</a>"
                "<a href=\"https://math100.ru/ege_profil_7_2-2/\">Ссылка 2</a>"
                "<a href=\"https://math100.ru/ege_profil_7_2-1/\">Дубликат ссылки</a>"
                "<a href=\"https://math100.ru/about/\">О сайте</a>"
                "</body></html>";
            const QStringList urls = extractTaskUrls(html, "https://math100.ru/prof-ege-2027-7-2/");
            const bool pass = (urls.size() == 2)
                && urls.contains("https://math100.ru/ege_profil_7_2-1/")
                && urls.contains("https://math100.ru/ege_profil_7_2-2/")
                && !urls.contains("https://math100.ru/about/");
            add("extractTaskUrls(fallback-F1)", pass,
                QString("найдено=%1 (ожидалось 2: ege_profil по href, дедуп)").arg(urls.size()));
        }

        // (b1) collectImageUrls: дедупликация URL картинок
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("collectImageUrls", false, "не удалось прочитать task_sample.html");
            } else {
                const QVector<QString> iu = collectImageUrls(html);
                const bool pass = (iu.size() == 3); // 4 тега <img>, 3 уникальных URL
                add("collectImageUrls(дедуп)", pass,
                    QString("уникальных URL картинок=%1 (ожидалось 3)").arg(iu.size()));
            }
        }

        // (b2) ЛЕГАСИ fixImageUrls оффлайн по ВЕСЬМУ fixture:
        // data-URL + удаление спойлеров/рекламы/cookie старыми паттернами
        // (сохранено из раунда 1 — старые тесты не тронуты)
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("fixImageUrls(офлайн)", false, "не удалось прочитать task_sample.html");
            } else {
                QHash<QString, QByteArray> imgs;
                imgs["https://math100.ru/images/task10_1-1.png"] = png1x1;
                imgs["https://math100.ru/images/task10_1-2.png"] = png1x1;
                imgs["https://math100.ru/images/figure.png"]     = png1x1;

                int embedded = 0;
                const QString fixed = fixImageUrlsOffline(html, imgs, &embedded);

                const bool noRemoteImg   = !fixed.contains("src=\"https://math100.ru/images/");
                const bool hasData       = fixed.contains("data:image/png;base64,");
                const bool countOk       = (embedded == 3);
                const bool spoilerGone   = !fixed.contains("СПОЙЛЕР-СЕКРЕТ");
                const bool adsGone       = !fixed.contains("ADS-СЕКРЕТ");
                const bool cookieGone    = !fixed.contains("COOKIE-СЕКРЕТ");
                const bool pass = noRemoteImg && hasData && countOk
                                && spoilerGone && adsGone && cookieGone;
                add("fixImageUrls(офлайн)", pass,
                    QString("встроено_уник=%1 (ожидалось 3), data-url=%2, remote-img-осталось=%3, "
                            "spoiler=%4, ads=%5, cookie=%6")
                        .arg(embedded)
                        .arg(hasData ? "есть" : "НЕТ")
                        .arg(noRemoteImg ? "нет" : "ДА")
                        .arg(spoilerGone ? "удалён" : "ОСТАЛСЯ")
                        .arg(adsGone ? "удалена" : "ОСТАЛАСЬ")
                        .arg(cookieGone ? "удалён" : "ОСТАЛСЯ"));
            }
        }

        // (d1) extractTaskContent: результат = ТОЛЬКО post-content-область,
        // начинается с <div, div'ы сбалансированы, мусор сайта отсутствует
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("extractTaskContent(область)", false, "не удалось прочитать task_sample.html");
            } else {
                QString title;
                int spoilers = -1;
                const QString content = extractTaskContent(html, &title, &spoilers);

                const bool startsWithDiv = content.startsWith("<div");
                const int openDivs  = content.count("<div");
                const int closeDivs = content.count("</div>");
                const bool balanced = (openDivs == closeDivs) && (openDivs > 0);
                const bool hasTaskText = content.contains("МАРКЕР-ТЕКСТ-УСЛОВИЯ-UNIQ");
                const bool noHeader    = !content.contains("fusion-header");
                const bool noAds       = !content.contains("ADS-СЕКРЕТ");
                const bool noAside     = !content.contains("БОКОВАЯ-ПАНЕЛЬ-МАРКЕР");
                const bool noCookie    = !content.contains("COOKIE-СЕКРЕТ");
                const bool noAfterPost = !content.contains("МАРКЕР-ПОСЛЕ-POSTCONTENT");
                const bool garbageFree = noHeader && noAds && noAside && noCookie && noAfterPost;
                const bool pass = startsWithDiv && balanced && hasTaskText && garbageFree;
                add("extractTaskContent(область)", pass,
                    QString("starts_with_<div>=%1, div-баланс=%2 (open=%3 close=%4), "
                            "текст_задачи=%5, header/ads/aside/cookie/after-мусор=%6")
                        .arg(startsWithDiv ? "ок" : "СБОЙ")
                        .arg(balanced ? "ок" : "СБОЙ")
                        .arg(openDivs).arg(closeDivs)
                        .arg(hasTaskText ? "есть" : "НЕТ")
                        .arg(garbageFree ? "отсутствует" : "НАШЁЛСЯ"));
            }
        }

        // (d2) extractTaskContent: спойлеры → ВИДИМЫЕ блоки (Ответ/Решение не теряются)
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("extractTaskContent(спойлеры)", false, "не удалось прочитать task_sample.html");
            } else {
                QString title;
                int spoilers = -1;
                const QString content = extractTaskContent(html, &title, &spoilers);

                const bool noSpoilerClass  = !content.contains("math100-spoiler");
                const bool hasBlock        = content.contains("task-answer-block");
                const bool twoBlocks       = (content.count("task-answer-block") >= 2);
                const bool labelAnswer     = content.contains("<p class=\"task-answer-label\"><strong>Ответ</strong></p>");
                const bool labelSolution   = content.contains("<p class=\"task-answer-label\"><strong>Решение</strong></p>");
                const bool answerKept      = content.contains("ОТВЕТ-МАРКЕР: 1234,5");
                const bool solutionKept    = content.contains("РЕШЕНИЕ-МАРКЕР");
                const bool secretKept      = content.contains("СПОЙЛЕР-СЕКРЕТ"); // в новом пайплайне — сохраняется
                const bool noDisplayNone   = !content.contains("display:none");
                const bool countOk         = (spoilers == 2);
                const bool pass = noSpoilerClass && hasBlock && twoBlocks
                                && labelAnswer && labelSolution
                                && answerKept && solutionKept && secretKept
                                && noDisplayNone && countOk;
                add("extractTaskContent(спойлеры)", pass,
                    QString("math100-spoiler=%1, task-answer-block=%2 (2 шт=%3), label-Ответ=%4, label-Решение=%5, "
                            "ОТВЕТ-МАРКЕР=%6, РЕШЕНИЕ-МАРКЕР=%7, СПОЙЛЕР-СЕКРЕТ=%8, display:none=%9, превращено=%10 (ожидалось 2)")
                        .arg(noSpoilerClass ? "нет" : "ОСТАЛСЯ")
                        .arg(hasBlock ? "есть" : "НЕТ")
                        .arg(twoBlocks ? "да" : "нет")
                        .arg(labelAnswer ? "есть" : "НЕТ")
                        .arg(labelSolution ? "есть" : "НЕТ")
                        .arg(answerKept ? "сохранён" : "ПОТЕРЯН")
                        .arg(solutionKept ? "сохранён" : "ПОТЕРЯН")
                        .arg(secretKept ? "сохранён" : "ПОТЕРЯН")
                        .arg(noDisplayNone ? "нет" : "ЕСТЬ")
                        .arg(spoilers));
            }
        }

        // (d3) extractTaskContent: чистка — нет <script>, yandex_rtb, <iframe>
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("extractTaskContent(чистка)", false, "не удалось прочитать task_sample.html");
            } else {
                const QString content = extractTaskContent(html);
                const bool noScript   = !content.contains("<script") && !content.contains("<SCRIPT");
                const bool noYandex   = !content.contains("yandex_rtb") && !content.contains("Ya.Context");
                const bool noIframe   = !content.contains("<iframe") && !content.contains("informer.yandex");
                const bool pass = noScript && noYandex && noIframe;
                add("extractTaskContent(чистка)", pass,
                    QString("<script=%1, yandex_rtb/Ya.Context=%2, iframe/informer=%3")
                        .arg(noScript ? "нет" : "ЕСТЬ")
                        .arg(noYandex ? "нет" : "ЕСТЬ")
                        .arg(noIframe ? "нет" : "ЕСТЬ"));
            }
        }

        // (d4) extractTaskContent: titleOut == entry-title из fixture
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("extractTaskContent(title)", false, "не удалось прочитать task_sample.html");
            } else {
                QString title;
                extractTaskContent(html, &title);
                const QString expected = QString::fromUtf8("ЕГЭ Профиль. Тестовая задача. Задача 1");
                const bool pass = (title == expected);
                add("extractTaskContent(title)", pass,
                    QString("titleOut=%1 (ожидалось: %2)").arg(title, expected));
            }
        }

        // (d5) task-12: хвостовой ОДИНОЧНЫЙ «/» в math-спане переносится
        // наружу сразу после </span> («20°/мин» читается правильно);
        // формулы без хвостового слеша и с двойным «//» не трогаются.
        {
            const QString html =
                "<html><body>"
                "<div class=\"post-content\">"
                "<p>Скорость <span class=\"math\">\\(\\omega  = {20^ \\circ }/\\)</span> мин</p>"
                "<p>Норма <span class=\"math\">\\(x + 1\\)</span> без слеша</p>"
                "<p>Двойной <span class=\"math\">\\(f//\\)</span> не одиночный</p>"
                "<p>Экранир. <span class=\"math\">\\(x\\/\\)</span> мин</p>"
                "</div>"
                "</body></html>";
            const QString content = extractTaskContent(html);

            const bool moved = content.contains(
                "<span class=\"math\">\\(\\omega  = {20^ \\circ }\\)</span>/ мин");
            const bool noInsideSlash = !content.contains("{20^ \\circ }/\\)");
            const bool normalUntouched = content.contains(
                "<span class=\"math\">\\(x + 1\\)</span>");
            const bool doubleSlashUntouched = content.contains(
                "<span class=\"math\">\\(f//\\)</span>");
            const bool escapedMoved = content.contains(
                "<span class=\"math\">\\(x\\)</span>/ мин");
            const bool pass = moved && noInsideSlash && normalUntouched
                            && doubleSlashUntouched && escapedMoved;
            add("extractTaskContent(хвостовой /)", pass,
                QString("20°/мин перенесён=%1, слеш-внутри-спана=%2, без-слеша=%3, "
                        "// не тронут=%4, \\/ перенесён=%5")
                    .arg(moved ? "ок" : "СБОЙ")
                    .arg(noInsideSlash ? "нет" : "ОСТАЛСЯ")
                    .arg(normalUntouched ? "ок" : "СБОЙ")
                    .arg(doubleSlashUntouched ? "ок" : "СБОЙ")
                    .arg(escapedMoved ? "ок" : "СБОЙ"));
        }

        // (h) картинки: КАК БЫЛО (data-URL, remote не осталось) —
        // теперь по реальному пайплайну: extractTaskContent → fixImageUrlsOffline
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("pipeline(картинки)", false, "не удалось прочитать task_sample.html");
            } else {
                QHash<QString, QByteArray> imgs;
                imgs["https://math100.ru/images/task10_1-1.png"] = png1x1;
                imgs["https://math100.ru/images/task10_1-2.png"] = png1x1;
                imgs["https://math100.ru/images/figure.png"]     = png1x1;

                const QString content = extractTaskContent(html);
                int embedded = 0;
                const QString fixed = fixImageUrlsOffline(content, imgs, &embedded);

                const bool noRemoteImg = !fixed.contains("src=\"https://math100.ru/");
                const bool hasData     = fixed.contains("data:image/png;base64,");
                const bool countOk     = (embedded == 3);
                const bool dataImgCountOk = (fixed.count("src=\"data:image/") == 4); // 4 тега img
                const bool pass = noRemoteImg && hasData && countOk && dataImgCountOk;
                add("pipeline(картинки)", pass,
                    QString("встроено_уник=%1 (ожидалось 3), data-url-тегов=%2 (ожидалось 4), "
                            "remote-img-осталось=%3")
                        .arg(embedded).arg(fixed.count("src=\"data:image/"))
                        .arg(noRemoteImg ? "нет" : "ДА"));
            }
        }

        // (i) standalone-обёртка: DOCTYPE, </html>, MathJax CDN, <title>
        {
            const QString html = readTextFile(td + "/task_sample.html");
            if (html.isEmpty()) {
                add("standalone(обёртка)", false, "не удалось прочитать task_sample.html");
            } else {
                QString title;
                extractTaskContent(html, &title);
                const QString doc = wrapStandaloneHtml(title, "<p>КОНТЕНТ-ТЕСТ</p>");
                const bool doctype  = doc.startsWith("<!DOCTYPE html>");
                const bool closed   = doc.trimmed().endsWith("</html>");
                // task-12d: standalone (внешний браузер, qrc недоступен) —
                // ОДИН простой CDN-тег jsdelivr, БЕЗ loader'а (unpkg/cdnjs/
                // setInterval-watchdog/__mathjaxFailed). В приложении MathJax
                // локальный (qrc:/mathjax).
                const bool mathjax  = doc.contains("https://cdn.jsdelivr.net/npm/mathjax@3/es5/tex-mml-chtml.js");
                const bool noLoader = !doc.contains("unpkg.com")
                                    && !doc.contains("cdnjs.cloudflare.com")
                                    && !doc.contains("__mathjaxFailed")
                                    && !doc.contains("setInterval");
                const bool confMath = doc.contains("window.MathJax")
                                    && doc.contains("inlineMath")
                                    && doc.contains("displayMath")
                                    && doc.contains("processEscapes");
                const bool titleOk  = doc.contains("<title>" + title + "</title>");
                const bool bodyOk   = doc.contains("<p>КОНТЕНТ-ТЕСТ</p>");
                const bool styleOk  = doc.contains(".task-answer-block") && doc.contains("Times New Roman");
                // task-12: @page для браузерной печати + КАНОНИЧЕСКИЙ fitWideMath
                // + автовызов после typeset (typesetPromise().then) + resize
                const bool pageOk   = doc.contains("@page { size: A4; margin: 15mm; }");
                const bool fitOk    = doc.contains("window.fitWideMath = function ()")
                                    && doc.contains("mjx-container[display=\"true\"]");
                const bool fitCallOk = doc.contains("MathJax.typesetPromise().then")
                                     && doc.contains("window.fitWideMath");
                const bool resizeOk = doc.contains("addEventListener('resize'");
                const bool pass = doctype && closed && mathjax && noLoader
                                && confMath && titleOk && bodyOk
                                && styleOk && pageOk && fitOk && fitCallOk && resizeOk;
                add("standalone(обёртка)", pass,
                    QString("doctype=%1, </html>=%2, mathjax-jsdelivr=%3, без-loader'а=%4, "
                            "mathjax-config=%5, <title>=%6, style=%7, @page15mm=%8, fitWideMath=%9, "
                            "fit-после-typeset=%10, resize=%11")
                        .arg(doctype ? "ок" : "СБОЙ")
                        .arg(closed ? "ок" : "СБОЙ")
                        .arg(mathjax ? "есть" : "НЕТ")
                        .arg(noLoader ? "ок" : "СБОЙ")
                        .arg(confMath ? "есть" : "НЕТ")
                        .arg(titleOk ? "ок" : "СБОЙ")
                        .arg(styleOk ? "есть" : "НЕТ")
                        .arg(pageOk ? "есть" : "НЕТ")
                        .arg(fitOk ? "есть" : "НЕТ")
                        .arg(fitCallOk ? "есть" : "НЕТ")
                        .arg(resizeOk ? "есть" : "НЕТ"));
            }
        }

        // (c) Кэш: saveToCache/loadFromCache round-trip во временный каталог
        {
            QTemporaryDir tmp;
            const bool tmpOk = tmp.isValid();
            const QString url  = "https://math100.ru/ege_profil_10_1-1/";
            const QString imgU = "https://math100.ru/images/figure.png";
            const QByteArray pagePayload = "<html>каши-вариант-тест-123</html>";
            const QByteArray imgPayload  = "IMGDATA-456";

            const bool saved1 = saveToCache(tmp.path(), url,  "pages",  pagePayload);
            const bool saved2 = saveToCache(tmp.path(), imgU, "images", imgPayload);

            QByteArray gotPage, gotImg;
            const bool loadPage = loadFromCache(tmp.path(), url,  "pages",  gotPage);
            const bool loadImg  = loadFromCache(tmp.path(), imgU, "images", gotImg);
            const bool pageEq = (gotPage == pagePayload);
            const bool imgEq  = (gotImg  == imgPayload);

            QByteArray missing;
            const bool missFalse = !loadFromCache(tmp.path(), "https://math100.ru/nope/", "pages", missing);

            const QString fn = urlToFilename(url);
            const bool fnOk  = fn.endsWith(".png") && (fn.size() == 36); // 32 hex + ".png"
            // Детерминизм: один URL -> один и тот же файл
            const bool fnDet = (urlToFilename(url) == fn);

            const bool pass = tmpOk && saved1 && saved2 && loadPage && pageEq
                            && loadImg && imgEq && missFalse && fnOk && fnDet;
            add("cache(round-trip)", pass,
                QString("tmp=%1, save=%2, page-rt=%3, image-rt=%4, missing=false=%5, filename=%6(%7)")
                    .arg(tmpOk ? "ок" : "НЕТ")
                    .arg((saved1 && saved2) ? "ок" : "СБОЙ")
                    .arg(pageEq ? "ок" : "СБОЙ")
                    .arg(imgEq ? "ок" : "СБОЙ")
                    .arg(missFalse ? "ок" : "СБОЙ")
                    .arg(fn)
                    .arg(fnOk ? "ок" : "СБОЙ"));
        }
    }

    int failed = 0;
    for (const Test &t : tests) {
        say(QString("  [%1] %2 — %3").arg(t.pass ? "PASS" : "FAIL", t.name, t.detail));
        if (!t.pass)
            ++failed;
    }
    const int total = tests.size();
    say(QString("Итог selftest: %1 из %2 тестов пройдено").arg(total - failed).arg(total));
    return failed == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Команда: fetch <variant-url>
// ---------------------------------------------------------------------------
static int cmdFetch(QNetworkAccessManager &nam, const QString &variantUrl)
{
    say("[1/3] Скачиваю страницу варианта: " + variantUrl);
    logEvent("fetch: start url=" + variantUrl);
    FetchResult v = fetchUrl(nam, variantUrl);
    if (!v.ok) {
        say("ОШИБКА: " + v.error);
        logEvent("fetch: FAIL " + v.error);
        return 1;
    }
    say(QString("      Получено %1 байт (HTTP %2)").arg(v.data.size()).arg(v.httpStatus));

    say("[2/3] Извлекаю URL задач (regex из эталонного networkmanager.cpp)...");
    const QString html = QString::fromUtf8(v.data);
    QStringList urls = extractTaskUrls(html, variantUrl);
    // task-10: кап 30 убран — возвращаем ВСЕ найденные задачи
    // (проф-варианты: 33, базовые: 18).
    if (urls.isEmpty()) {
        say("ОШИБКА: на странице не найдено ни одной ссылки на задачи.");
        logEvent("fetch: FAIL no task urls found");
        return 1;
    }
    say(QString("      Найдено ссылок: %1").arg(urls.size()));
    for (int i = 0; i < urls.size(); ++i)
        say(QString("  %1. %2").arg(i + 1).arg(urls.at(i)));

    const QString dir = outDir();
    QDir().mkpath(dir);
    const QByteArray hash = QCryptographicHash::hash(variantUrl.toUtf8(), QCryptographicHash::Md5);
    const QString outPath = dir + "/variant_" + QString(hash.toHex()) + ".html";
    const bool saved = writeUtf8File(outPath, v.data);
    say("[3/3] Сохранено: " + outPath + (saved ? " (OK)" : " (ОШИБКА ЗАПИСИ)"));
    logEvent(QString("fetch: OK urls=%1 saved=%2 path=%3")
                  .arg(urls.size()).arg(saved ? "да" : "нет").arg(outPath));
    return saved ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Команда: task <task-url> [--no-images]
// task-4 пайплайн: fetch → extractTaskContent (только post-content, спойлеры
// → видимые блоки) → fixImageUrlsOffline (картинки в data-URL) → standalone HTML
// (MathJax для браузера) → сохранение.
// ---------------------------------------------------------------------------
static int cmdTask(QNetworkAccessManager &nam, const QString &taskUrl, bool noImages)
{
    say("[1/4] Скачиваю страницу задачи: " + taskUrl);
    logEvent("task: start url=" + taskUrl + " noImages=" + (noImages ? "да" : "нет"));
    FetchResult t = fetchUrl(nam, taskUrl);
    if (!t.ok) {
        say("ОШИБКА: " + t.error);
        logEvent("task: FAIL " + t.error);
        return 1;
    }
    const QString html = QString::fromUtf8(t.data);
    say(QString("      Получено %1 байт (HTTP %2)").arg(t.data.size()).arg(t.httpStatus));

    say("[2/4] Извлекаю ТОЛЬКО контент задачи (div.post-content, без рекламы/шапки/сайдбара)...");
    QString title;
    int spoilersConverted = 0;
    const QString content = extractTaskContent(html, &title, &spoilersConverted);
    say(QString("      title = %1").arg(title.isEmpty()
            ? QString::fromUtf8("(не найден, использую «Задача»)") : title));
    say(QString("      spoiler-блоков превращено = %1 (Ответ/Решение стали видимыми)")
            .arg(spoilersConverted));

    int embedded = 0, downloaded = 0, cached = 0;
    QString fixed;
    if (noImages) {
        say("[3/4] --no-images: пропускаю скачивание картинок");
        QHash<QString, QByteArray> empty;
        fixed = fixImageUrlsOffline(content, empty, &embedded);
    } else {
        say("[3/4] Скачиваю картинки задачи и встраиваю как data-URL...");
        const QVector<QString> iurls = collectImageUrls(content);
        say(QString("      Найдено уникальных картинок в контенте: %1").arg(iurls.size()));
        QHash<QString, QByteArray> imgs;
        for (const QString &u : iurls) {
            QByteArray data;
            if (loadFromCache(outDir(), u, "images", data)) {
                imgs[u] = data;
                ++cached;
            } else {
                FetchResult ir = fetchUrl(nam, u);
                if (ir.ok) {
                    imgs[u] = ir.data;
                    ++downloaded;
                    saveToCache(outDir(), u, "images", ir.data);
                } else {
                    say("      предупреждение: не скачалась " + u + " (" + ir.error + ")");
                    logEvent("task: img FAIL " + u + " " + ir.error);
                }
            }
        }
        say(QString("      Скачано: %1, из кэша: %2, готово для встраивания: %3")
            .arg(downloaded).arg(cached).arg(imgs.size()));
        fixed = fixImageUrlsOffline(content, imgs, &embedded);
    }

    say("[4/4] Собираю standalone HTML (MathJax, стили) и сохраняю...");
    const QString doc = wrapStandaloneHtml(title, fixed);

    const QString dir = outDir();
    QDir().mkpath(dir);
    const QByteArray hash = QCryptographicHash::hash(taskUrl.toUtf8(), QCryptographicHash::Md5);
    const QString outPath = dir + "/task_" + QString(hash.toHex()) + ".html";
    const bool saved = writeUtf8File(outPath, doc.toUtf8());
    say("[OK] Сохранено: " + outPath + (saved ? " (OK)" : " (ОШИБКА ЗАПИСИ)"));

    // Честная сводка по ИТОГОВОМУ документу (весь standalone HTML, а не только контент)
    const int finalDataImgs = doc.count(R"(src="data:image/)");
    say(QString("      Сводка: размер итогового HTML = %1 байт (вся страница была %2 байт); "
                "картинок встроено (data-URL) = %3; скачано = %4; из кэша = %5; "
                "spoiler-блоков превращено = %6; title = %7")
        .arg(doc.toUtf8().size()).arg(t.data.size())
        .arg(finalDataImgs).arg(downloaded).arg(cached)
        .arg(spoilersConverted).arg(title.isEmpty() ? QString("Задача") : title));
    logEvent(QString("task: OK finalSize=%1 rawSize=%2 embedded=%3 finalDataImgs=%4 "
                     "downloaded=%5 cached=%6 spoilers=%7 title=%8 path=%9")
                  .arg(doc.toUtf8().size()).arg(t.data.size()).arg(embedded)
                  .arg(finalDataImgs).arg(downloaded).arg(cached)
                  .arg(spoilersConverted).arg(title).arg(outPath));
    return saved ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Команда: cache clear
// ---------------------------------------------------------------------------
static int cmdCacheClear()
{
    const QString dir = outDir();
    if (QDir(dir).exists())
        QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    say("Очищено: " + dir);
    logEvent("cache clear: OK");
    return 0;
}

// ---------------------------------------------------------------------------
// Справка
// ---------------------------------------------------------------------------
static void printHelp()
{
    say("math100_console — headless-тестер функционала Math100 PDF Generator (Qt Network, без GUI).");
    say("");
    say("Команды:");
    say("  selftest                       Офлайн-тесты без сети: extractTaskUrls, extractTaskContent,");
    say("                                  fixImageUrls, standalone-обёртка, кэш");
    say("  fetch <variant-url>            Скачать страницу варианта, извлечь ВСЕ URL задач (без капа);");
    say("                                  поддерживаются проф- (ege_profil_*) и базовые (baza_*) варианты");
    say("  task <task-url> [--no-images]  Скачать задачу и сохранить STANDALONE HTML: только текст задачи");
    say("                                  (без шапки/рекламы/сайдбара), Ответ/Решение — видимые блоки,");
    say("                                  картинки — data-URL, MathJax для формул (CDN при открытии в браузере)");
    say("  cache clear                    Очистить каталог console/out/");
    say("  help                           Эта справка");
    say("");
    say("Примеры:");
    say("  bash console/run.sh selftest");
    say("  bash console/run.sh fetch https://math100.ru/prof-ege-2027-10-1/");
    say("  bash console/run.sh task https://math100.ru/ege_profil_8_1-1/");
    say("  bash console/run.sh task https://math100.ru/ege_profil_8_1-1/ --no-images");
    say("  bash console/run.sh cache clear");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("math100_console");
    QNetworkAccessManager nam;

    QStringList args = app.arguments();
    args.removeFirst(); // имя программы

    if (args.isEmpty()) {
        printHelp();
        return 0;
    }

    const QString cmd = args.first();

    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        printHelp();
        return 0;
    }

    if (cmd == "selftest") {
        const int r = cmdSelftest();
        logEvent(QString("selftest: exit=%1").arg(r));
        return r;
    }

    if (cmd == "fetch") {
        if (args.size() < 2) {
            say("Ошибка: fetch требует <variant-url>. Пример:");
            say("  bash console/run.sh fetch https://math100.ru/prof-ege-2027-10-1/");
            return 1;
        }
        return cmdFetch(nam, args.at(1));
    }

    if (cmd == "task") {
        bool noImages = false;
        QString url;
        for (int i = 1; i < args.size(); ++i) {
            const QString a = args.at(i);
            if (a == "--no-images")
                noImages = true;
            else if (!a.startsWith("--"))
                url = a;
        }
        if (url.isEmpty()) {
            say("Ошибка: task требует <task-url>. Пример:");
            say("  bash console/run.sh task https://math100.ru/ege_profil_8_1-1/");
            return 1;
        }
        return cmdTask(nam, url, noImages);
    }

    if (cmd == "cache") {
        if (args.size() >= 2 && args.at(1) == "clear")
            return cmdCacheClear();
        say("Ошибка: неизвестное действие 'cache' (используйте: cache clear)");
        return 1;
    }

    say("Неизвестная команда: " + cmd);
    printHelp();
    return 1;
}
