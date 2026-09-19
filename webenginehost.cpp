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
QString waitMathJaxReady(QWebEnginePage *page, int timeoutMs)
{
    QString mjStatus;
    QEventLoop loop;
    bool done = false;
    auto finish = [&loop, &done, &mjStatus](const QString &status) {
        if (done) {
            return;
        }
        done = true;
        mjStatus = status;
        loop.quit();
    };
    auto *poll = new QTimer(&loop);
    int pollCount = 0; // task-12c DEBUG (временное)
    QObject::connect(poll, &QTimer::timeout, &loop, [page, poll, finish, &pollCount]() {
        ++pollCount;
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
        )", [page, poll, finish, &pollCount](const QVariant &res) {
            // task-12c DEBUG (временное)
            qInfo().noquote() << QString("[MJWAIT] poll#%1: res=\"%2\" url=%3")
                .arg(pollCount).arg(res.toString()).arg(page->url().toString());
            if (!res.isValid() || res.toString().isEmpty()) {
                // Пустой результат (артефакт сборки) — не останавливаем poll,
                // сработает backstop-таймаут.
                return;
            }
            const QString status = res.toString();
            if (status == "ready" || status == "absent") {
                poll->stop();
                finish(status);
            }
        });
    });
    QTimer::singleShot(timeoutMs, &loop, [finish] { finish("timeout"); });
    poll->start(500);
    loop.exec();
    return mjStatus;
}

// Асинхронный printToPdf (Qt 6.8) через callback-перегрузку + локальный
// event loop; backstop-таймаут. Возвращает true, если callback пришёл.
bool waitForPrintToPdf(QWebEnginePage *page, QByteArray &pdfData, int timeoutMs)
{
    QEventLoop loop;
    bool done = false;
    QTimer backstop;
    backstop.setSingleShot(true);
    QObject::connect(&backstop, &QTimer::timeout, &loop, [&loop, &done] {
        if (!done) {
            done = true;
            loop.quit();
        }
    });
    backstop.start(timeoutMs);
    // task-12c-fix DEBUG (временное)
    QElapsedTimer dbgT;
    dbgT.start();
    qInfo().noquote() << "[PDFDBG] printToPdf: request issued url=" << page->url().toString();

    page->printToPdf([&pdfData, &loop, &done, &dbgT](const QByteArray &data) {
        if (done) {
            return;
        }
        qInfo().noquote() << "[PDFDBG] printToPdf: callback data=" << data.size()
                          << "bytes after" << dbgT.elapsed() << "ms";
        done = true;
        pdfData = data;
        loop.quit();
    });
    loop.exec();
    if (!done) {
        qInfo().noquote() << "[PDFDBG] printToPdf: BACKSTOP fired after" << dbgT.elapsed()
                          << "ms (callback не пришёл)";
    }
    return done;
}

// task-12: подгонка широких формул ПЕРЕД печатью. Выполняет на странице
// window.fitWideMath() (объявлена в app.js для превью и встраивается в
// экспорт-документы; window.__mathFitted сбрасывается на всякий случай)
// и ЖДЁТ callback runJavaScript (backstop 5s — сбой fit не останавливает
// печать), а printToPdf вызывается ВНУТРИ этого callback.
void printAfterFit(QWebEnginePage *page, QByteArray &pdfData)
{
    QEventLoop loop;
    bool started = false;
    QElapsedTimer dbgFitT;
    dbgFitT.start();
    auto doPrint = [&page, &pdfData, &loop, &started, &dbgFitT](auto &&...) {
        if (started) {
            return;
        }
        qInfo().noquote() << "[PDFDBG] printAfterFit: fit-callback path, elapsed="
                          << dbgFitT.elapsed() << "ms";
        started = true;
        waitForPrintToPdf(page, pdfData, 30000);
        loop.quit();
    };
    QTimer backstop;
    backstop.setSingleShot(true);
    QObject::connect(&backstop, &QTimer::timeout, &loop, doPrint);
    backstop.start(5000);
    page->runJavaScript(
        "window.__mathFitted=false; if (window.fitWideMath) window.fitWideMath(); 'ok'",
        doPrint);
    loop.exec();
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
    // m_page) — кэш/cookies те же, что у приложения. Родителя нет:
    // после printToPdf делаем deleteLater.
    QWebEngineProfile *profile = m_page ? m_page->profile() : QWebEngineProfile::defaultProfile();
    auto *page = new QWebEnginePage(profile);
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
    emit statusChanged(tr("Создание PDF..."));
    QByteArray pdfData;
    printAfterFit(page, pdfData);

    // task-12c: страховка от мигания Chromium — пустой результат первой
    // попытки повторяем ОДИН раз (страница ещё жива), прежде чем объявлять
    // ошибку.
    if (pdfData.isEmpty()) {
        qWarning() << "[PDF] первая попытка пуста — retry printToPdf";
        emit statusChanged(tr("PDF пуст, повторная попытка..."));
        printAfterFit(page, pdfData);
    }
    page->deleteLater(); // страница больше не нужна

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
