# math100_console — headless-тестер функционала Math100 PDF Generator

Автономный консольный режим (CLI) для проверки **ядра функционала** проекта
*«Math100 PDF Generator»* **без GUI**: только `QCoreApplication` + Qt Network
(`QNetworkAccessManager`). Никаких Widgets/WebEngine — работает в headless-окружении
(без `DISPLAY`).

Задача: воспроизвести и прогнать в живьём ту же логику, что в GUI-версии
(`networkmanager.cpp` в корне проекта):
- извлечение URL задач со страницы варианта (те же regex'ы),
- скачивание страницы задачи и картинок,
- **извлечение ТОЛЬКО контента задачи** (`div.post-content`): без шапки, меню,
  сайдбара, рекламы (adsbygoogle/yandex_rtb) и скриптов метрик,
- **сохранение Ответа/Решения**: спойлеры `math100-spoiler` конвертируются в
  всегда-видимые блоки `.task-answer-block` (раньше негребедийный regex вырезал их
  на первом `</div>` — Ответ/Решение терялись),
- инлайнинг картинок как `data:`-URL (base64),
- кэширование картинок по имени `MD5(URL)`.

Команда `task` сохраняет **standalone HTML для браузера**: заголовок в `<title>`,
inline-стили (Times New Roman, блоки ответов), MathJax 3 с CDN (jsdelivr) —
при открытии файла в браузере формулы (`span.math`, `\( … \)`) рендерятся как на сайте
(нужен доступ к `cdn.jsdelivr.net`; сам файл задачи полностью автономен).

Логика regex'ов **скопирована** из эталонного `/projects/math100_generator/networkmanager.cpp`
(не компилируется, не включается — свой файл `console_main.cpp`; `extractTaskContent`
в корне уже есть своя реализация — здесь осознанная само-копия, конфликтов нет).

## Сборка

```bash
bash console/run.sh build
```
(эквивалент: `cmake -S console -B console/build -DCMAKE_BUILD_TYPE=Release && cmake --build console/build -j4`).
Бинарник: `console/build/math100_console`.

Требования (уже установлены в окружении): Qt6 (Core, Network), CMake ≥ 3.16, C++17.

> `bash console/run.sh <args...>` сам соберёт проект, если бинарника ещё нет.

## Команды

```
selftest                       Офлайн-тесты без сети (11 тестов)
fetch <variant-url>            Скачать вариант → извлечь ВСЕ URL задач (без капа) → сохранить HTML
                               (поддерживаются проф- ege_profil_* и базовые baza_* варианты)
task <task-url> [--no-images]  Скачать задачу → извлечь только контент → встроить
                               картинки data-URL → сохранить standalone HTML (MathJax)
cache clear                    Очистить console/out/
help                           Справка
```

### Примеры

```bash
bash console/run.sh selftest
bash console/run.sh fetch https://math100.ru/prof-ege-2027-10-1/
bash console/run.sh task  https://math100.ru/ege_profil_8_1-1/
bash console/run.sh task  https://math100.ru/ege_profil_8_1-1/ --no-images
bash console/run.sh cache clear
```

- `fetch` печатает пронумерованный список URL задач (task-10: **кап 30 убран** —
  ВСЕ задачи: проф-варианты 33, базовые 18) и кладёт HTML варианта
  в `console/out/variant_<md5(url)>.html`. Основной паттерн — якорь math100.ru
  с текстом «Задача N» (работает и для `ege_profil_*`, и для `baza_*`; чужие
  якоря без такого текста не попадают — баг «1 из 18» на базовых профилях
  исправлен); fallback'и — ege_profil по href → текст «Задача|ege» → шаблон.
- `task` (пайплайн: **fetch → extractTaskContent → fixImageUrlsOffline → standalone-обёртка**):
  1. скачивает страницу задачи (вся страница ~100–350 КБ);
  2. `extractTaskContent()` делает **balanced-извлечение** `div.post-content`
     (fallback: `<main id="main">`, затем вся страница) и заголовок из
     `span.entry-title`; из извлечённого удаляются ВСЕ `<script>`, `<iframe>`,
     `<noscript>` и `yandex_rtb`-блоки;
  3. спойлеры `math100-spoiler` (Ответ/Решение) **конвертируются** в видимые блоки
     `<div class="task-answer-block"><p class="task-answer-label"><strong>Ответ</strong></p>
     <div class="task-answer-body">…</div></div>` — содержимое спойлера сохраняется,
     `style="display:none"` не переносится;
  4. картинки задачи скачиваются в `console/out/images/<md5(url)>.png` (с повторным
     использованием кэша) и встраиваются как `data:<mime>;base64,...`;
  5. результат оборачивается в standalone HTML (`<!DOCTYPE html>`, `<title>`, стили,
     MathJax) и сохраняется в `console/out/task_<md5(url)>.html`.
  Сводка в консоли: размер итогового HTML (честно — по итоговому документу),
  число встроенных картинок, скачано/из кэша, **spoiler-блоков превращено = N**,
  **title = …**.
- **Как открыть результат в браузере:** просто открыть
  `console/out/task_<md5(url)>.html` (двойной клик / `xdg-open`). Формулы подтянутся
  и отрендерятся из MathJax CDN (нужен интернет для `cdn.jsdelivr.net`); картинки уже
  встроены в файл и не требуют сети.
- `--no-images` — не качать картинки (контент всё равно извлекается и обёртывается).
- Все команды пишут статусы по-русски (UTF-8), прогресс `[1/4] … [4/4]`, ошибки → `exit 1`.
  Лог: `console/out/run.log` (append, ISO-время).

## Что проверяет `selftest`

Офлайн, на локальных fixture (`console/testdata/`), без сети:

| Тест | Что проверяет |
|------|--------------|
| `extractTaskUrls` | основной паттерн «Задача N» по тексту якоря: 36 = 33 `ege_profil_10_1-N` + 3 базовых `baza_2025_17_1-N`; дедупликация; сортировка по N (неубывающая); чужой якорь `ege_profil_5_1-1` без текста НЕ найден; кап 30 УБРАН; дистракторы не попадают |
| `extractTaskUrls(fallback-F1)` | без текста «Задача N» срабатывает fallback по href `ege_profil` (дедуп, дистракторы не попадают) |
| `collectImageUrls(дедуп)` | уникальные URL картинок (4 тега `<img>` → 3 URL) |
| `fixImageUrls(офлайн)` | ЛЕГАСИ: замена `src`→`data:` по карте `url→bytes`; спойлер/реклама/cookie удаляются старыми паттернами (тест раунда 1, сохранён) |
| `extractTaskContent(область)` | результат = только post-content-область: начинается с `<div`, div'ы сбалансированы, МЕТКИ сайта (header/adsbygoogle/aside/cookie/после-post-content) отсутствуют |
| `extractTaskContent(спойлеры)` | `math100-spoiler` исчез, есть 2× `task-answer-block`, label-ы «Ответ»/«Решение», содержимое маркеров сохранено, `display:none` отсутствует, превращено = 2 |
| `extractTaskContent(чистка)` | нет `<script>`, `yandex_rtb`, `Ya.Context`, `<iframe>`, `informer.yandex` |
| `extractTaskContent(title)` | `titleOut` == ожидаемому `span.entry-title` |
| `pipeline(картинки)` | по реальному пайплайну: 3 уникальных data-URL, 4 тега `<img>` со `src="data:image/`, remote не осталось |
| `standalone(обёртка)` | `<!DOCTYPE html>`, `</html>`, MathJax CDN + конфиг (inlineMath/displayMath/processEscapes), `<title>`, стили |
| `cache(round-trip)` | `saveToCache`/`loadFromCache` во временный каталог; детерминизм имени `MD5`; `false` на промахе |

Итог: `PASS/FAIL` по каждому тесту + сводка; **ненулевой exit code при любом FAIL**.

## Расхождения с эталонным `networkmanager.cpp` (зафиксированы)

На живой странице math100.ru поведение чуть отличается от эталонного кода; в
`console_main.cpp` сделаны адаптации (в корне проекта ничего не менялось):

1. **Путь картинок.** Эталон ищет только `https?://math100.ru/images/...`, но на живой
   странице картинки лежат в `…/wp-content/uploads/…`. Здесь матчим
   `https?://math100.ru/<любой путь>/<png|jpg|jpeg|gif|webp>`. Без этой правки на живой
   странице не нашлось бы ни одной картинки.
2. **MIME в data-URL** выводится из расширения файла (эталон всегда `image/png`).
3. **`srcset`/`sizes`** вырезаются у `<img>` с уже встроенным `data:`-`src` (иначе браузер
   может отдать удалённый `srcset` вместо встроенного `src` — нарушается автономность HTML).
4. `fixImageUrls` разбит на оффлайн-часть `fixImageUrlsOffline(html, map url→bytes)` —
   её используют и `selftest` (без сети), и `task` после загрузки картинок.
5. Кэш: явный `baseDir` (`console/out`), без in-memory `QHash`+`QMutex` (CLI достаточно
   файлового кэша). Имена файлов как в эталоне: `MD5(URL).png`.
6. **task-4: только контент задачи.** Эталон/GUI сохраняли целую страницу (с рекламой),
   а негребедийный spoiler-regex `.*?</div>` ломал вложенные div'ы спойлеров
   (Ответ/Решение терялись). Здесь: balanced-скан `div` (`findDivSpan`), точное
   совпадение токена `math100-spoiler` в class-списке (не `-title`/`-content`),
   полная вычистка script/iframe/noscript/yandex_rtb и standalone-обёртка с MathJax.
   Легаси-паттерны в `fixImageUrlsOffline` оставлены как предохранитель.
7. **MathJax.** На сайте math100.ru формулы — `<span class="math">\(…latex…\)</span>`,
   т.е. текст уже несёт делимитаторы `\(…\)`. Inline-скрипт обёртки НЕ дублирует
   делимитаторы (иначе MathJax ломал бы формулу), а переносит текст как есть;
   если делимитаторов нет — добавляет `\(…\)` (inline) или `\[…\]` (display, если
   родитель P/DIV/BLOCKQUOTE). Typeset запускается вручную после конвертации
   (`startup.typeset: false`), чтобы избежать гонки и двойной обработки.

## Структура `console/`

```
console/
├── CMakeLists.txt        # автономный CMake (Qt6 Core+Network, C++17, AUTOMOC)
├── console_main.cpp      # весь CLI: selftest/fetch/task/cache + extractTaskContent
│                         # + convertSpoilers + wrapStandaloneHtml (MathJax)
├── run.sh                # обёртка: build / прокидка аргументов (set -e)
├── README.md
├── testdata/
│   ├── variant_sample.html   # 33 ссылки ege_profil_10_1-N («Задача N») + 3 дубликата
│                             # («Задание N») + 3 базовых baza_2025_17_1-N («Задача N»)
│                             # + чужой якорь ege_profil_5_1-1 БЕЗ текста + дистракторы
│   └── task_sample.html      # реалистичная структура: шапка/adsbygoogle/aside (мусор)
│                             # + span.entry-title + div.post-content (текст, span.math,
│                             # 4 img/3 URL, 2 вложенных спойлера Ответ/Решение,
│                             # yandex_rtb + script + iframe)
├── build/                # артефакты сборки (бинарник math100_console)
└── out/                  # результаты: variant_*.html, task_*.html, images/, run.log
```

> Пути `out/` и `testdata/` вычисляются относительно бинарника (`console/build/…`),
> т.е. корень `console/` — parent от `applicationDirPath()`. Поэтому сборку важно делать
> именно в `console/build` (так делает `run.sh`).

## Живой результат (проверено)

`fetch` (task-10, кап 30 убран, базовые профили поддерживаются):

| Страница варианта | Найдено задач |
|-------------------|---------------|
| `https://math100.ru/prof-ege-2027-10-1/` | **33** (`ege_profil_8_1-1..33`) |
| `https://math100.ru/baz17-1_2025/` | **18** (`baza_2025_17_1-1..18`; чужой якорь `ege_profil_5_1-1` НЕ найден) |

Задача:

В итоговых файлах отсутствуют: `yandex_rtb`, `adsbygoogle`, `informer.yandex`,
`math100-spoiler`, `display:none`, `Ya.Context`, шапка/сайдбар/меню; из скриптов
страницы не осталось ничего — добавлены только конфиг MathJax, CDN-скрипт
`jsdelivr` и inline-скрипт конвертации `span.math`.
