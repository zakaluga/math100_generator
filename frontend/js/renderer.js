/**
 * renderer.js - Дополнительные рендереры для формул
 * Используется как fallback если MathJax не справляется
 */

(function () {
    'use strict';

    /**
     * Рендерит все формулы в контейнере
     */
    window.renderAllFormulas = function (container) {
        if (!container) return;

        // Находим все элементы с формулами
        var mathElements = container.querySelectorAll('.math-formula, span.math, .formula');

        for (var i = 0; i < mathElements.length; i++) {
            var el = mathElements[i];
            var latex = el.textContent || el.innerText;

            if (!latex || latex.trim() === '') continue;

            // Если это ещё не рендернутая формула
            if (!el.getAttribute('data-rendered')) {
                el.setAttribute('data-rendered', 'true');
            }
        }
    };

    /**
     * Предпросмотр формулы (для тестирования)
     */
    window.previewFormula = function (latex) {
        var div = document.createElement('div');
        div.className = 'formula-preview';
        div.style.cssText = 'text-align:center; margin:10px; padding:10px; background:#f9f9f9; border:1px solid #ddd;';
        div.innerHTML = '\\[' + latex + '\\]';
        document.body.appendChild(div);

        if (window.MathJax && MathJax.typesetPromise) {
            MathJax.typesetPromise([div]).catch(function (err) {
                console.warn('Formula preview error:', err);
                div.innerHTML = '<span style="color:red;">Error: ' + latex + '</span>';
            });
        }

        return div;
    };

})();
