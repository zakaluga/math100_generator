#include "webenginehost.h"
#include "networkmanager.h"
#include <QVBoxLayout>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QUrl>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QJsonDocument>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QTimer>
#include <QScopeGuard>

namespace {

// Ожидание готовности MathJax на странице. Возвращает:
//   "ready"   — mjx-container появились либо MathJax.state() == TYPESET;
//   "absent"  — MathJax на странице нет вообще (нечего ждать), в т.ч.
//               task-12c: window.__mathjaxFailed (все CDN мертвы — loader
//               поставил флаг; проверяется ПЕРВЫМ, до readyState-гейта);
//   "timeout" — backstop-таймер сработал (продолжаем, не роняем).
// ВАЖНО (проверено headless-тестом на Qt 6.8.2): runJavaScript с
// Promise-результатом доставляет callback ПУСТОЕ значение (особенность
// этой сборки Qt), поэтому поллим простой (не-promise) выражение каждые
// 500ms, а не MathJax.startup.promise. Пустой результат не останавливает
// poll — сработает backstop.
// task-15b: всё состояние (loop/done/pollCount/статус) — в куче с родителем
// page. Отложенный callback runJavaScript (рендерер занят typeset'ом) может
// прийти ПОСЛЕ выхода из loop.exec() — с локальными переменными стека это
// use-after-free (кандидат на краш Windows). Пока page жива, st жива
// (дети QObject уничтожаются после тела ~QWebEnginePage, т.е. даже во время
// teardown WebEngineCore st ещё валидна); после удаления страницы новых
// callback'ов по странице не будет.
QString waitMathJaxReady(QWebEnginePage *page, int timeoutMs)
{
    struct WaitState : public QObject {
        QEventLoop loop;
        bool done = false;
        int pollCount = 0;
        QString status;
        explicit WaitState(QObject *parent) : QObject(parent) {}
    };
    auto *st = new WaitState(page);
    // URL берём ДО loop.exec(): callback может сработать во время
    // уничтожения страницы, а обращаться к page внутри него небезопасно.
    const QString urlStr = page->url().toString();
    auto *poll = new QTimer(st);
    QObject::connect(poll, &QTimer::timeout, st, [page, poll, st, urlStr]() {
        ++st->pollCount;
        page->runJavaScript(R"(
            (function(){
                try {
                    // task-12c: все CDN мертвы (loader поставил флаг) —
                    // уходить в 'absent' СРАЗУ, до readyState-гейта:
                    // страница с упавшими скриптами завершает загрузку,
                    // printToPdf сработает, не ждём backstop.
                    if (window.__mathjaxFailed) {
                        return 'absent';
                    }
                    // Страница ещё грузится — MathJax-конфиг мог не выполниться,
                    // рано говорить 'absent'.
                    if (document.readyState !== 'complete') {
                        return 'pending';
                    }
                    if (document.querySelectorAll('mjx-container').length > 0) {
                        return 'ready';
                    }
                    var MJ = window.MathJax;
                    if (!MJ) {
                        return 'absent';
                    }
                    if (MJ.startup && MJ.startup.document && MJ.STATE &&
                        MJ.startup.document.state() === MJ.STATE.TYPESET) {
                        return 'ready';
                    }
                    return 'pending';
                } catch (e) {
                    return 'pending';
                }
            })()
        )", [st, poll, urlStr](const QVariant &res) {
            // task-12c DEBUG (временное)
            qInfo().noquote() << QString("[MJWAIT] poll#%1: res=\"%2\" url=%3")
                .arg(st->pollCount).arg(res.toString()).arg(urlStr);
            if (!res.isValid() || res.toString().isEmpty()) {
                // Пустой результат (артефакт сборки) — не останавливаем poll,
                // сработает backstop-таймаут.
                return;
            }
            const QString status = res.toString();
            if (status == "ready" || status == "absent") {
                if (st->done) {
                    return;
                }
                poll->stop();
                st->done = true;
                st->status = status;
                st->loop.quit();
            }
        });
    });
    QTimer::singleShot(timeoutMs, st, [st] {
        if (!st->done) {
            st->done = true;
            st->status = QStringLiteral("timeout");
            st->loop.quit();
        }
    });
    poll->start(500);
    st->loop.exec();
    return st->status;
}

