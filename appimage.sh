#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

APPDIR="dist/math100_generator.AppDir"
APPIMAGE="dist/math100_generator-x86_64.AppImage"

echo "============================================"
echo "  Math100 PDF Generator — AppImage Builder"
echo "============================================"
echo

# 0. Проверяем зависимости
echo "[0/5] Проверка зависимостей..."
for cmd in wget curl patchelf mksquashfs appimagetool; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "[!] $cmd не найден, извлекаю из linuxdeploy..."
        break
    fi
done

DEPLOY="linuxdeploy-x86_64.AppImage"
DEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"

if [ ! -f "$DEPLOY" ]; then
    echo "[1/5] Скачивание linuxdeploy (~30MB)..."
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

# Извлекаем инструменты
LINDEPLOY_BIN="squashfs-root/usr/bin/linuxdeploy"
APPIMAGE_BIN="squashfs-root/usr/bin/appimagetool"
PATCH="squashfs-root/usr/bin/patchelf"

if [ ! -f "$LINDEPLOY_BIN" ]; then
    echo "[1.1] Извлечение linuxdeploy..."
    ./linuxdeploy-x86_64.AppImage --appimage-extract &>/dev/null
fi

export PATH="$(dirname "$PATCH"):$PATH"

# 1. Собираем проект
echo "[2/5] Сборка проекта..."
"$SCRIPT_DIR/build.sh" --build-only

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] Сборка проекта не удалась!"
    exit 1
fi

# 2. Подготавливаем AppDir
echo "[3/5] Подготовка AppDir..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR"/{bin,lib,plugins/{imageformats,platforms,generic},libexec,resources,usr/share/applications}

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
    cp "$QT_PLUGINS/imageformats/"*.so "$APPDIR/plugins/imageformats/" 2>/dev/null || true
    cp "$QT_PLUGINS/platforms/"*.so "$APPDIR/plugins/platforms/" 2>/dev/null || true
    cp "$QT_PLUGINS/generic/"*.so "$APPDIR/plugins/generic/" 2>/dev/null || true
fi

# QtWebEngine Process и ресурсы
if [ -d "/usr/lib/qt6/libexec" ]; then
    cp /usr/lib/qt6/libexec/QtWebEngineProcess "$APPDIR/libexec/"
    cp /usr/lib/qt6/libexec/webenginedriver "$APPDIR/libexec/" 2>/dev/null || true
fi

if [ -d "/usr/share/qt6/resources" ]; then
    cp /usr/share/qt6/resources/* "$APPDIR/resources/"
fi

# 3. Копируем библиотеки
echo "[4/5] Копирование библиотек..."

# Qt6 библиотеки (preserve symlinks)
cp -P /usr/lib/x86_64-linux-gnu/libQt6*.so* "$APPDIR/lib/" 2>/dev/null || true

# Системные библиотеки
cp /usr/lib/x86_64-linux-gnu/lib{drm,glesv2,gbm,wayland-client,wayland-cursor,wayland-egl,xdg-shell,dbus-1,nss3,nspr4,rtcore,atomic}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /usr/lib/x86_64-linux-gnu/lib{asound,pulse,pulse-simple,xcb,xkbfile,SM,ICE,X11,Xau,Xdmcp,Xext,Xrender,Xfixes,Xcursor,Xdamage,Xi,xtst,fontconfig,freetype,harfbuzz,svg,z,dl,zstd,bz2,png16}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /usr/lib/x86_64-linux-gnu/lib{EGL,GLESv2,vulkan}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /usr/lib/x86_64-linux-gnu/lib{pcre2,expat,icuuc,icudata,icui18n}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /usr/lib/x86_64-linux-gnu/lib{pthread,m,c,dl,rt,util,mvec,munmap}.so* "$APPDIR/lib/" 2>/dev/null || true
cp /lib/x86_64-linux-gnu/lib{pthread,m,c,dl,rt,util}.so* "$APPDIR/lib/" 2>/dev/null || true

# Настройка RPATH
patchelf --set-rpath '$ORIGIN/../lib' "$BUNDLED_BIN" || true
if [ -f "$APPDIR/libexec/QtWebEngineProcess" ]; then
    patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/libexec/QtWebEngineProcess" || true
fi
for so in "$APPDIR/plugins"/*.so "$APPDIR/plugins"/*/*.so; do
    [ -f "$so" ] && patchelf --set-rpath '$ORIGIN/../../../lib' "$so" 2>/dev/null || true
done

# Зависимости через ldd
for lib in $(ldd "$BUNDLED_BIN" 2>/dev/null | grep -oP '/[^ ]+' | sort -u); do
    [ ! -f "$APPDIR/lib/$(basename "$lib")" ] && cp "$lib" "$APPDIR/lib/" 2>/dev/null || true
done

# Strip Qt библиотек для уменьшения размера
for so in "$APPDIR/lib/libQt6"*".so"*; do
    [ -f "$so" ] && echo "$so" | grep -qP '\.so\.[0-9]$' && strip --strip-debug "$so" 2>/dev/null || true
done

# 4. Создаём AppImage
echo "[5/5] Создание AppImage..."

# .desktop и иконка
cp math100_generator.desktop "$APPDIR/math100_generator.desktop"
cp math100_generator.desktop "$APPDIR/usr/share/applications/"

# Иконка (если нет, создаём простую)
if [ ! -f "$APPDIR/math100_generator.png" ]; then
    python3 -c "
import struct, zlib
w, h = 32, 32
data = b''
for y in range(h):
    for x in range(w):
        dx, dy = x-16, y-16
        if dx*dx + dy*dy <= 14*14:
            data += struct.pack('BBBB', 70, 130, 180, 255)
        else:
            data += b'\\x00\\x00\\x00\\x00'
def chunk(t, d):
    c = t + d; crc = zlib.crc32(c) & 0xffffffff
    return struct.pack('>I', len(d)) + c + struct.pack('>I', crc)
sig = b'\\x89PNG\\r\\n\\x1a\\n'
ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
raw = b''
for y in range(h): raw += b'\\x00' + data[y*w*4:(y+1)*w*4]
idat = chunk(b'IDAT', zlib.compress(raw))
with open('$APPDIR/math100_generator.png', 'wb') as f: f.write(sig + ihdr + idat + chunk(b'IEND', b''))
" 2>/dev/null || echo "[!] Не создана иконка, но это не критично"
fi

# AppRun
cp "$BUNDLED_BIN" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"

# Создаём AppImage через appimagetool
if command -v appimagetool &>/dev/null; then
    appimagetool "$APPDIR" "$APPIMAGE" 2>&1
elif [ -f "$APPIMAGE_BIN" ]; then
    chmod +x "$APPIMAGE_BIN"
    "$APPIMAGE_BIN" "$APPDIR" "$APPIMAGE" 2>&1
else
    echo "[ERROR] appimagetool не найден!"
    exit 1
fi

# Проверяем результат
if [ -f "$APPIMAGE" ]; then
    chmod +x "$APPIMAGE"
    echo ""
    echo "============================================"
    echo "  AppImage создан!"
    echo "============================================"
    echo
    ls -lh "$APPIMAGE"
    echo
    echo "Запустить? (Y/N)"
    read -r answer
    if [ "$answer" = "Y" ] || [ "$answer" = "y" ]; then
        echo ""
        echo "Запуск AppImage..."
        "$APPIMAGE" --appimage-extract-and-run
    fi
else
    echo ""
    echo "[ERROR] AppImage не создан!"
    exit 1
fi
