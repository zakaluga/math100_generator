cmake_minimum_required(VERSION 3.16)
project(math100_generator VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Совместимость с Qt 6.2+
if(POLICY CMP0150)
    cmake_policy(SET CMP0150 OLD)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network WebEngineWidgets WebChannel)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# task-16b: шрифты MathJax встроены в JS как data: URIs (анти-CORS: Chromium
# на Windows блокирует кросс-оригинную загрузку шрифтов со схемы qrc:,
# data: — в whitelist). Скрипт генерирует встраиваемый JS + .qrc; пути в
# .qrc относительные (от каталога .qrc) — переносимо для CI.
# Старые mathjax-записи удалены из resources.qrc (rcc: дубликат
# prefix+alias из двух .qrc недопустим).
set(MJX_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/mathjax-embedded")
set(MJX_EMBED_QRC "${CMAKE_CURRENT_BINARY_DIR}/mathjax-embedded.qrc")
file(GLOB MJX_FONT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/mathjax/fonts/*.woff")
add_custom_command(
    OUTPUT "${MJX_EMBED_QRC}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_mathjax_fonts.py"
            --src "${CMAKE_CURRENT_SOURCE_DIR}/resources/mathjax"
            --out "${MJX_EMBED_DIR}"
            --qrc "${MJX_EMBED_QRC}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_mathjax_fonts.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/mathjax/es5/tex-mml-chtml.js"
            ${MJX_FONT_FILES}
    COMMENT "MathJax: встраивание woff-шрифтов data: URI (анти-CORS qrc)"
    VERBATIM)
set_source_files_properties("${MJX_EMBED_QRC}" PROPERTIES GENERATED TRUE)

# task-20: иконка приложения (Windows): icon.rc ссылается на icon.ico
# (генерируется из icon-148-148.png скриптом /tmp/opencode/png2ico.py —
# в репо закоммичен готовый icon.ico, 6 размеров 16..128).
# На Linux .rc не участвует в сборке.
if(WIN32)
    set(APP_ICON_SOURCE icon.rc)
endif()

qt_add_executable(math100_generator
    main.cpp
    mainwindow.h mainwindow.cpp
    networkmanager.h networkmanager.cpp
    webenginehost.h webenginehost.cpp
    exportdialog.h exportdialog.cpp
    mathsplit.h mathsplit.cpp
    resources.qrc
    "${MJX_EMBED_QRC}"
    ${APP_ICON_SOURCE}
)

target_link_libraries(math100_generator PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Core
    Qt6::Network
)

set_target_properties(math100_generator PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

# Копируем frontend и ресурсы в директорию сборки
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/frontend DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/resources DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/cache DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS math100_generator
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# task-13: unit-тест splitWideMathChains (QtCore только, без WebEngine).
# Запуск из корня проекта: ./build/mathsplit_test
add_executable(mathsplit_test
    tests/mathsplit_test.cpp
    mathsplit.h mathsplit.cpp
)
target_include_directories(mathsplit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mathsplit_test PRIVATE Qt6::Core)
