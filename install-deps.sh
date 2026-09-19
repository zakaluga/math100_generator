#!/bin/bash
set -e

echo "============================================"
echo "  Math100 Generator - Установка зависимостей"
echo "============================================"
echo

# Определяем дистрибутив
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
else
    echo "[ERROR] Не удалось определить дистрибутив Linux"
    exit 1
fi

echo "Определён дистрибутив: $DISTRO"
echo

case "$DISTRO" in
    ubuntu|debian|linuxmint|pop)
        echo "[1/2] Обновление пакетов..."
        sudo apt update

        echo "[2/2] Установка зависимостей..."
        sudo apt install -y \
            cmake \
            build-essential \
            qt6-base-dev \
            qt6-webengine-dev \
            qt6-base-dev-tools \
            libgl1-mesa-dev \
            libxcomposite1 \
            libxrandr2 \
            libxss1 \
            libasound2 \
            libpulse0 \
            libnss3 \
            libdbus-1-3 \
            wget \
            file \
            desktop-file-utils \
            squashfs-tools

        echo ""
        echo "Готово! Можно собирать: bash build.sh"
        ;;

    fedora|rhel|centos|rocky|almalinux)
        echo "[1/1] Установка зависимостей..."
        sudo dnf install -y \
            cmake \
            gcc-c++ \
            make \
            qt6-qtbase-devel \
            qt6-qtwebengine-devel \
            mesa-libGL \
            libXcomposite \
            libXrandr \
            alsa-lib \
            pulseaudio-libs \
            nss \
            dbus-libs \
            wget \
            file \
            desktop-file-utils \
            squashfs-utils

        echo ""
        echo "Готово! Можно собирать: bash build.sh"
        ;;

    arch|manjaro|endeavouros)
        echo "[1/1] Установка зависимостей..."
        sudo pacman -S --noconfirm \
            cmake \
            base-devel \
            qt6-base \
            qt6-webengine \
            mesa \
            libxcomposite \
            libxrandr \
            alsa-lib \
            pulseaudio \
            nss \
            dbus \
            wget \
            file \
            desktop-file-utils \
            squashfs-tools

        echo ""
        echo "Готово! Можно собирать: bash build.sh"
        ;;

    opensuse-tumbleweed|opensuse-leap|opensuse)
        echo "[1/1] Установка зависимостей..."
        sudo zypper install -y \
            cmake \
            gcc-c++ \
            make \
            libQt6Core-devel \
            libQt6Widgets-devel \
            libQt6WebEngineCore-devel \
            libQt6WebEngineWidgets-devel

        echo ""
        echo "Готово! Можно собирать: bash build.sh"
        ;;

    *)
        echo "[WARNING] Дистрибутив $DISTRO не определён в скрипте."
        echo ""
        echo "Установите вручную:"
        echo "  - CMake 3.16+"
        echo "  - GCC/Clang с C++17"
        echo "  - Qt 6.x с модулями: Core, Widgets, WebEngine, Network"
        echo ""
        echo "Пример для Debian/Ubuntu:"
        echo "  sudo apt install cmake build-essential qt6-base-dev qt6-webengine-dev"
        ;;
esac

echo ""
echo "============================================"
echo "  Зависимости установлены!"
echo "============================================"
echo
echo "Дополнительно для bundle/AppImage:"
echo "  - linuxdeployqt скачается автоматически при первом запуске:"
echo "    bash build.sh bundle"
echo "    bash build.sh appimage"
