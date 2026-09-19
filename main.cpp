#include <QApplication>
#include <QStyleFactory>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QSysInfo>
#include <QMutex>
#include "mainwindow.h"

#include "webenginehost.h"

#include <QWebEnginePage>
#include <QWebEngineView>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>

#include <cstdio>

#if defined(Q_OS_WIN)
#  include <windows.h>
#elif defined(Q_OS_UNIX)
#  include <unistd.h>
#endif

// task-15a: file-logging.
// На Windows это GUI-приложение (WIN32_EXECUTABLE), stderr невидим — чтобы видеть,
// что происходило до падения, все сообщения Qt пишем в math100_generator.log:
//   - ленивое открытие файла при первом сообщении;
//   - путь: сначала рядом с exe (если каталог доступен на запись), иначе в TempLocation;
//   - режим APPEND, если файл > ~10 МБ — переоткрываем с truncate (простая ротация);
//   - все уровни (debug..fatal); fatal пишем в лог и НЕ вызываем abort/exit.
namespace {

constexpr qint64 kLogMaxBytes = 10 * 1024 * 1024; // ~10 МБ

QMutex &logMutex() { static QMutex m; return m; }
QFile  &logFile()  { static QFile f;  return f; }
QString &logPath() { static QString p; return p; }

// Каталога, где лежит exe. QCoreApplication::applicationDirPath() пуст, пока
// не создан экземпляр QApplication, а первые сообщения (например, предупреждение
// о локали) приходят ещё в конструкторе — поэтому в этом случае определяем
// путь напрямую: /proc/self/exe (POSIX) или GetModuleFileName (Windows).
QString selfExeDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
        return appDir;

#if defined(Q_OS_WIN)
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return QFileInfo(QString::fromWCharArray(buf, static_cast<int>(n))).absolutePath();
#elif defined(Q_OS_UNIX)
    char buf[4096] = {0};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return QFileInfo(QString::fromLocal8Bit(buf)).absolutePath();
    }
#endif
    return QString();
}

// Определяет путь к лог-файлу:
// 1) рядом с exe, если каталог существует, доступен на запись и тестовое
//    открытие файла проходит;
// 2) иначе — QStandardPaths::TempLocation.
QString resolveLogPath()
{
    const QString fileName = QStringLiteral("math100_generator.log");
    const QString appDir = selfExeDir();
    if (!appDir.isEmpty()) {
        const QDir dir(appDir);
        const QFileInfo dirInfo(appDir);
        if (dir.exists() && dirInfo.isWritable()) {
            const QString candidate = appDir + '/' + fileName;
            QFile test(candidate);
            if (test.open(QIODevice::ReadWrite | QIODevice::Append)) {
                test.close();
                return candidate;
            }
        }
    }
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return tempDir + '/' + fileName;
}

// Вызывается с захваченным logMutex(). Лениво открывает лог-файл (APPEND,
// с простой ротацией: файл > kLogMaxBytes — переоткрываем с Truncate).
bool openLogFileLocked()
{
    if (logFile().isOpen())
        return true;

    logPath() = resolveLogPath();

    const QFileInfo fi(logPath());
    const bool rotate = fi.exists() && fi.size() > kLogMaxBytes;

    QFile::OpenMode mode = QIODevice::WriteOnly;
    mode |= rotate ? QIODevice::Truncate : QIODevice::Append;

    logFile().setFileName(logPath());
    if (!logFile().open(mode))
        return false;
    return true;
}

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO";
    case QtWarningMsg:  return "WARNING";
    case QtCriticalMsg: return "CRITICAL";
    case QtFatalMsg:    return "FATAL";
    }
    return "UNKNOWN";
}

void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)

    // Формат: "[HH:mm:ss.zzz] <LEVEL> <message>" + перевод строки. Локальное время.
    const QString line = QStringLiteral("[%1] %2 %3\n")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             QLatin1String(levelName(type)),
             msg);
    const QByteArray data = line.toUtf8();

    {
        QMutexLocker locker(&logMutex());
        if (openLogFileLocked()) {
            logFile().write(data);
            logFile().flush(); // чтобы строка гарантированно была видна после падения
        }
    }

    // Консольные платформы: дублируем в stderr, чтобы не потерять привычный вывод
    // (на Windows GUI stderr невидим — основной носитель информации здесь файл).
    std::fputs(data.constData(), stderr);
    std::fflush(stderr);

    // QtFatalMsg: по умолчанию Qt вызывает abort() после обработчика.
    // Мы лог записали — и всё, ничего не делаем (без exit/abort).
}

} // namespace