// Асинхронный printToPdf (Qt 6.8) через callback-перегрузку + локальный
// event loop; backstop-таймаут. Возвращает true, если callback пришёл.
// task-15b: состояние (done/loop/таймер) — в куче с родителем page.
// Документация Qt: callback printToPdf ВСЕГДА вызывается, в т.ч. при
// уничтожении страницы (с пустым значением). С переменными в стеке
// (done/pdfData/loop — кадры уже завершившихся функций) это
// use-after-free: backstop -> print остаётся in-flight -> deleteLater
// страницы -> callback на мёртвый стек. Теперь callback проверяет
// st->done (куча, живёт вместе со страницей) и не трогает pdfData,
// если вызов уже не актуален.
bool waitForPrintToPdf(QWebEnginePage *page, QByteArray &pdfData, int timeoutMs)
{
    struct WaitState : public QObject {
        QEventLoop loop;
        bool done = false;
        QElapsedTimer dbgT;
        explicit WaitState(QObject *parent) : QObject(parent) {}
    };
    auto *st = new WaitState(page);
    QTimer *backstop = new QTimer(st);
    backstop->setSingleShot(true);
    QObject::connect(backstop, &QTimer::timeout, st, [st] {
        if (!st->done) {
            st->done = true;
            st->loop.quit();
        }
    });
    backstop->start(timeoutMs);
    // task-12c-fix DEBUG (временное)
    st->dbgT.start();
    qInfo().noquote() << "[PDFDBG] printToPdf: request issued url=" << page->url().toString();

    page->printToPdf([st, &pdfData](const QByteArray &data) {
        if (st->done) {
            // Backstop уже сработал (callback принят ранее) — pdfData живёт
            // в стеке вызывающего, который мог уже завершиться. Не трогаем.
            return;
        }
        qInfo().noquote() << "[PDFDBG] printToPdf: callback data=" << data.size()
                          << "bytes after" << st->dbgT.elapsed() << "ms";
        st->done = true;
        pdfData = data;
        st->loop.quit();
    });
    st->loop.exec();
    if (!st->done) {
        qInfo().noquote() << "[PDFDBG] printToPdf: BACKSTOP fired after" << st->dbgT.elapsed()
                          << "ms (callback не пришёл)";
    }
    return st->done;
}

// task-12: подгонка широких формул ПЕРЕД печатью. Выполняет на странице
// window.fitWideMath() (объявлена в app.js для превью и встраивается в
// экспорт-документы; window.__mathFitted сбрасывается на всякий случай)
// и ЖДЁТ callback runJavaScript (backstop 5s — сбой fit не останавливает
// печать), а printToPdf вызывается ВНУТРИ этого callback.
// task-15b: состояние (started/loop/таймер) — в куче с родителем page:
// отложенный callback runJavaScript может прийти после выхода из
// loop.exec() (когда backstop уже сработал) — с локальным `started`
// в стеке это use-after-free.
void printAfterFit(QWebEnginePage *page, QByteArray &pdfData)
{
    struct FitState : public QObject {
        QEventLoop loop;
        bool started = false;
        QElapsedTimer dbgFitT;
        explicit FitState(QObject *parent) : QObject(parent) {}
    };
    auto *st = new FitState(page);
    st->dbgFitT.start();
    auto doPrint = [page, &pdfData, st](auto &&...) {
        if (st->started) {
            return;
        }
        qInfo().noquote() << "[PDFDBG] printAfterFit: fit-callback path, elapsed="
                          << st->dbgFitT.elapsed() << "ms";
        st->started = true;
        waitForPrintToPdf(page, pdfData, 30000);
        st->loop.quit();
    };
    QTimer *backstop = new QTimer(st);
    backstop->setSingleShot(true);
    QObject::connect(backstop, &QTimer::timeout, st, doPrint);
    backstop->start(5000);
    page->runJavaScript(
        "window.__mathFitted=false; if (window.fitWideMath) window.fitWideMath(); 'ok'",
        doPrint);
    st->loop.exec();
}

// Запись готовых байтов PDF в файл. false + *errorOut при ошибке.
bool writePdfFile(const QByteArray &pdfData, const QString &filePath, QString *errorOut)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[PDF] не удалось открыть файл для записи:" << qPrintable(filePath)
                   << file.errorString();
        if (errorOut) {
            *errorOut = file.errorString();
        }
        return false;
    }
    const qint64 written = file.write(pdfData);
    file.close();
    if (written != pdfData.size()) {
        qWarning() << "[PDF] неполная запись:" << written << "из" << pdfData.size() << "байт";
        if (errorOut) {
            *errorOut = QStringLiteral("неполная запись");
        }
        return false;
    }
    return true;
}

} // namespace

