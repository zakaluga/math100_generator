#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

APPDIR="dist/math100_generator.AppDir"

echo "============================================"
echo "  Math100 PDF Generator — Bundle Builder"
echo "============================================"
echo

# 0. Проверяем/скачиваем linuxdeployqt (для patchelf)
DEPLOY="linuxdeployqt-x86_64.AppImage"
DEPLOY_URL="https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"

if [ ! -f "$DEPLOY" ]; then
    echo "[1/5] Скачивание linuxdeployqt..."
    if command -v wget &> /dev/null; then
        wget -q "$DEPLOY_URL" -O "$DEPLOY"
    elif command -v curl &> /dev/null; then
        curl -sL "$DEPLOY_URL" -o "$DEPLOY"
    else
        echo "[ERROR] wget/curl не найден!"
        echo "Установите: sudo apt install wget"
        exit 1
    fi
    chmod +x "$DEPLOY"
fi

# Извлекаем patchelf из linuxdeployqt (он есть внутри AppImage)
PATCH="squashfs-root/usr/bin/patchelf"
if [ ! -f "$PATCH" ]; then
    echo "[1.1] Извлечение patchelf..."
    ./linuxdeployqt-x86_64.AppImage --appimage-extract &>/dev/null
fi

# 1. Собираем проект
echo "[2/5] Сборка проекта..."
"$SCRIPT_DIR/build.sh" --build-only

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] Сборка проекта не удалась!"
    exit 1
fi

# 2. Подготавливаем AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR"/{bin,lib,plugins/{imageformats,platforms,generic,webengine},libexec,resources,qt6}

BINARY="build/math100_generator"

# Копируем бинарник
cp "$BINARY" "$APPDIR/bin/"
BUNDLED_BIN="$APPDIR/bin/math100_generator"

# Копируем frontend и ресурсы
cp -r frontend "$APPDIR/"
cp -r resources "$APPDIR/"
cp -r cache "$APPDIR/"

# Копируем плагины Qt
QT_PLUGINS=$(find /usr /opt -path "*/qt6/plugins" -type d -o -path "*/Qt6/plugins" -type d 2>/dev/null | head -1)

if [ -n "$QT_PLUGINS" ] && [ -d "$QT_PLUGINS" ]; then
    echo "[3/5] Копирование Qt плагинов..."

    cp "$QT_PLUGINS/imageformats/"*.so "$APPDIR/plugins/imageformats/" 2>/dev/null || true
    cp "$QT_PLUGINS/platforms/"*.so "$APPDIR/plugins/platforms/" 2>/dev/null || true
    cp "$QT_PLUGINS/generic/"*.so "$APPDIR/plugins/generic/" 2>/dev/null || true
fi

# QtWebEngine Process и ресурсы
if [ -d "/usr/lib/qt6/libexec" ]; then
    echo "[3.1] Копирование QtWebEngine Process..."
    cp /usr/lib/qt6/libexec/QtWebEngineProcess "$APPDIR/libexec/"
    cp /usr/lib/qt6/libexec/webenginedriver "$APPDIR/libexec/" 2>/dev/null || true
fi

