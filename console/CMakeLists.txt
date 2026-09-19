# Автономный CMake-проект headless-тестера Math100 (только Qt Core + Network, без GUI).
# НЕ подключается к корневому CMakeLists.txt.
#
# Сборка (рекомендуется через run.sh):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j4
# Бинарник: build/math100_console

cmake_minimum_required(VERSION 3.16)

project(math100_console LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Для корректной работы AUTOMOC/CMake с Qt
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_executable(math100_console
    console_main.cpp
)

target_link_libraries(math100_console PRIVATE
    Qt6::Core
    Qt6::Network
)
