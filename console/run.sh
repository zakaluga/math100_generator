#!/usr/bin/env bash
# run.sh — обёртка над math100_console.
#
# Использование:
#   bash console/run.sh build                 — собрать бинарник (Release, -j4)
#   bash console/run.sh <args...>             — выполнить команду; если бинарника
#                                               нет, сначала соберёт, затем прокинет args
#
# Примеры:
#   bash console/run.sh selftest
#   bash console/run.sh fetch https://math100.ru/prof-ege-2027-10-1/
#   bash console/run.sh task https://math100.ru/ege_profil_8_1-1/
set -e

# Каталог, в котором лежит этот run.sh (= корень console/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BIN="${BUILD_DIR}/math100_console"

build() {
    echo "[run.sh] Сборка: cmake -S ${SCRIPT_DIR} -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Release"
    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
    echo "[run.sh] Сборка: cmake --build ${BUILD_DIR} -j4"
    cmake --build "${BUILD_DIR}" -j4
    echo "[run.sh] Готово: ${BIN}"
}

if [ "${1:-}" = "build" ]; then
    build
    exit 0
fi

# Если бинарника нет — сначала соберём
if [ ! -x "${BIN}" ]; then
    echo "[run.sh] Бинарник не найден, собираю..."
    build
fi

# Кириллица/UTF-8 в консоли
export LC_ALL="${LC_ALL:-C.UTF-8}"
export LANG="${LANG:-C.UTF-8}"

exec "${BIN}" "$@"