if [ -d "/usr/share/qt6/resources" ]; then
    echo "[3.2] Копирование QtWebEngine ресурсов..."
    cp /usr/share/qt6/resources/* "$APPDIR/resources/"
fi

# 3. Копируем Qt библиотеки
echo "[4/5] Копирование Qt библиотек..."

# Получаем список Qt6 библиотек
QT6_LIBS=$(pkg-config --list-all 2>/dev/null | grep -i "^Qt6" | awk '{print $1}')

# Копируем все Qt6 библиотеки (preserve symlinks)
cp -P /usr/lib/x86_64-linux-gnu/libQt6*.so* "$APPDIR/lib/" 2>/dev/null || true

# Также копируем системные библиотеки, которые нужны QtWebEngine
cp /usr/lib/x86_64-linux-gnu/lib{drm,glesv2,gbm,wayland-client,wayland-cursor,wayland-egl,xdg-shell,dbus-1,nss3,nspr4,rtcore,atomic}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /usr/lib/x86_64-linux-gnu/lib{asound,pulse,pulse-simple,xcb,xkbfile,SM,ICE,X11,Xau,Xdmcp,Xext,Xrender,Xfixes,Xcursor,Xdamage,Xdamage,Xfixes,Xi,xtst,fontconfig,freetype,harfbuzz,svg,z,dl,zstd,bz2,png16}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /usr/lib/x86_64-linux-gnu/lib{EGL,GLESv2,vulkan}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /usr/lib/x86_64-linux-gnu/lib{pcre2,expat,icuuc,icudata,icui18n}.so* "$APPDIR/lib/" 2>/dev/null || true

# Копируем базовые системные библиотеки
cp /usr/lib/x86_64-linux-gnu/lib{pthread,m,c,dl,rt,util,mvec,munmap}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /lib/x86_64-linux-gnu/lib{pthread,m,c,dl,rt,util}.so* "$APPDIR/lib/" 2>/dev/null || true

# Настройка RPATH
echo "[4.1] Настройка RPATH..."
export PATH="$(dirname "$PATCH"):$PATH"

# Patchelf для бинарника
patchelf --set-rpath '$ORIGIN/../lib' "$BUNDLED_BIN" || true

# Patchelf для QtWebEngineProcess
if [ -f "$APPDIR/libexec/QtWebEngineProcess" ]; then
    patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/libexec/QtWebEngineProcess" || true
fi

# Patchelf для плагинов
for so in "$APPDIR/plugins"/*.so "$APPDIR/plugins"/*/*.so; do
    if [ -f "$so" ]; then
        patchelf --set-rpath '$ORIGIN/../../../lib' "$so" 2>/dev/null || true
    fi
done

# Копируем зависимости для бинарника (ldd)
echo "[4.2] Копирование зависимостей..."
for lib in $(ldd "$BUNDLED_BIN" 2>/dev/null | grep -oP '/[^ ]+' | sort -u); do
    if echo "$lib" | grep -qE "(libc|libpthread|libdl|libm|librt|libutil|libstdc\+\+|libgcc_s)"; then
        # Пропускаем стандартные библиотеки, если они уже скопированы
        basename=$(basename "$lib")
        if [ ! -f "$APPDIR/lib/$basename" ]; then
            cp "$lib" "$APPDIR/lib/" 2>/dev/null || true
        fi
    else
        # Копируем нестандартные библиотеки
        basename=$(basename "$lib")
        if [ ! -f "$APPDIR/lib/$basename" ]; then
            cp "$lib" "$APPDIR/lib/" 2>/dev/null || true
        fi
    fi
done

# Patchelf для Qt библиотек (чтобы они искали другие Qt библиотеки)
for so in "$APPDIR/lib/libQt6"*".so"*; do
    if [ -f "$so" ] && echo "$so" | grep -qP '\.so\.[0-9]'; then
        # Это symlink или real library
        patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
        # strip debug symbols для уменьшения размера
        strip --strip-debug "$so" 2>/dev/null || true
    fi
done

# Удалить дубликаты (symlinks и real files)
echo "[4.3] Очистка библиотек..."
for so in "$APPDIR/lib/libQt6"*.so; do
    if [ -L "$so" ] && [ -f "$so" ]; then
        TARGET=$(basename "$so")
        REAL=$(readlink -f "$so")
        if [ -f "$APPDIR/lib/$REAL" ]; then
            rm "$so"
        fi
    fi
done

# 4. Копируем .desktop файл и иконку
cp math100_generator.desktop "$APPDIR/"
mkdir -p "$APPDIR/usr/share/applications"
cp math100_generator.desktop "$APPDIR/usr/share/applications/"

# Иконка
if [ ! -f "$APPDIR/math100_generator.png" ]; then
    cp math100_generator.png "$APPDIR/" 2>/dev/null || true
fi

echo ""
echo "============================================"
echo "  Bundle готов!"
echo "============================================"
echo
echo "Папка: $APPDIR/"
echo
du -sh "$APPDIR/" 2>/dev/null || echo "(размер: см. выше)"
echo
echo "Запустить? (Y/N)"
read -r answer
if [ "$answer" = "Y" ] || [ "$answer" = "y" ]; then
    echo ""
    echo "Запуск из AppDir..."
    export LD_LIBRARY_PATH="$APPDIR/lib:$APPDIR/plugins/platforms:$LD_LIBRARY_PATH"
    export QT_PLUGIN_PATH="$APPDIR/plugins"
    export QT_QPA_PLATFORM=xcb
    export QTWEBENGINE_DISABLE_SANDBOX=1
    export QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu --offscreen-mode"
    ./math100_generator --platform xcb
fi
