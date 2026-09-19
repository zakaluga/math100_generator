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

qt_add_executable(math100_generator
    main.cpp
    mainwindow.h mainwindow.cpp
    networkmanager.h networkmanager.cpp
    webenginehost.h webenginehost.cpp
    exportdialog.h exportdialog.cpp
    mathsplit.h mathsplit.cpp
    resources.qrc
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
