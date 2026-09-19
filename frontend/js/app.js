/**
 * app.js - Основная логика отображения задач math100.ru
 *
 * Парсит HTML, обрабатывает формулы MathJax, изображения и структуру.
 */

(function () {
    'use strict';

    // task-12: КАНОНИЧЕСКИЙ fitWideMath. ПОСЛОВНО совпадает с копией, которую
    // worker-a встраивает в экспорт-документы (networkmanager) и в standalone
    // HTML консоли (console_main.cpp). Сжимает display-формулы MathJax CHTML,
    // не помещающиеся в ширину колонки (CHTML сам не умеет их уменьшать).
    // Идемпотентна: повторный вызов без изменений ничего не делает, а если
    // формула снова влезает (например, после resize) — снимает обёртку.
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

    // Глобальная функция для C++ - вызывает loadTaskPage из C++
    window.renderTask = function () {
        try {
            window.__tl = window.__tl || {};
            window.__tl.tRenderTask = Math.round(performance.now());
        } catch (e) {}
        var container = document.getElementById('task-content');
        if (!container) return;

        // Если есть готовый HTML от C++, используем его
        if (window.__taskHtml) {
            container.innerHTML = window.__taskHtml;
        }

        // Если контента ещё нет (например, loaded via setContent),
        // проверяем, не пустой ли контейнер
        if (container.children.length === 0 || 
            (container.children.length === 1 && container.firstChild.tagName === 'DIV' && 
             container.firstChild.children.length === 0)) {
            // Контент уже в DOM от C++ (через createFullHtml)
            // Ничего делать не нужно, обрабатываем текущий контент
        }

        // Обрабатываем контент
        processContent(container);
    };

    /**
     * Обрабатывает весь контент задачи
     */
    function processContent(container) {
        if (!container) return;

        // 1. Извлекаем post-content если есть
        var postContent = container.querySelector('.post-content');
        if (postContent) {
            // Копируем контент post-content внутрь container
            while (postContent.firstChild) {
                container.appendChild(postContent.firstChild);
            }
            postContent.style.display = 'none';
        }

        // 2. Убираем спойлеры
        removeSpoilers(container);

        // 3. Убираем рекламу
        removeAds(container);

        // 4. Обрабатываем изображения
        processImages(container);

        // 5. Обрабатываем формулы MathJax
        processMath(container);

        // 6. Форматируем текст
        formatText(container);

        // 7. Рендерим MathJax
        renderMathJax();
    }

    /**
     * Удаляет спойлеры (скрытые ответы/решения)
     */
    function removeSpoilers(container) {
        var spoilers = container.querySelectorAll('.math100-spoiler, [data-spoiler]');
        for (var i = 0; i < spoilers.length; i++) {
            spoilers[i].style.display = 'none';
        }

        // Также убираем div с классом spoiler
        var allDivs = container.querySelectorAll('div');
        for (var j = 0; j < allDivs.length; j++) {
            var classes = allDivs[j].getAttribute('class') || '';
            if (classes.indexOf('spoiler') !== -1 ||
                classes.indexOf('math100-answer') !== -1 ||
                classes.indexOf('math100-solution') !== -1) {
                allDivs[j].style.display = 'none';
            }
        }
    }

    /**
     * Удаляет рекламные блоки
     */
    function removeAds(container) {
        var adSelectors = [
            '.adsbygoogle',
            '.math100-promo-bar',
            '.m100-cookie-banner',
            '.cookie-banner',
            '.ad-container',
            'iframe',
            '.math100-promo',
            '#math100-promo'
        ];

        for (var i = 0; i < adSelectors.length; i++) {
            var elements = container.querySelectorAll(adSelectors[i]);
            for (var j = 0; j < elements.length; j++) {
                elements[j].style.display = 'none';
            }
        }

        // Убираем скрипты
        var scripts = container.querySelectorAll('script');
        for (var k = 0; k < scripts.length; k++) {
            scripts[k].style.display = 'none';
        }
    }

    /**
     * Обрабатывает изображения
     */
    function processImages(container) {
        var images = container.querySelectorAll('img');
        for (var i = 0; i < images.length; i++) {
            var img = images[i];
            var src = img.getAttribute('src');

            // Пропускаем пустые src
            if (!src || src.trim() === '') {
                img.style.display = 'none';
                continue;
            }

            // Пропускаем data:image (встроенные)
            if (src.indexOf('data:image') === 0) {
                continue;
            }

            // Обрезажаем path для локальных файлов
            if (src.indexOf('/images/') !== -1) {
                // Это изображение с math100.ru - оно уже в кэше через C++
                // Можно оставить как есть
            }

            // Ограничиваем размер изображений
            img.style.maxWidth = '90%';
            img.style.height = 'auto';
            img.style.display = 'block';
            img.style.margin = '8px auto';
        }
    }

    /**
     * task-12: является ли формула «блочной» по типографии сайта:
     * ближайший блочный предок (P/DIV/BLOCKQUOTE) центрирован
     * (text-align: center — так math100.ru оформляет блочные формулы)
     * либо формула(ы) — единственный видимый контент блока.
     * Аналог правила экспорта «полной формы» (convertMathSpansToDisplay):
     * такие формулы должны рендериться display, чтобы fitWideMath
     * (обрабатывает только mjx-container[display="true"]) мог
     * масштабировать широкие из них под ширину колонки.
     */
    function shouldBecomeDisplayMath(span) {
        var p = span.parentNode;
        var guard = 0;
        while (p && p.nodeType === 1 && guard++ < 8) {
            var t = p.tagName.toUpperCase();
            if (t === 'P' || t === 'DIV' || t === 'BLOCKQUOTE') {
                break;
            }
            p = p.parentNode;
        }
        if (!p || p.nodeType !== 1) {
            return false;
        }
        var tag = p.tagName.toUpperCase();
        if (tag !== 'P' && tag !== 'DIV' && tag !== 'BLOCKQUOTE') {
            return false;
        }
        // 1) Центрированный блок — блочная формула по типографии сайта
        try {
            if (getComputedStyle(p).textAlign === 'center') {
                return true;
            }
        } catch (e) {
            // среда без getComputedStyle — переходим к правилу 2
        }
        // 2) Единственный видимый контент блока: текст блока за вычетом
        //    текста всех math-спанов/уже сконвертированных .math-formula
        //    (единицы измерения вроде « м/с» после формулы блок НЕ делают
        //    нецентрованным правилом — но и «не единственным» контентом;
        //    такие случаи на практике центрируются правилом 1)
        var rest = p.textContent || '';
        var mathNodes = p.querySelectorAll('span.math, .math-formula');
        for (var i = 0; i < mathNodes.length; i++) {
            var txt = mathNodes[i].textContent || '';
            if (!txt) {
                continue;
            }
            var idx = rest.indexOf(txt);
            if (idx >= 0) {
                rest = rest.slice(0, idx) + rest.slice(idx + txt.length);
            }
        }
        return rest.replace(/\s/g, '') === '';
    }

    /**
     * Обрабатывает LaTeX формулы
     */
    function processMath(container) {
        // Находим все span.math
        var mathSpans = container.querySelectorAll('span.math');
        var pmReport = (window.__pmReports = window.__pmReports || []);
        for (var i = 0; i < mathSpans.length; i++) {
            var span = mathSpans[i];

            // task-12b: ГОНОЧКА С АВТО-TYPESET'ом MATHJAX. Порядок DCL-listener'ов
            // не определён (CDN-latency скрипта MathJax vs синхронные body-скрипты):
            // MathJax иногда typeset'ит span.math РАНЬШЕ processMath. Тогда в span
            // уже лежит готовый mjx-container, а span.textContent — это РЕНДЕРЕНЫЙ
            // текст (не LaTeX): перечитывать его и переобёртывать в делимитаторы
            // нельзя — родитель span в блочных формулах сайта — SPAN, и формула
            // навсегда останется inline, а fitWideMath (обрабатывает только
            // mjx-container[display="true"]) её не масштабирует (дефект: широкие
            // формулы solutions переполняли страницу, неопределённо по задачам).
            // Поднимаем готовый контейнер в .math-formula КАК ЕСТЬ: display-статус
            // и рендер сохраняются, повторный typeset MathJax контейнер пометит
            // как processed и пропустит; fitWideMath подхватит через
            // MutationObserver/typeset-цепочку. В норме (typeset позже
            // processMath) контейнера ещё нет — идёт обычный путь ниже.
            var existingMjx = span.querySelector
                ? span.querySelector('mjx-container') : null;
            if (existingMjx && span.parentNode) {
                window.__liftedMjx = (window.__liftedMjx || 0) + 1;
                var lifted = document.createElement('div');
                lifted.className = 'math-formula';
                lifted.style.textAlign = 'center';
                lifted.style.margin = '6px 0';
                lifted.style.fontSize =
                    getComputedStyle(span).fontSize || '11pt';
                span.parentNode.replaceChild(lifted, span);
                lifted.appendChild(existingMjx);
                continue;
            }

            var latex = span.textContent || span.innerText;

            if (!latex || latex.trim() === '') {
                continue;
            }

            // Очищаем LaTeX от HTML-сущностей
            latex = decodeHtmlEntities(latex).trim();

            // Создаём div с формулой для MathJax
            var mathDiv = document.createElement('div');
            mathDiv.className = 'math-formula';
            mathDiv.style.textAlign = 'center';
            mathDiv.style.margin = '6px 0';
            mathDiv.style.fontSize = getComputedStyle(span).fontSize || '11pt';

            // Если текст уже окружён делимитаторами MathJax (\(…\) или \[…\]) —
            // переносим его как есть, без дообёртки. Иначе внутри отрисованной
            // формулы видны литеральные \( и \) (двойная обёртка).
            var isInlineWrapped = latex.length >= 4 &&
                latex.charAt(0) === '\\' && latex.charAt(1) === '(' &&
                latex.charAt(latex.length - 2) === '\\' && latex.charAt(latex.length - 1) === ')';
            var isDisplayWrapped = latex.length >= 4 &&
                latex.charAt(0) === '\\' && latex.charAt(1) === '[' &&
                latex.charAt(latex.length - 2) === '\\' && latex.charAt(latex.length - 1) === ']';

            if (latex.length >= 100) {
                pmReport.push({
                    len: latex.length,
                    start: latex.slice(0, 3),
                    inlineWrapped: isInlineWrapped,
                    displayWrapped: isDisplayWrapped,
                    early: !!span.__fitConverted
                });
            }
            // task-13: цепочки (\,\,\, ⇔ \,\,\, …) -> по строке на шаг.
            // Только там, где формула становится display (\[ … \]) — перенос
            // «\\» в inline (в тексте предложения) не допустим. Зеркало
            // mathsplit.cpp (export): те же правила и фикстуры.
            var toDisplay = function (l) {
                return window.splitWideMathChains ? window.splitWideMathChains(l) : l;
            };

            if (isInlineWrapped) {
                if (shouldBecomeDisplayMath(span)) {
                    // task-12: блочная формула (центрированный блок / весь
                    // контент блока) — инлайн-делимитаторы в display:
                    // только так fitWideMath может масштабировать широкую
                    // формулу под ширину колонки (как в экспорте «полной
                    // формы»). Узкие формулы внутри текста остаются inline.
                    mathDiv.innerHTML = toDisplay(latex).replace(/^\\\(/, '\\[')
                                             .replace(/\\\)$/, '\\]');
                } else {
                    // Делимитаторы уже есть, формула inline — как есть
                    mathDiv.innerHTML = latex;
                }
            } else if (isDisplayWrapped) {
                // Делимитаторы уже есть — как есть (display: перенос допустим)
                mathDiv.innerHTML = toDisplay(latex);
            } else {
                // Определяем блочная это формула или inline
                var parent = span.parentNode;
                if (parent && (parent.tagName === 'P' || parent.tagName === 'DIV' || parent.tagName === 'BLOCKQUOTE')) {
                    // Блочная формула
                    mathDiv.innerHTML = '\\[' + toDisplay(latex) + '\\]';
                } else {
                    // Inline формула
                    mathDiv.innerHTML = '\\(' + latex + '\\)';
                }
            }

            span.parentNode.replaceChild(mathDiv, span);
        }

        // Также ищем другие паттерны формул
        var allElements = container.querySelectorAll('*');
        for (var j = 0; j < allElements.length; j++) {
            var el = allElements[j];
            var text = el.textContent || '';

            // Ищем формулы в формате $...$ или $$...$$
            if (text.indexOf('$') !== -1) {
                var html = el.innerHTML || '';
                // Заменяем $...$ на \(...\)
                html = html.replace(/\$([^\$]+)\$/g, '\\($1\\)');
                // Заменяем $$...$$ на \[...\]
                html = html.replace(/\$\$([^\$]+)\$\$/g, '\\[$1\\]');
                el.innerHTML = html;
            }
        }
    }

    /**
     * Форматирует текстовые элементы
     */
    function formatText(container) {
        // Убираем лишние пустые параграфы
        var paragraphs = container.querySelectorAll('p');
        for (var i = 0; i < paragraphs.length; i++) {
            var p = paragraphs[i];
            var text = (p.textContent || '').trim();

            // Удаляем пустые параграфы
            if (text === '') {
                p.style.display = 'none';
                continue;
            }

            // Удаляем параграфы только с неразрывными пробелами
            if (/^[\s\u00a0]+$/.test(text)) {
                p.style.display = 'none';
            }

            // Убираем лишние переносы строк
            p.innerHTML = p.innerHTML.replace(/<br\s*\/?>/g, ' ');
        }

        // Форматируем заголовки
        var headings = container.querySelectorAll('h1, h2, h3, h4');
        for (var j = 0; j < headings.length; j++) {
            var h = headings[j];
            var hText = (h.textContent || '').trim();

            // Убираем "Задача N" заголовки - они дублируются из QListWidget
            if (hText.match(/^Задача\s+\d+/i)) {
                h.style.display = 'none';
                continue;
            }

            if (hText === '') {
                h.style.display = 'none';
            }
        }

        // Форматируем списки
        var lists = container.querySelectorAll('ul, ol');
        for (var k = 0; k < lists.length; k++) {
            lists[k].style.marginLeft = '20px';
        }

        // Форматируем таблицы
        var tables = container.querySelectorAll('table');
        for (var l = 0; l < tables.length; l++) {
            tables[l].style.borderCollapse = 'collapse';
            tables[l].style.width = '100%';
            tables[l].style.fontSize = '10pt';

            var cells = tables[l].querySelectorAll('td, th');
            for (var m = 0; m < cells.length; m++) {
                cells[m].style.border = '1px solid #ccc';
                cells[m].style.padding = '4px 8px';
                cells[m].style.textAlign = 'left';
            }
        }
    }

    /**
     * task-12: РАННИЙ проход display-конвертации. Выполняется СИНХРОННО при
     * выполнении скрипта (контент тела уже распарсен, а авто-typeset MathJax
     * случается не раньше DOMContentLoaded — listener MathJax мог
     * зарегистрироваться РАНЬШЕ listener'ов renderTask, тогда MathJax
     * отрендерил бы исходные span.math раньше processMath и широкие
     * inline-формулы навсегда остались бы inline — fitWideMath их не
     * масштабирует). Поэтому делимитаторы переписываем на месте, ещё до
     * DOMContentLoaded: \(…\) → \[…\] только для блочных по типографии
     * сайта формул (см. shouldBecomeDisplayMath). processMath потом
     * просто перенесёт уже display-окружённый текст в .math-formula.
     */
    function convertDisplayMathSpansEarly() {
        var spans = document.querySelectorAll('span.math');
        var converted = 0;
        var report = { total: spans.length, converted: 0, wideInline: [], detail: [] };
        for (var i = 0; i < spans.length; i++) {
            var span = spans[i];
            var latex = (span.textContent || '').trim();
            var isInlineWrapped = latex.length >= 4 &&
                latex.charAt(0) === '\\' && latex.charAt(1) === '(' &&
                latex.charAt(latex.length - 2) === '\\' &&
                latex.charAt(latex.length - 1) === ')';
            if (latex.length >= 100) {
                var p = span.parentNode;
                var chain = [];
                for (var g = 0; p && p.nodeType === 1 && g < 8; g++, p = p.parentNode) {
                    var tg = p.tagName.toUpperCase();
                    var ta = '?';
                    try { ta = getComputedStyle(p).textAlign; } catch (e) {}
                    chain.push(tg + ':' + ta);
                    if (tg === 'P' || tg === 'DIV' || tg === 'BLOCKQUOTE') {
                        break;
                    }
                }
                report.detail.push({
                    len: latex.length,
                    inlineWrapped: isInlineWrapped,
                    should: isInlineWrapped ? shouldBecomeDisplayMath(span) : 'n/a',
                    chain: chain.join('|')
                });
            }
            if (!isInlineWrapped) {
                continue;
            }
            if (shouldBecomeDisplayMath(span)) {
                span.textContent = latex.replace(/^\\\(/, '\\[')
                                        .replace(/\\\)$/, '\\]');
                span.__fitConverted = true;
                ++converted;
            } else if (latex.length >= 100) {
                report.wideInline.push(latex.slice(0, 40));
            }
        }
        report.converted = converted;
        try {
            report.tEarly = Math.round(
                (window.performance && performance.now) ? performance.now() : 0);
        } catch (e) {
            report.tEarly = -1;
        }
        window.__earlyFit = report;
        return report;
    }

    /**
     * Fallback: MathJax не загрузился (CDN недоступен) — убираем делимитаторы
     * из .math-formula, чтобы пользователь видел читаемый LaTeX-текст без \( \)
     */
    function stripMathDelimiters() {
        var divs = document.querySelectorAll('.math-formula');
        for (var i = 0; i < divs.length; i++) {
            var html = divs[i].innerHTML;
            var stripped = html
                .replace(/\\\(([\s\S]*?)\\\)/g, '$1')
                .replace(/\\\[([\s\S]*?)\\\]/g, '$1');
            if (stripped !== html) {
                divs[i].innerHTML = stripped;
            }
        }
    }

    /**
     * task-12: ожидает готовности MathJax (состояние TYPESET) и после него
     * вызывает fitWideMath. Fallback на случай, когда typeset выполняет САМ
     * MathJax при старте (не через нашу renderMathJax/typesetPromise-цепочку):
     * опрашиваем MathJax.startup.document.state() каждые 200ms (до 30 с) и
     * подгоняем формулы при каждом переходе в TYPESET (повторные typeset
     * тоже подхватываются). fitWideMath идемпотентна.
     */
    var fitWatchRunning = false;
    function scheduleFitAfterTypeset() {
        if (fitWatchRunning) {
            return;
        }
        fitWatchRunning = true;
        var attempt = 0;
        var lastState = -1;
        var check = function () {
            attempt++;
            var mj = window.MathJax;
            var state = -1;
            try {
                if (mj && mj.startup && mj.startup.document && mj.STATE) {
                    state = mj.startup.document.state();
                }
            } catch (e) {
                state = -1;
            }
            if (state !== -1 && state === mj.STATE.TYPESET &&
                lastState !== mj.STATE.TYPESET && window.fitWideMath) {
                window.fitWideMath();
            }
            lastState = state;
            if (attempt >= 150) {
                // 30 с ожидания — дальше не трогаем (C++ перед печатью
                // принудительно повторит fit через runJavaScript)
                fitWatchRunning = false;
                return;
            }
            setTimeout(check, 200);
        };
        check();
    }

    /**
     * task-12: MutationObserver — как только MathJax (пере)вставил
     * mjx-container, подгоняем широкие формулы СИНХРОННО в callback'е
     * (микрозадача — раньше ЛЮБЫХ ожидающих в очереди задач страницы, в
     * т.ч. диагностических runJavaScript). Без троттлинга: MathJax
     * пере-рендерит формулы при загрузке шрифтов (woff2) — это сбрасывает
     * наш scale, и синхронная повторная подгонка на каждой такой мутации
     * убирает «окно» неотскалированной широкой формулы. Хвостовой прогон
     * через 250ms подхватывает дописываемые последующие чанки. Живёт 60 с
     * (шрифты могут подгрузиться поздно), потом отключается.
     */
    var mjxObserverInstalled = false;
    function installMjxObserver() {
        if (mjxObserverInstalled ||
            typeof MutationObserver === 'undefined' || !document.body) {
            return;
        }
        mjxObserverInstalled = true;
        var installedAt = Date.now();
        var tailTimer = null;
        var observer = new MutationObserver(function () {
            if (Date.now() - installedAt > 60000) {
                observer.disconnect();
                return;
            }
            if (!document.querySelector('mjx-container[display="true"]')) {
                return;
            }
            if (window.fitWideMath) {
                window.fitWideMath();
            }
            if (tailTimer) {
                clearTimeout(tailTimer);
            }
            tailTimer = setTimeout(function () {
                tailTimer = null;
                if (window.fitWideMath) {
                    window.fitWideMath();
                }
            }, 250);
        });
        observer.observe(document.body, { childList: true, subtree: true });
    }

    /**
     * Запускает рендеринг MathJax
     */
    function renderMathJax() {
        // MathJax 3 - typesetAll
        if (window.MathJax && MathJax.typesetPromise) {
            MathJax.typesetPromise()
                .then(function () {
                    // task-12: после typeset — подгонка широких формул
                    if (window.fitWideMath) {
                        window.fitWideMath();
                    }
                })
                .catch(function (err) {
                    console.warn('MathJax typeset error:', err);
                });
        }

        // Fallback для старых версий
        if (window.MathJax && MathJax.Hub) {
            MathJax.Hub.Queue(['Typeset', MathJax.Hub]);
        }

        // task-12: страховка на случай, когда typeset делает сам MathJax
        // (авто-starter), а не наша typesetPromise-цепочка
        scheduleFitAfterTypeset();
    }

    /**
     * Декодирует HTML-сущности
     */
    function decodeHtmlEntities(text) {
        var entityMap = {
            '&amp;': '&',
            '&lt;': '<',
            '&gt;': '>',
            '&quot;': '"',
            '&#39;': "'",
            '&nbsp;': ' ',
            '&ndash;': '\u2013',
            '&mdash;': '\u2014',
            '&laquo;': '\u00AB',
            '&raquo;': '\u00BB',
            '&times;': '\u00D7',
            '&divide;': '\u00F7',
            '&infin;': '\u221E',
            '&int;': '\u222B',
            '&sum;': '\u2211',
            '&prod;': '\u220F',
            '&neq;': '\u2260',
            '&leq;': '\u2264',
            '&geq;': '\u2265',
            '&forall;': '\u2200',
            '&exist;': '\u2203',
            '&part;': '\u2202'
        };

        return text.replace(/&(amp|lt|gt|quot|#39|nbsp|ndash|mdash|laquo|raquo|times|divide|infin|int|sum|prod|neq|leq|geq|forall|exist|part);/gi, function (match, entity) {
            return entityMap['&' + entity + ';'] || match;
        });
    }

    // ========================================
    // Инициализация
    // ========================================

    document.addEventListener('DOMContentLoaded', function () {
        console.log('Math100 Generator - initialized');
        try {
            window.__tl = window.__tl || {};
            window.__tl.tDCL = Math.round(performance.now());
        } catch (e) {}

        // Если MathJax не загрузился (CDN недоступен) — убираем делимитаторы
        setTimeout(function () {
            if (!(window.MathJax && MathJax.startup && MathJax.startup.document)) {
                console.warn('MathJax not loaded (CDN unavailable?) - showing raw LaTeX text');
                stripMathDelimiters();
            }
        }, 7000);

        // task-12: подгонка широких формул при изменении размера окна
        // (debounce ~200ms; fitWideMath идемпотентна)
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

        // Установка наблюдателя за вставкой mjx-container (см. installMjxObserver)
        installMjxObserver();

        // Ждём MathJax
        document.addEventListener('mathjax-ready', function () {
            console.log('MathJax ready');
            if (window.__taskHtml) {
                processContent(document.getElementById('task-content'));
            }
            // task-12: fallback — если typeset выполнял сам MathJax,
            // TYPESET-watcher подгонит формулы после его готовности
            scheduleFitAfterTypeset();
        }, { once: true });

        // Если MathJax уже загружен
        if (window.MathJax && MathJax.startup) {
            document.dispatchEvent(new Event('mathjax-ready'));
        }

        // Если MathJax до этого уже отрисовал формулы (напр., авто-starter
        // успел к DOMContentLoaded) — сразу повторяем подгонку
        scheduleFitAfterTypeset();
    });

    // task-12: ранняя display-конвертация — СИНХРОННО, при выполнении
    // скрипта (до DOMContentLoaded и до авто-typeset'а MathJax)
    convertDisplayMathSpansEarly();

    // Экспорт для вызова из C++
    window.processContent = processContent;
    window.removeSpoilers = removeSpoilers;
    window.removeAds = removeAds;
    window.processImages = processImages;
    window.processMath = processMath;
    window.formatText = formatText;
    window.renderMathJax = renderMathJax;
    window.decodeHtmlEntities = decodeHtmlEntities;
    window.stripMathDelimiters = stripMathDelimiters;
    window.scheduleFitAfterTypeset = scheduleFitAfterTypeset;
    window.installMjxObserver = installMjxObserver;
    window.shouldBecomeDisplayMath = shouldBecomeDisplayMath;
    window.convertDisplayMathSpansEarly = convertDisplayMathSpansEarly;
})();
