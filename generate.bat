@echo off
chcp 65001 >nul 2>&1
title Генератор PDF вариантов ЕГЭ

echo ============================================
echo   Генератор PDF вариантов ЕГЭ (math100.ru)
echo ============================================
echo.

REM Проверяем наличие Python
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ОШИБКА] Python не найден!
    echo.
    echo Установите Python с https://www.python.org/downloads/
    echo При установке ОБЯЗАТЕЛЬНО поставьте галочку "Add Python to PATH"
    echo.
    pause
    exit /b 1
)

echo [1/3] Установка зависимостей...
pip install -r requirements.txt --quiet --disable-pip-version-check
if %errorlevel% neq 0 (
    echo [ОШИБКА] Не удалось установить зависимости!
    echo Попробуйте запустить от имени администратора.
    pause
    exit /b 1
)

echo [2/3] Запуск генерации PDF...
echo.
python generate_pdf.py
echo.

echo [3/3] Готово!
echo.
if exist "variant.pdf" (
    echo Файл variant.pdf успешно создан!
    echo Хотите открыть его? (Y/N)
    choice /c YN /n /m "> "
    if errorlevel 1 (
        start variant.pdf
    )
) else (
    echo Файл PDF не был создан. Проверьте сообщения об ошибках выше.
)

echo.
pause