WebEngineHost::WebEngineHost(QWidget *parent, NetworkManager *networkMgr)
    : QObject(parent)
    , m_view(nullptr)
    , m_page(nullptr)
    , m_channel(nullptr)
    , m_networkManager(networkMgr)
    , m_currentTaskUrl("")
    , m_initialized(false)
    , m_pdfBusy(false)
{
    setupWebView();
    setupWebChannel();
    showPlaceholder("Введите URL варианта и нажмите «Загрузить»");
}

void WebEngineHost::setupWebView()
{
    m_view = new QWebEngineView();

    // Настройка профиля
    auto *profile = QWebEngineProfile::defaultProfile();

    // Настройки WebView
    auto *settings = profile->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LinksIncludedInFocusChain, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    settings->setAttribute(QWebEngineSettings::WebGLEnabled, false);

    m_page = m_view->page();

    connect(m_page, &QWebEnginePage::loadStarted, this, [this]() {
        emit statusChanged("Загрузка страницы...");
    });

    connect(m_page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (ok) {
            emit statusChanged("Страница загружена");
        } else {
            emit statusChanged("Ошибка загрузки страницы");
        }
    });
}

void WebEngineHost::setupWebChannel()
{
    m_channel = new QWebChannel(m_page);
    m_channel->registerObject("webEngineHost", this);
    m_page->setWebChannel(m_channel);
    m_initialized = true;

    // Инициализация QWebChannel на стороне JS
    m_page->runJavaScript(R"(
        if (typeof qtInstance === 'undefined') {
            new QWebChannel(qt.webChannelTransport, function(channel) {
                window.qtInstance = channel.objects.webEngineHost;
            });
        }
    )");
}

void WebEngineHost::loadTaskPage(const QString &html, const QString &taskUrl)
{
    m_currentTaskUrl = taskUrl;
    emit statusChanged("Отрисовка задачи...");

    // Создаём полный HTML-документ
    QString fullHtml = createFullHtml(html);

    // Загружаем с base URL
    QUrl baseUrl{taskUrl};
    m_page->setContent(fullHtml.toUtf8(), "text/html", baseUrl);
}

void WebEngineHost::triggerPrint()
{
    if (!m_page) {
        return;
    }

    // Имя файла по умолчанию: последний сегмент пути URL задачи + ".pdf"
    // (напр. https://math100.ru/ege_profil_8_1-3/ -> ege_profil_8_1-3.pdf)
    QString base = "task";
    const QStringList segments = QUrl(m_currentTaskUrl).path().split('/', Qt::SkipEmptyParts);
    if (!segments.isEmpty()) {
        const QString last = segments.last().simplified();
        if (!last.isEmpty()) {
            base = last;
        }
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }
    QDir().mkpath(dir);

    QWidget *parent = m_view ? m_view->window() : nullptr;
    QString path = QFileDialog::getSaveFileName(
        parent,
        tr("Сохранить PDF"),
        dir + '/' + base + ".pdf",
        tr("PDF-файлы (*.pdf)"));

    if (path.isEmpty()) {
        // Пользователь отменил выбор — просто выходим без ошибок
        return;
    }
    if (!path.endsWith(".pdf", Qt::CaseInsensitive)) {
        path += ".pdf";
    }

    const bool ok = printPdfTo(path);
    if (ok) {
        emit statusChanged(tr("PDF сохранён: %1").arg(path));
    } else {
        emit statusChanged(tr("Ошибка создания PDF"));
    }
}

