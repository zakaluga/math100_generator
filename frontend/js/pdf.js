/**
 * pdf.js - Генерация PDF
 * Использует window.print() для native print-to-PDF
 */

(function () {
    'use strict';

    /**
     * Генерирует PDF для текущей задачи
     */
    window.generateTaskPDF = function () {
        // Настраиваем страницу для печати
        document.body.style.margin = '0';
        document.body.style.padding = '15mm';

        window.print();
    };

    /**
     * Генерирует PDF для всех задач
     */
    window.generateAllPDF = function (taskHtmls) {
        if (!taskHtmls || taskHtmls.length === 0) {
            console.warn('No tasks to print');
            return;
        }

        // Создаём временный контейнер со всеми задачами
        var tempContainer = document.createElement('div');
        tempContainer.id = 'all-tasks-pdf';
        tempContainer.style.cssText = 'font-family: "Times New Roman", serif; font-size: 12pt; line-height: 1.5;';

        for (var i = 0; i < taskHtmls.length; i++) {
            var taskDiv = document.createElement('div');
            taskDiv.className = 'pdf-task';
            taskDiv.style.pageBreakBefore = i > 0 ? 'always' : 'auto';
            taskDiv.innerHTML = taskHtmls[i];
            tempContainer.appendChild(taskDiv);
        }

        // Заменяем контент
        var originalContent = document.getElementById('task-content');
        originalContent.innerHTML = '';
        originalContent.appendChild(tempContainer);

        // Рендерим MathJax
        if (window.renderMathJax) {
            window.renderMathJax();
        }

        // Печатаем через секунду (даём время рендерингу)
        setTimeout(function () {
            window.print();
        }, 1000);
    };

    /**
     * Восстанавливает контент после печати
     */
    window.restoreAfterPrint = function () {
        document.body.style.margin = '';
        document.body.style.padding = '';
    };

    // Слушаем событие печати
    window.addEventListener('afterprint', function () {
        window.restoreAfterPrint();
    });

})();
