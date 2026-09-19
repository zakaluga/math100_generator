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
    // task-17: Chromium print-пайплайн на Windows зависает с GPU-растеризацией
    // (printToPdf callback не приходит никогда, даже на видимом view — лог
    // пользователя 17:06-17:09, все 3 эскалации пусты). Software-растеризация
    // стабильна для печати; для этого приложения (статичный контент) цена
    // --disable-gpu пренебрежима. Убираем флаг, когда найдем корень точно.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");

    // task-18: Chromium-лог в отдельный файл рядом с exe (level 2 = INFO).
    // Если печать опять не сойдётся — в этом файле будут внутренние
    // сообщения Chromium (PrintViewManager/PrintRenderFrame/frames):
    // они видны только здесь, qInstallMessageHandler их не ловит.
    // Файл append-only и может расти — его можно удалять в любой момент.
    const QString chromiumLog = selfExeDir() + "/math100_generator_chromium.log";
    if (!chromiumLog.isEmpty()) {
        qputenv("QTWEBENGINE_CHROMIUM_LOG_FILE", chromiumLog.toUtf8().constData());
        qputenv("QTWEBENGINE_CHROMIUM_LOG_LEVEL", "2");
    }

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

    return app.exec();
}