bool WebEngineHost::printPdfTo(const QString &filePath)
{
    if (!m_page) {
        qWarning() << "[PDF] printPdfTo: страница не инициализирована";
        return false;
    }
    if (m_pdfBusy) {
        qWarning() << "[PDF] printPdfTo: предыдущий запрос ещё выполняется";
        emit statusChanged(tr("PDF уже создаётся, повторите позже"));
        return false;
    }
    m_pdfBusy = true;
    const QScopeGuard busyGuard{[this] { m_pdfBusy = false; }};

    // 1) Ждём готовности MathJax: иначе в PDF попадут неотрисованные формулы.
    //    (Poll простыми JS-выражениями, backstop 10s — см. waitMathJaxReady.)
    emit statusChanged(tr("Ожидание готовности MathJax..."));

    const QString mjStatus = waitMathJaxReady(m_page, 10000);

    if (mjStatus == "timeout") {
        qWarning() << "[PDF] MathJax: таймаут ожидания (10s) — продолжаем, "
                   << "формулы могут быть неотрисованы";
        emit statusChanged(tr("MathJax не готов (таймаут), продолжаем..."));
    } else if (mjStatus == "absent") {
        qWarning() << "[PDF] MathJax не обнаружен на странице (CDN недоступен?) — "
                   << "формулы не отрисуются";
    }

    // 2) Генерация PDF. task-12: перед printToPdf принудительно повторяем
    //    подгонку широких display-формул (window.fitWideMath) и ждём её
    //    callback; сам printToPdf вызывается внутри этого callback
    //    (Qt 6.8: асинхронный, callback + локальный event loop,
    //    страховка-таймаут 30s).
    emit statusChanged(tr("Создание PDF..."));

    QByteArray pdfData;
    printAfterFit(m_page, pdfData);

    // task-12c: страховка от мигания Chromium — пустой результат первой
    // попытки повторяем ОДИН раз, прежде чем объявлять ошибку.
    if (pdfData.isEmpty()) {
        qWarning() << "[PDF] первая попытка пуста — retry printToPdf";
        emit statusChanged(tr("PDF пуст, повторная попытка..."));
        printAfterFit(m_page, pdfData);
    }

    if (pdfData.isEmpty()) {
        qWarning() << "[PDF] printToPdf вернул пустой результат (таймаут или сбой рендеринга)";
        emit statusChanged(tr("Ошибка создания PDF"));
        return false;
    }

    // 3) Запись файла
    QString err;
    if (!writePdfFile(pdfData, filePath, &err)) {
        emit statusChanged(tr("Ошибка создания PDF: %1").arg(err));
        return false;
    }

    qInfo() << "[PDF] PDF записан:" << qPrintable(filePath) << "(" << pdfData.size() << "байт)";
    return true;
}

