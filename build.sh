#!/bin/bash
set -e

echo "============================================"
echo "  Math100 PDF Generator (Qt6, C++)"
echo "============================================"
echo

# Режимы: обычная сборка / только build / bundle / appimage
MODE="${1:-run}"
INTERACTIVE=true

# --- Режим bundle ---
if [ "$MODE" = "bundle" ]; then
    echo "[INFO] Запуск bundle-сборки..."
    bash "$(dirname "$0")/bundle.sh"
    exit 0
fi

# --- Режим appimage ---
if [ "$MODE" = "appimage" ]; then
    echo "[INFO] Запуск AppImage-сборки..."
    bash "$(dirname "$0")/appimage.sh"
    exit 0
fi

# --- Режим только build ---
if [ "$MODE" = "--build-only" ]; then
    echo "[INFO] Только сборка..."
    INTERACTIVE=false
fi

# Проверяем CMake
if ! command -v cmake &> /dev/null; then
    echo "[ERROR] CMake не найден!"
    echo ""
    echo "Установите:"
    echo "  Ubuntu/Debian: sudo apt install cmake"
    echo "  Fedora:        sudo dnf install cmake"
    echo "  Arch:          sudo pacman -S cmake"
    exit 1
fi

# Проверяем Qt6
if ! pkg-config --exists qt6Core 2>/dev/null && ! dpkg -l libqt6core6 &>/dev/null; then
    echo "[!] Qt6 не найден через pkg-config, пытаемся найти через qmake6..."
    if command -v qmake6 &> /dev/null; then
        QT_PREFIX=$(qmake6 -query QT_INSTALL_PREFIX 2>/dev/null || echo "")
    else
        QT_PREFIX=""
    fi
fi

echo "[1/3] Создание директории сборки..."
rm -rf build
mkdir -p build
cd build

echo "[2/3] Настройка CMake..."
cmake -DCMAKE_BUILD_TYPE=Release ..

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] CMake конфигурация не удалась!"
    echo ""
    echo "Возможные причины:"
    echo "  - Qt6 не установлен (см. install-deps.sh)"
    echo "  - Qt WebEngine модуль не установлен"
    echo "  - CMake версии ниже 3.16"
    echo ""
    echo "Установите зависимости:"
    echo "  bash ../install-deps.sh"
    exit 1
fi

echo "[3/3] Компиляция..."
cmake --build . -j$(nproc 2>/dev/null || echo 2)

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] Компиляция не удалась!"
    echo "Проверьте ошибки выше."
    exit 1
fi

echo ""
echo "============================================"
echo "  Сборка завершена успешно!"
echo "============================================"
echo
echo "Бинарник: build/math100_generator"
echo
echo "Дополнительные цели:"
echo "  bash build.sh bundle       — собрать bundle (папка с зависимостями)"
echo "  bash build.sh appimage     — собрать AppImage (один файл)"
echo
if [ "$INTERACTIVE" = true ]; then
    echo "Запустить? (Y/N)"
    read -r answer
    if [ "$answer" = "Y" ] || [ "$answer" = "y" ]; then
        echo ""
        echo "Запуск..."
        ./math100_generator
    fi
fi