int main(int argc, char *argv[])
{
    // task-17: Chromium print-пайплайн на Windows зависает (printToPdf
    // callback не приходит никогда — логи пользователя 17:06/21:10).
    // Software-растеризация стабильна для печати; цена --disable-gpu для
    // этого приложения (статичный контент) пренебрежима.
    // task-19: + Chromium-лог в файл рядом с exe. ВАЖНО: env
    // QTWEBENGINE_CHROMIUM_LOG_FILE в Qt 6.8/6.10 файл НЕ создаёт
    // (проверено selftest'ом) — работает только нативный Chromium-флаг
    // --log-file (base::logging). --log-level=0 = INFO и выше: в этом
    // уровне пишет print-пайплайн (PrintViewManager/PrintRenderFrame).
    // Файл append-only и может расти — его можно удалять в любой момент.
    const QString chromiumLog = selfExeDir() + "/math100_generator_chromium.log";
    // Если env уже задан (отладка) — НЕ затираем, а дописываем свои флаги.
    QString chromiumFlags = qEnvironmentVariable("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!chromiumFlags.isEmpty()) {
        chromiumFlags += QLatin1Char(' ');
    }
    // task-19d: отключаемо для отладки (MATH100_NO_DISABLEGPU=1): в
    // offscreen-окружении (headless-контейнер) printToPdf с --disable-gpu
    // виснет, а без него работает — подозреваем именно этот флаг.
    if (!qEnvironmentVariableIsSet("MATH100_NO_DISABLEGPU")) {
        chromiumFlags += QStringLiteral("--disable-gpu");
    }
    if (!chromiumLog.isEmpty()) {
        chromiumFlags += QStringLiteral(" --log-file=%1 --log-level=0").arg(chromiumLog);
        // на всякий случай оставляем и Qt-env (на других сборках может сработать)
        qputenv("QTWEBENGINE_CHROMIUM_LOG_FILE", chromiumLog.toUtf8().constData());
        qputenv("QTWEBENGINE_CHROMIUM_LOG_LEVEL", "2");
    }
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", chromiumFlags.toUtf8().constData());

    // Устанавливаем ДО создания QApplication, чтобы не потерять сообщения,
    // появляющиеся ещё на этапе инициализации.
    qInstallMessageHandler(fileMessageHandler);

    QApplication app(argc, argv);
    app.setApplicationName("Math100 PDF Generator");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Math100Gen");
    app.setStyle(QStyleFactory::create("Fusion"));

    // Стартовая строка в логе (идёт через handler => ленивое открытие файла).
    qInfo().noquote()
        << QStringLiteral("[start] Qt version: %1 | platform: %2 | os: %3 | pid: %4 | exe: %5")
               .arg(qVersion(),
                    QGuiApplication::platformName(),
                    QSysInfo::prettyProductName(),
                    QString::number(QCoreApplication::applicationPid()),
                    QCoreApplication::applicationFilePath());

    MainWindow window;
    window.show();

    // task-19: print-selftest (env MATH100_SELFTEST_PRINT=[путь.pdf]).
    // Прогоняет весь экспортный print-путь (setHtml → waitForPageLoad →
    // waitMathJaxReady → fitWideMath → printToPdf → восстановление превью)
    // на маленьком документе с MathJax — то же самое, что делает экспорт,
    // без UI и без сети. 0 = PDF создан и не пустой.
    if (qEnvironmentVariableIsSet("MATH100_SELFTEST_PRINT")) {
        const QString envVal = qEnvironmentVariable("MATH100_SELFTEST_PRINT");
        const QString outPath = envVal.isEmpty()
            ? QDir::tempPath() + "/selftest_print.pdf"
            : envVal;
        // Математика/конфиг MathJax — как в экспортном документе
        // (NetworkManager::buildExportPageHtml).
        QString testHtml = QStringLiteral(R"SELFTEST_HTML(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<title>Selftest</title>
<script>
    window.MathJax = {
        tex: { inlineMath: [['\\(', '\\)']], displayMath: [['\\[', '\\]']],
               processEscapes: true, processEnvironments: true },
        options: { skipHtmlTags: ['script', 'noscript', 'style', 'textarea', 'pre'] },
        startup: {
            ready: function() {
                MathJax.startup.defaultReady();
                MathJax.startup.promise.then(function() {
                    if (window.fitWideMath) window.fitWideMath();
                });
            }
        }
    };
</script>
<script src="qrc:/mathjax/es5/tex-mml-chtml.js" async></script>
<style>
    body { font-family: 'Times New Roman', Times, serif; font-size: 16pt; margin: 0; padding: 0; }
    @page { size: A4; margin: 10mm 10mm 12mm 10mm; }
</style>
</head>
<body>
<h2>Selftest-документ</h2>
<p>Формула: \[ x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a} \]</p>
<p>Вторая: \( \int_0^\infty e^{-x^2} dx = \frac{\sqrt{\pi}}{2} \)</p>
</body>
</html>
)SELFTEST_HTML");
        // MATH100_SELFTEST_PLAIN=1 — документ БЕЗ MathJax (диагностика:
        // работает ли printToPdf вообще в этой среде).
        const bool plain = qEnvironmentVariableIsSet("MATH100_SELFTEST_PLAIN");
        if (plain) {
            testHtml = QStringLiteral("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<style>body{font-family:Arial,sans-serif;font-size:16pt;}</style></head>"
                "<body><h1>Plain selftest</h1><p>1 2 3 4 5</p></body></html>");
        }

        // MATH100_SELFTEST_MINIMAL=1 — диагностика: ТОЧНО минимальный flow
        // (setHtml about:blank → loadFinished → printToPdf callback) на
        // РЕАЛЬНОЙ странице приложения (view внутри видимого окна).
        // Отделяет «окружение приложения» от «потока printHtmlToPdf».
        if (qEnvironmentVariableIsSet("MATH100_SELFTEST_MINIMAL")) {
            QWebEnginePage *page = window.webEngine()->view()->page();
            const QString minDoc = QStringLiteral("<html><head><meta charset='utf-8'></head>"
                "<body style='font-family:Arial;font-size:20pt'><h1>Minimal</h1>"
                "<p>print test</p></body></html>");
            page->setHtml(minDoc, QUrl("about:blank"));
            QEventLoop loadLoop;
            QObject::connect(page, &QWebEnginePage::loadFinished, &loadLoop,
                             [&loadLoop](bool) { loadLoop.quit(); });
            QTimer::singleShot(15000, &loadLoop, [&loadLoop] { loadLoop.quit(); });
            loadLoop.exec();
            qInfo().noquote() << "[SELFTEST] minimal: loaded, url=" << page->url().toString();
            QEventLoop printLoop;
            bool got = false;
            long size = -1;
            QElapsedTimer pt;
            pt.start();
            page->printToPdf([&](const QByteArray &data) {
                size = (long)data.size();
                got = data.size() > 0;
                printLoop.quit();
            });
            QTimer::singleShot(30000, &printLoop, [&printLoop] { printLoop.quit(); });
            printLoop.exec();
            qInfo().noquote() << "[SELFTEST] minimal print:" << (got ? "OK" : "HANG")
                              << "size=" << size << "after" << pt.elapsed() << "ms";
            return got ? 0 : 1;
        }

        // MATH100_SELFTEST_SYNC=1 — диагностика: СТАРАЯ синхронная
        // printToPdf(QByteArray*) (deprecated, блокирующая) вместо асинхронной
        // callback-версии. Если она работает, а callback-версия виснет —
        // баг именно в async-обёртке этой сборки Qt.
        if (qEnvironmentVariableIsSet("MATH100_SELFTEST_SYNC")) {
            QWebEnginePage *page = window.webEngine()->view()->page();
            page->setHtml(testHtml, QUrl("about:blank"));
            QEventLoop loadLoop;
            QObject::connect(page, &QWebEnginePage::loadFinished, &loadLoop,
                             [&loadLoop](bool) { loadLoop.quit(); });
            QTimer::singleShot(30000, &loadLoop, [&loadLoop] { loadLoop.quit(); });
            loadLoop.exec();
            qInfo().noquote() << "[SELFTEST] sync: doc loaded, url="
                              << page->url().toString();
            // ВАРИАНТ A: перегрузка printToPdf(filePath) — пишет PDF
            // напрямую в файл БЕЗ callback (если callback-механика
            // сломана, это может обойти баг). Ждём появления файла.
            QFile::remove(outPath);
            page->printToPdf(outPath);
            qInfo().noquote() << "[SELFTEST] printToPdf(filePath) issued, waiting for file...";
            for (int i = 0; i < 120; ++i) {
                QThread::msleep(1000);
                QFile f(outPath);
                if (f.exists() && f.size() > 500) {
                    qInfo().noquote() << "[SELFTEST] file appeared, size=" << f.size()
                                      << "after" << (i + 1) << "s";
                    return 0;
                }
            }
            qWarning() << "[SELFTEST] printToPdf(filePath): file did not appear in 120s";
            return 1;
        }
        qInfo().noquote() << "[SELFTEST] start, out=" << outPath
                          << (plain ? "mode=plain" : "mode=mathjax");
        const bool ok = window.webEngine()->printHtmlToPdf(testHtml, outPath, plain ? 60 : 120);
        QFile f(outPath);
        const qint64 size = f.exists() ? f.size() : -1;
        qInfo().noquote() << "[SELFTEST] result ok=" << (ok ? "true" : "false")
                          << "size=" << size << "file=" << outPath;
        return (ok && size > 500) ? 0 : 1;
    }

    return app.exec();
}
