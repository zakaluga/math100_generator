#include "mathsplit.h"

#include <QRegularExpression>

// task-13: реализация канонического правила (см. mathsplit.h).
// Зеркало: frontend/js/mathsplit.js (поведение должно совпадать
// побайтно на фикстурах tests/testdata/mathsplit_cases.txt).

namespace {

// REL: RUN + отношение (группа 1) + RUN.
// Порядок альтернатив важен: длинные команды ПЕРЕД короткими
// (Leftrightarrow перед Leftarrow).
const char *kRelRx =
    R"((?:\\,){2,}\s*(\\(?:Leftrightarrow|Rightarrow|Leftarrow|iff|to|sim|equiv))\s*(?:\\,){2,})";

int countMatches(const QString &s, const QRegularExpression &rx)
{
    int n = 0;
    auto it = rx.globalMatch(s);
    while (it.hasNext()) {
        it.next();
        ++n;
    }
    return n;
}

} // namespace

QString splitWideMathChains(const QString &latex)
{
    if (latex.isEmpty()) {
        return latex;
    }
    // 1) \left/\right — не трогаем (fitWideMath масштабирует).
    if (latex.contains(QLatin1String("\\left"))
        || latex.contains(QLatin1String("\\right"))) {
        return latex;
    }

    static const QRegularExpression relRx{kRelRx};

    // 2) Меньше трёх шагов — не трогаем.
    if (countMatches(latex, relRx) < 2) {
        return latex;
    }

    // 3) REL -> «\\» + отношение + «\, ».
    // ВНИМАНИЕ: в C++ комментарии, кончающиеся обратным слешем, склеиваются
    // со следующей строкой (line-continuation, фаза 2 препроцессора) — такие
    // комментарии здесь ЗАПРЕЩЕНЫ: они глотают следующий код молча.
    QString out;
    int last = 0;
    auto it = relRx.globalMatch(latex);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += latex.mid(last, m.capturedStart() - last);
        out += QLatin1String("\\\\");    // перенос строки (два слеша)
        out += m.captured(1);            // отношение в начале строки
        out += QLatin1String("\\, ");    // тонкий пробел
        last = m.capturedEnd();
    }
    out += latex.mid(last);
    return out;
}