bool WebEngineHost::printHtmlToPdf(const QString &fullHtml, const QString &filePath, int mathjaxWaitSec)
{
    if (fullHtml.isEmpty()) {
        qWarning() << "[PDF] printHtmlToPdf: пустой HTML-документ";
        return false;
    }
    if (m_pdfBusy) {
        qWarning() << "[PDF] printHtmlToPdf: предыдущий запрос ещё выполняется";
        emit statusChanged(tr("PDF уже создаётся, повторите позже"));
        return false;
    }
    m_pdfBusy = true;
    const QScopeGuard busyGuard{[this] { m_pdfBusy = false; }};

    // Отдельная временная страница: отображаемую m_page (видимый view с
    // текущей задачей) не трогаем. Профиль общий (defaultProfile через
    // m_page) — кэш/cookies те же, что у приложения.
    // task-16a: страница ОБЯЗАТЕЛЬНО привязана к реальному QWebEngineView —
    // print-конвейер Chromium (viz/print handler) инициализируется только
    // для страницы внутри view: на Windows «сиротная» страница (без view)
    // давала JS/MathJax ok, но callback printToPdf НИКОГДА не приходил
    // (30s backstop, пустой PDF). setPage переродительствует page в view
    // (docs Qt) — владение: view -> page, cleanup ниже уничтожает view.
    // task-15b: удаление — НЕ сразу после print, а после loadFinished
    // + grace (блок "cleanup" ниже): уничтожение страницы с in-flight
    // load/print роняет Chromium, а callback printToPdf при этом
    // гарантированно срабатывает (docs Qt) — на мёртвый стек.
    QWebEngineProfile *profile = m_page ? m_page->profile() : QWebEngineProfile::defaultProfile();
    auto *page = new QWebEnginePage(profile, this);
    // WebEngineHost — QObject (не QWidget): родительский WIDGET, если он
    // есть (в приложении — MainWindow), берём через qobject_cast.
    QWidget *parentW = qobject_cast<QWidget *>(this->parent());
    auto *view = parentW ? new QWebEngineView(parentW) : new QWebEngineView();
    view->setPage(page);
    view->setFixedSize(794, 1123); // ≈A4 @96dpi
    view->setWindowTitle(tr("Math100 — формирование PDF"));
    // Qt::Tool: без записи в taskbar (Windows); если флаг даст проблемы
    // с modality/поведением — см. отчёт task-16a.
    view->setWindowFlags(view->windowFlags() | Qt::Tool);
    // База about:blank допустима: скрипты/стили в документе экспорта —
    // абсолютные (CDN). loadFinished покрывается тем же poll (readyState).
    page->setHtml(fullHtml, QUrl("about:blank"));

    // 1) Ожидание loadFinished + готовности MathJax (backstop: mathjaxWaitSec).
    emit statusChanged(tr("Ожидание готовности MathJax..."));
    const QString mjStatus = waitMathJaxReady(page, mathjaxWaitSec * 1000);
    if (mjStatus == "timeout") {
        qWarning() << "[PDF] MathJax: таймаут ожидания (" << mathjaxWaitSec
                   << "s) — продолжаем, формулы могут быть неотрисованы";
        emit statusChanged(tr("MathJax не готов (таймаут), продолжаем..."));
    } else if (mjStatus == "absent") {
        qWarning() << "[PDF] MathJax не обнаружен в документе экспорта "
                   << "(CDN недоступен?) — формулы не отрисуются";
    }

    // 2) Генерация PDF: task-12 — сначала fitWideMath (callback), потом
    //    printToPdf внутри callback (та же механика, что в printPdfTo).
    // task-16a: до 3 попыток с эскалацией видимости view — print-конвейер
    // Chromium на Windows может не инициализироваться, пока view не стал
    // «видимым» (в разных смыслах): попытка 1 — WA_DontShowOnScreen
    // (видим для layout/рендера, без окна), попытка 2 — невидимое
    // tool-окно за экраном, попытка 3 — реально видимое окно (крайний
    // случай, пользователь видит процесс). Страница между попытками НЕ
    // пересоздаётся: MathJax уже typeset'нут, setHtml выше — один раз.
    emit statusChanged(tr("Создание PDF..."));
    QByteArray pdfData;
    const struct { const char *mode; const char *status; } kAttemptModes[] = {
        {"dontshow",  "Создание PDF (попытка 1: offscreen-видимость)..."},
        {"offscreen", "PDF пуст, повтор (попытка 2: окно за экраном)..."},
        {"visible",   "PDF пуст, повтор (попытка 3: окно видно)..."},
    };
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt == 0) {
            view->setAttribute(Qt::WA_DontShowOnScreen, true);
        } else if (attempt == 1) {
            // Неудача «чистого» offscreen — даём view настоящее (но
            // вынесенное за экран) окно: на Windows именно show()
            // инициализирует нативное окно + print-конвейер.
            view->setAttribute(Qt::WA_DontShowOnScreen, false);
            view->move(-4000, -4000);
        } else {
            // Крайний случай: окно реально видно (tool-окно, без записи
            // в taskbar). Пользователь видит процесс формирования PDF.
            view->move(0, 0);
        }
        view->show();
        qInfo().noquote() << "[PDFDBG] attempt" << (attempt + 1) << ": window="
                          << kAttemptModes[attempt].mode;
        emit statusChanged(tr(kAttemptModes[attempt].status));
        printAfterFit(page, pdfData);
        if (!pdfData.isEmpty()) {
            break;
        }
        qWarning() << "[PDF] попытка" << (attempt + 1) << "(window="
                   << kAttemptModes[attempt].mode << ") пуста — следующая";
    }
    // task-16a (расширен task-15b): удаление — только когда (1) load
    // завершён и (2) прошёл grace 3с после print (внутренние операции
    // Chromium, включая висящий printToPdf после backstop, доведены до
    // конца). Уничтожаем VIEW: после setPage она владеет page,
    // page-дети умрут вместе с ней. Явный page->deleteLater() —
    // страховка (Qt 6.8: deleteLater дебаунсится через deleteLaterCalled
    // + в деструкторе removePostedEvents — повторный отложенный delete
    // отбрасывается, двойного удаления нет). QTimer привязан к view:
    // если view/хост умрут раньше — таймер умрёт с ними и lambda не
    // сработает (нет UAF). Если load так и не завершился (зависший
    // рендерер), view живёт до уничтожения родителя (parentWidget) —
    // ограниченная память, не утечка.
    auto schedulePageCleanup = [view, page]() {
        QTimer::singleShot(3000, view, [view, page] {
            qInfo() << "[PDF] временная страница экспорта удалена (load ok, grace 3s)";
            view->close();
            view->deleteLater();
            page->deleteLater();
        });
    };
    if (page->isLoading()) {
        // Load ещё идёт (например, MathJax-таймаут: документ не догрузился).
        QObject::connect(page, &QWebEnginePage::loadFinished, page,
                         [schedulePageCleanup]() { schedulePageCleanup(); },
                         Qt::SingleShotConnection);
    } else {
        schedulePageCleanup();
    }

    if (pdfData.isEmpty()) {
        qWarning() << "[PDF] printToPdf вернул пустой результат (таймаут или сбой рендеринга)";
        emit statusChanged(tr("Ошибка создания PDF"));
        return false;
    }

    // 3) Запись файла
    QString err;
    if (!writePdfFile(pdfData, filePath, &err)) {
        emit statusChanged(tr("Ошибка создания PDF: %1").arg(err));
        return false;
    }

    qInfo() << "[PDF] PDF записан:" << qPrintable(filePath) << "(" << pdfData.size() << "байт)";
    return true;
}

