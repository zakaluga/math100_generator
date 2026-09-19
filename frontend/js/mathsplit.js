/**
 * task-13: перенос длинных цепочек формул на новые строки (PREVIEW).
 *
 * ЗЕРКАЛО mathsplit.cpp (каноническое правило — оно же описано в
 * mathsplit.h). Фикстуры/тесты: tests/testdata/mathsplit_cases.txt,
 * tests/mathsplit.test.js (node) — результат на них должен совпадать
 * побайтно с C++.
 *
 * window.splitWideMathChains(latex) вызывается ТОЛЬКО для формул,
 * которые рендерятся как display (\[ … \]) — в них перенос «\\»
 * допустим. Для inline формул (в тексте предложения) НЕ вызывать.
 *
 * Правило:
 *   1) Формула содержит \left или \right — не трогаем (fitWideMath).
 *   2) REL = RUN + (пробелы) + отношение (\Leftrightarrow \Rightarrow
 *      \Leftarrow \iff \to \sim \equiv) + (пробелы) + RUN, где RUN —
 *      >= 2 «\,» подряд. Если REL < 2 — не трогаем.
 *   3) Каждое REL -> перенос «\\» + отношение + тонкий пробел «\,».
 * Остальные группы «\,» (без отношения) НЕ трогаем — это пометки
 * операций сайта (| …).
 */
(function () {
    "use strict";

    var REL_RX =
        /(?:\\,){2,}\s*(\\(?:Leftrightarrow|Rightarrow|Leftarrow|iff|to|sim|equiv))\s*(?:\\,){2,}/g;
    var REL_COUNT_RX = new RegExp(REL_RX.source, "g");

    window.splitWideMathChains = function (latex) {
        if (!latex || latex.length === 0) {
            return latex;
        }
        if (latex.indexOf("\\left") !== -1 || latex.indexOf("\\right") !== -1) {
            return latex;
        }
        var rels = (latex.match(REL_COUNT_RX) || []).length;
        if (rels < 2) {
            return latex;
        }
        return latex.replace(REL_RX, function (match, rel) {
            // «\\» (перенос) + отношение в начале строки + тонкий пробел.
            return "\\\\" + rel + "\\, ";
        });
    };
})();
