// task-13: unit-тест splitWideMathChains (C++).
// Фикстуры: tests/testdata/mathsplit_cases.txt (общие с node-тестом).
// Запуск из корня проекта: ./build/mathsplit_test
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QStringList>

#include "mathsplit.h"

static QString findFixture()
{
    const QStringList candidates = {
        QStringLiteral("tests/testdata/mathsplit_cases.txt"),
        QStringLiteral("../tests/testdata/mathsplit_cases.txt"),
    };
    for (const QString &c : candidates) {
        if (QFile::exists(c)) {
            return QDir::toNativeSeparators(c);
        }
    }
    return QString();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString path = findFixture();
    if (path.isEmpty()) {
        qCritical() << "FIXTURES NOT FOUND (запускай из корня проекта: ./build/mathsplit_test)";
        return 2;
    }
    QFile f{path};
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical() << "cannot open" << path;
        return 2;
    }
    int total = 0, failed = 0;
    int lineNo = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        ++lineNo;
        const QStringList parts = QString::fromUtf8(line).split('\t');
        if (parts.size() != 3 || parts[0] != "IN") {
            qWarning() << "line" << lineNo << ": malformed, skipped";
            continue;
        }
        const QString input = parts[1];
        const QString expected = parts[2];
        const QString actual = splitWideMathChains(input);
        ++total;
        if (actual == expected) {
            qInfo().noquote() << QStringLiteral("PASS [%1]").arg(lineNo);
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("FAIL [%1]").arg(lineNo);
            qCritical() << "  input:    " << input;
            qCritical() << "  expected: " << expected;
            qCritical() << "  actual:   " << actual;
        }
    }
    if (failed == 0) {
        qInfo().noquote() << QStringLiteral("ALL PASS (%1/10)").arg(total);
        return 0;
    }
    qCritical().noquote() << QStringLiteral("FAILURES: %1/%2").arg(failed).arg(total);
    return 1;
}