void WebEngineHost::clearContent()
{
    m_page->setHtml("<html><body style='display:flex;align-items:center;justify-content:center;height:100vh;color:#999;font-family:Arial,sans-serif;'><p>Контент очищен</p></body></html>");
    m_currentTaskUrl = "";
    emit statusChanged("Контент очищен");
}

void WebEngineHost::showPlaceholder(const QString &message)
{
    QString html = QString(
        "<!DOCTYPE html>"
        "<html>"
        "<head><meta charset='utf-8'><title>Math100 PDF Generator</title></head>"
        "<body style='display:flex;align-items:center;justify-content:center;height:100vh;margin:0;'"
        "font-family:Arial,sans-serif;background:#f5f5f5;'>"
        "<div style='text-align:center;color:#666;'>"
        "<h1>Math100 PDF Generator</h1>"
        "<p>%1</p>"
        "<p style='font-size:12px;color:#999;'>Введите URL варианта на math100.ru и нажмите «Загрузить»</p>"
        "</div></body></html>"
    ).arg(message);

    m_page->setHtml(html);
}

QString WebEngineHost::createFullHtml(const QString &taskHtml)
{
    QString fullHtml = R"(<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Задача</title>
    <link rel="stylesheet" href="qrc:/css/style.css">
    <script>
        window.MathJax = {
            tex: {
                inlineMath: [['\\(', '\\)']],
                displayMath: [['\\[', '\\]']],
                processEscapes: true,
                processEnvironments: true
            },
            options: {
                // task-12b: mjx-container — повторный typeset не должен
                // заглядывать внутрь готового вывода MathJax
                skipHtmlTags: ['script', 'noscript', 'style', 'textarea', 'pre', 'mjx-container']
            },
            startup: {
                ready: function() {
                    MathJax.startup.defaultReady();
                }
            }
        };
    </script>
    <!-- task-12d: MathJax только ЛОКАЛЬНЫЙ (qrc, worker-a: resources/mathjax/
         в resources.qrc, prefix /mathjax). Без CDN/loader'а/watchdog'а:
         CDN jsdelivr периодические виснет → printToPdf пустой. -->
    <script src="qrc:/mathjax/es5/tex-mml-chtml.js" async></script>
    <style>
        /* Блоки Ответ/Решение (конвертация из спойлеров math100.ru) */
        .task-answer-block { margin: 12px 0; padding: 10px 14px; background: #f7f7f7; border: 1px solid #ddd; border-radius: 6px; }
        .task-answer-label { margin: 0 0 6px; font-size: 13pt; }
        .task-answer-body p { margin: 6px 0; }
        /* task-12: поля страниц задаём ТОЛЬКО через @page (для ВСЕХ страниц,
           а не только первой — body-подложка на 2+ страницах не работает).
           padding у body при печати убран (переопределяет style.css). */
        @page { size: A4; margin: 10mm 10mm 12mm 10mm; }
        @media print {
            body { margin: 0; padding: 0; }
            .task-answer-block { background: #fff; border: 1px solid #bbb; }
        }
    </style>
</head>
<body>
    <div id="task-content">
)";

    fullHtml += taskHtml;

    fullHtml += R"(
    </div>
    <!-- task-13: mathsplit ДО app.js — app.js вызывает window.splitWideMathChains -->
    <script src="qrc:/js/mathsplit.js"></script>
    <script src="qrc:/js/app.js"></script>
    <script src="qrc:/js/pdf.js"></script>
    <script>
        document.addEventListener('DOMContentLoaded', function() {
            if (window.renderTask) {
                window.renderTask();
            }
        });
    </script>
</body>
</html>)";

    return fullHtml;
}
