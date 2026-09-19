# Math100 PDF Generator

Генератор PDF вариантов ЕГЭ по математике с math100.ru:
поддерживаются как **профильные** (`ege_profil_*`, 33 задачи), так и
**базовые** (`baza_*`, 18 задач) варианты.

## Возможности

- Загрузка вариантов с math100.ru
- Красивый рендеринг формул через MathJax 3
- Скачивание изображений в кэш
- Генерация PDF через native print-to-PDF
- Кэширование страниц и изображений
- Удаление спойлеров и рекламы
- Кроссплатформенная сборка (Windows, Linux, macOS)

## Требования

- **C++17** компилятор (MSVC 2019+, GCC 9+, Clang 10+)
- **CMake** 3.16+
- **Qt 6.x** с модулями:
  - Qt Core
  - Qt Widgets
  - Qt WebEngine Widgets
  - Qt Network

## Установка (Windows)

1. Установите [CMake](https://cmake.org/download/)
2. Установите [Qt 6.x](https://www.qt.io/download-qt-installer-oss) (обязательно Qt WebEngine)
3. Установите [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) с workload "Desktop development with C++"
4. Запустите `build.bat`

## Установка (Linux)

```bash
# Ubuntu/Debian
sudo apt install cmake build-essential qt6-base-dev qt6-webengine-dev qt6-networkauth-dev

# Fedora
sudo dnf install cmake qt6-qtbase-devel qt6-qtwebengine-devel

# Arch
sudo pacman -S cmake qt6-base qt6-webengine
```

Сборка:
```bash
mkdir build && cd build
cmake -DCMAKE_PREFIX_PATH=/path/to/qt6 ..
cmake --build .
```

## Сборка

### Обычная сборка (для разработки)

```bash
bash build.sh                    # сборка + запуск
bash build.sh --build-only       # только сборка
```

### Распространение

#### Bundle (папка со всеми зависимостями)

```bash
bash build.sh bundle
# Результат: dist/math100_generator.AppDir/ (~400MB)
```

> Содержит: бинарник, Qt библиотеки, плагины, QtWebEngine Process, frontend, ресурсы.

Запуск из bundle:
```bash
LD_LIBRARY_PATH=./dist/math100_generator.AppDir/lib \
QT_PLUGIN_PATH=./dist/math100_generator.AppDir/plugins \
./dist/math100_generator.AppDir/bin/math100_generator
```

#### AppImage (один файл для любого Linux)

```bash
bash build.sh appimage
# Результат: dist/math100_generator-x86_64.AppImage (~150MB)
```

Запуск AppImage:
```bash
./dist/math100_generator-x86_64.AppImage
```

> linuxdeploy и appimagetool скачиваются автоматически (~30MB) при первом запуске.

## Использование

1. Запустите приложение
2. Вставьте URL варианта, например: `https://math100.ru/prof-ege-2027-10-1/`
   (профильный) или `https://math100.ru/baz17-1_2025/` (базовый)
3. Нажмите "Загрузить вариант"
4. В списке слева появятся ВСЕ задачи варианта (профиль: 33, база: 18)
5. Выберите задачу из списка
6. Для сохранения в PDF нажмите "Скачать PDF" (используется системный диалог печати)

## Экспорт PDF

После загрузки варианта доступны три кнопки экспорта (нижняя панель):

- **«Экспорт PDF…»** — диалог: отметьте нужные задачи («Выбрать все» / «Снять выбор»),
  контент — ☑ Задания (всегда), ☐ Ответы, ☐ Решения — и **формат**:
  - **«По страницам (полная форма)»** — одна задача на страницу, inline-формулы
    раскрываются в display (на отдельных строках, как в превью);
  - **«В таблицу (краткая форма, компактно)»** — все задачи в одной HTML-таблице
    (компактно, формулы остаются inline).
  Дефолт — **таблица**. Суффикс имени файла: `-export`.
- **«PDF: все задания (студент)»** — все задачи варианта, только сами задания
  (без ответов и решений), в формате **таблица** (компактный список заданий).
  Суффикс: `-student`.
- **«PDF: задания+ответы+решения (преподаватель)»** — все задачи с блоками «Ответ»
  и «Решение», в формате **страницы** (полные задачи с решениями читаются удобнее
  на отдельных страницах). Суффикс: `-teacher`.

Имя файла по умолчанию: название варианта + suffix + `.pdf`
(напр. `prof-ege-2027-10-1-student.pdf`), каталог загрузки; задаётся в диалоге сохранения.
Недостающие страницы задач скачиваются автоматически (прогресс — в прогресс-баре),
формулы отрисовываются MathJax; в формате «страницы» между задачами — разрывы страниц.
Работает с любым количеством задач (без капа 30): профильные варианты (33 задачи)
и базовые (18 задач, ссылки вида `baza_*`).

## Архитектура

```
┌─────────────────────────────────────┐
│         Qt Application (C++)         │
│  ┌───────────────────────────────┐   │
│  │   QtWebEngineView             │   │
│  │   ┌─────────────────────────┐ │   │
│  │   │   Embedded Chromium     │ │   │
│  │   │  HTML + JS + MathJax    │ │   │
│  │   │  window.print() for PDF │ │   │
│  │   └─────────────────────────┘ │   │
│  └───────────────┬───────────────┘   │
│                  │                   │
│  QNetworkAccessManager               │
│  (скачивает страницы без CORS)       │
└─────────────────────────────────────┘
```

## Структура проекта

```
math100_generator/
├── bundle.sh               # Сборка bundle (папка с зависимостями)
├── appimage.sh             # Сборка AppImage (один файл)
├── math100_generator.desktop  # .desktop файл для AppImage
├── CMakeLists.txt          # Настройка сборки
├── main.cpp                # Точка входа
├── mainwindow.h/.cpp       # Главное окно приложения
├── networkmanager.h/.cpp   # Сетевые запросы (QNetworkAccessManager)
├── webenginehost.h/.cpp    # QtWebEngineView + QWebChannel
├── urlinterceptor.h        # Интерцептор URL для Qt WebEngine
├── resources.qrc           # Qt ресурсы
├── frontend/               # HTML/JS фронтенд
│   ├── index.html
│   ├── css/style.css
│   ├── js/app.js
│   ├── js/renderer.js
│   └── js/pdf.js
├── resources/
│   └── styles.css
├── cache/                  # Кэш скачанных данных
├── build.bat               # Скрипт сборки для Windows
├── .gitignore
└── README.md
```

## Лицензия

MIT
