@echo off
chcp 65001 >nul 2>&1
title Math100 Generator - Сборка

echo ============================================
echo   Math100 PDF Generator (Qt6, C++)
echo ============================================
echo.

REM Проверяем CMake
cmake --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ОШИБКА] CMake не найден!
    echo.
    echo Установите CMake с https://cmake.org/download/
    echo При установке поставьте галочку "Add CMake to PATH"
    echo.
    pause
    exit /b 1
)

REM Проверяем Qt6
set QT_DIR=%QT_DIR%
if not defined QT_DIR (
    echo [!] QT_DIR не установлен. Попытка автоопределения...
    for /d %%d in ("C:\Qt\6.*\msvc*") do set QT_DIR=%%d
    if not defined QT_DIR (
        for /d %%d in ("%USERPROFILE%\Qt\6.*\msvc*") do set QT_DIR=%%d
    )
)

if not defined QT_DIR (
    echo [ОШИБКА] Qt6 не найден!
    echo.
    echo Установите Qt6 с https://www.qt.io/download-qt-installer-oss
    echo Убедитесь что установлены: Qt 6.x, Qt WebEngine, C++ Compiler
    echo.
    echo Для автоматической настройки используйте:
    echo   Qt Creator -> Tools -> Options -> Kits
    echo.
    pause
    exit /b 1
)

echo [1/3] Найден Qt: %QT_DIR%
echo.

REM Создаём директорию сборки
if exist build (
    echo [!] Директория build уже существует. Очищаем...
    rmdir /s /q build
)
mkdir build
cd build

REM Генерация проекта CMake
echo [2/3] Настройка CMake...
cmake -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
    ..

if %errorlevel% neq 0 (
    echo.
    echo [ОШИБКА] CMake конфигурация не удалась!
    echo.
    echo Возможные причины:
    echo   - Qt6 установлен не в стандартное место
    echo   - Не установлен Qt WebEngine модуль
    echo   - Visual Studio 2022 не установлен
    echo.
    pause
    exit /b 1
)

echo.
echo [3/3] Компиляция (Release)...
cmake --build . --config Release -- /m

if %errorlevel% neq 0 (
    echo.
    echo [ОШИБКА] Компиляция не удалась!
    echo Проверьте ошибки выше.
    echo.
    pause
    exit /b 1
)

echo.
echo ============================================
echo   Сборка завершена успешно!
echo ============================================
echo.
echo Файл: build\Release\math100_generator.exe
echo.
echo Хотите запустить? (Y/N)
choice /c YN /n /m "> "
if errorlevel 2 goto :skip

echo.
echo Запуск...
start "" "Release\math100_generator.exe"

:skip
echo.
pause
