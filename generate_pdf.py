#!/usr/bin/env python3
"""
Генератор PDF для вариантов ЕГЭ с math100.ru
Скачивает задания с указанного варианта и собирает в один PDF-файл.
"""

import os
import re
import sys
import time
import hashlib
import tempfile
import requests
from io import BytesIO
from pathlib import Path
from urllib.parse import urljoin

from bs4 import BeautifulSoup, NavigableString
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Image,
    PageBreak, Table, TableStyle, KeepTogether
)
from reportlab.lib import colors
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from PIL import Image as PILImage


# ── Конфигурация ──────────────────────────────────────────────

VARIANT_URL = "https://math100.ru/prof-ege-2027-10-1/"
OUTPUT_FILE = "variant.pdf"
IMAGES_DIR = "images_cache"
LATEX_DIR = "latex_cache"
REQUEST_DELAY = 1.0  # задержка между запросами (сек)
PAGE_WIDTH, PAGE_HEIGHT = A4  # 210 x 297 мм

# ── Регистрация шрифтов ───────────────────────────────────────

def register_fonts():
    """Регистрирует шрифты с поддержкой кириллицы."""
    font_dirs = [
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts"),
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        os.path.expanduser("~/.fonts"),
        os.path.join(os.path.dirname(__file__), "fonts"),
    ]

    # Попробуем найти DejaVu Sans
    font_name = "DejaVuSans"
    font_found = False

    for base_dir in font_dirs:
        for root, dirs, files in os.walk(base_dir):
            for f in files:
                if f.lower() == "dejavusans.ttf":
                    full = os.path.join(root, f)
                    try:
                        pdfmetrics.registerFont(TTFont(font_name, full))
                        # Bold
                        bold_path = os.path.join(root, "DejaVuSans-Bold.ttf")
                        if os.path.exists(bold_path):
                            pdfmetrics.registerFont(TTFont(font_name + "-Bold", bold_path))
                        else:
                            pdfmetrics.registerFont(TTFont(font_name + "-Bold", full))
                        font_found = True
                        break
                    except Exception:
                        continue
            if font_found:
                break
        if font_found:
            break

    if not font_found:
        # Fallback: попробуем загрузить из репозитория
        try:
            import subprocess
            subprocess.run(
                ["apt-get", "install", "-y", "fonts-dejavu-core"],
                capture_output=True, timeout=60
            )
            for base_dir in font_dirs:
                for root, dirs, files in os.walk(base_dir):
                    for f in files:
                        if f.lower() == "dejavusans.ttf":
                            full = os.path.join(root, f)
                            pdfmetrics.registerFont(TTFont(font_name, full))
                            bold_path = os.path.join(root, "DejaVuSans-Bold.ttf")
                            if os.path.exists(bold_path):
                                pdfmetrics.registerFont(TTFont(font_name + "-Bold", bold_path))
                            else:
                                pdfmetrics.registerFont(TTFont(font_name + "-Bold", full))
                            font_found = True
                            break
                    if font_found:
                        break
                if font_found:
                    break
        except Exception:
            pass

    if not font_found:
        print("WARNING: DejaVu Sans not found, using Helvetica (no Cyrillic)")
        return "Helvetica", "Helvetica-Bold"

    return font_name, font_name + "-Bold"


# ── HTTP ──────────────────────────────────────────────────────

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                  "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
})


def fetch_page(url):
    """Загружает HTML-страницу."""
    resp = SESSION.get(url, timeout=30)
    resp.raise_for_status()
    resp.encoding = resp.apparent_encoding or "utf-8"
    return resp.text


def download_image(url, save_path):
    """Скачивает изображение, возвращает путь к файлу или None."""
    if os.path.exists(save_path):
        return save_path
    try:
        resp = SESSION.get(url, timeout=30)
        resp.raise_for_status()
        with open(save_path, "wb") as f:
            f.write(resp.content)
        return save_path
    except Exception as e:
        print(f"  [!] Ошибка скачивания {url}: {e}")
        return None


# ── Парсинг страницы варианта ─────────────────────────────────

def get_task_urls(variant_url):
    """Извлекает URL-ы всех задач со страницы варианта."""
    html = fetch_page(variant_url)
    soup = BeautifulSoup(html, "html.parser")

    urls = []
    for a in soup.select("div.post-content a[href]"):
        href = a.get("href", "")
        text = a.get_text(strip=True)
        if "Задача" in text or "ege_profil_8_1-" in href:
            full_url = urljoin(variant_url, href)
            urls.append(full_url)

    if not urls:
        # Fallback: генерируем URL-ы по паттерну
        base = variant_url.rstrip("/")
        # Попробуем определить номер варианта из URL
        match = re.search(r"prof-ege-\d+-\d+-(\d+)/", variant_url)
        if not match:
            match = re.search(r"prof-ege-\d+-(\d+)/", variant_url)
        print("[!] Не удалось извлечь ссылки, генерируем по паттерну...")
        for i in range(1, 34):
            urls.append(f"https://math100.ru/ege_profil_8_1-{i}/")

    return urls


# ── Парсинг задачи ────────────────────────────────────────────

def parse_task(html, task_url):
    """
    Парсит HTML задачи, извлекает:
    - task_text: HTML с текстом задания (без спойлеров)
    - images: список URL картинок задания
    - latex_blocks: список LaTeX-формул
    - answer_url: URL страницы (ссылка на решение)
    """
    soup = BeautifulSoup(html, "html.parser")
    content = soup.select_one("div.post-content")
    if not content:
        content = soup.select_one("#content")
    if not content:
        return None

    # Убираем спойлеры (ответ/решение) — они нам не нужны для задания,
    # но ссылку на страницу сохраняем
    spoilers = content.select("div.math100-spoiler")
    for s in spoilers:
        s.decompose()

    # Убираем рекламу и ненужные элементы
    for sel in [".adsbygoogle", "script", "style", ".math100-promo-bar",
                ".m100-cookie-banner", "iframe"]:
        for el in content.select(sel):
            el.decompose()

    # Извлекаем картинки
    images = []
    for img in content.select("img"):
        src = img.get("src", "")
        if src and "math100.ru" in src:
            full_url = urljoin(task_url, src)
            images.append(full_url)

    # Извлекаем LaTeX-формулы
    latex_blocks = []
    for span in content.select("span.math"):
        latex = span.get_text()
        latex_blocks.append(latex)

    # Получаем чистый HTML текста задания
    task_html = str(content)

    return {
        "html": task_html,
        "images": images,
        "latex_blocks": latex_blocks,
        "solution_url": task_url,
    }


# ── Конвертация LaTeX в изображение ───────────────────────────

def _crop_and_normalize(path, font_size, dpi=150):
    """Обрезает белые края формулы и нормализует высоту к font_size * 0.9."""
    img = PILImage.open(path).convert('RGB')
    inverted = img.point(lambda p: 255 - p)
    bbox = inverted.getbbox()
    if bbox:
        w0, h0, w1, h1 = bbox
        # getbbox может выходить за границы на 1px
        w0 = max(0, w0)
        h0 = max(0, h0)
        w1 = min(img.size[0] - 1, w1)
        h1 = min(img.size[1] - 1, h1)
        content_w = w1 - w0 + 1
        content_h = h1 - h0 + 1
        if content_w > 0 and content_h > 0:
            img = img.crop((w0, h0, w1 + 1, h1 + 1))
    # Нормализация высоты к font_size * 0.9 мм
    target_h_px = int(font_size * 0.9 / 25.4 * dpi)
    target_h_px = max(10, min(target_h_px, img.size[1] * 3))
    if img.size[1] != target_h_px:
        aspect = img.size[0] / img.size[1]
        new_w = max(1, int(target_h_px * aspect))
        img = img.resize((new_w, target_h_px), PILImage.LANCZOS)
    img.save(path)


def latex_to_image(latex_str, save_path, font_size=11, target_height_mm=None):
    """Конвертирует LaTeX-строку в PNG-изображение.

    :param font_size: размер шрифта LaTeX, совпадает с размером окружающего текста
    :param target_height_mm: целевая высота картинки в мм (по умолчанию = font_size * 1.3)
    """
    if os.path.exists(save_path):
        return save_path

    # Очистка LaTeX
    clean = latex_str.strip()
    if clean.startswith("\\(") and clean.endswith("\\)"):
        clean = clean[2:-2]
    if clean.startswith("$") and clean.endswith("$"):
        clean = clean[1:-1]
    clean = clean.replace("&#8212;", "-").replace("&mdash;", "-")
    clean = clean.replace("&#8211;", "-").replace("&ndash;", "-")
    clean = clean.replace("&#160;", " ").replace("&nbsp;", " ")

    if not any(c in clean for c in ["\\", "^", "_", "{", "}"]):
        return None

    if target_height_mm is None:
        target_height_mm = font_size * 0.9  # ~9.9 при font_size=11

    target_height_in = target_height_mm / 25.4

    try:
        fig = plt.figure(figsize=(8, target_height_in))
        fig.patch.set_facecolor('white')
        text = fig.text(0.02, 0.5, f"${clean}$",
                        fontsize=font_size, va='center', ha='left',
                        math_fontfamily='cm')
        fig.savefig(save_path, dpi=150, bbox_inches='tight',
                    pad_inches=0, facecolor='white')
        plt.close(fig)
        _crop_and_normalize(save_path, font_size, dpi=150)
        return save_path
    except Exception as e:
        print(f"  [!] Ошибка рендера LaTeX: {e}")
        try:
            fig = plt.figure(figsize=(8, target_height_in))
            fig.patch.set_facecolor('white')
            fig.text(0.02, 0.5, clean, fontsize=font_size, va='center', ha='left')
            fig.savefig(save_path, dpi=150, bbox_inches='tight',
                        pad_inches=0, facecolor='white')
            plt.close(fig)
            _crop_and_normalize(save_path, font_size, dpi=150)
            return save_path
        except Exception:
            return None


# ── Генерация PDF ─────────────────────────────────────────────

def html_to_flowables(html_str, task_num, solution_url, images,
                      font_name, font_bold, text_size=11):
    """
    Конвертирует HTML задания в list[Flowable] для reportlab.
    """
    soup = BeautifulSoup(html_str, "html.parser")
    content = soup.select_one("div.post-content") or soup

    flowables = []

    # Заголовок задачи
    style_title = ParagraphStyle(
        "TaskTitle",
        parent=getSampleStyleSheet()["Heading1"],
        fontName=font_bold,
        fontSize=16,
        leading=20,
        spaceAfter=12,
        alignment=TA_LEFT,
    )
    flowables.append(Paragraph(f"Задача {task_num}", style_title))

    # Основной стиль текста
    style_text = ParagraphStyle(
        "TaskText",
        parent=getSampleStyleSheet()["Normal"],
        fontName=font_name,
        fontSize=text_size,
        leading=text_size * 1.36,
        spaceAfter=6,
    )

    # Ссылка на решение
    style_link_title = ParagraphStyle(
        "LinkTitle",
        parent=getSampleStyleSheet()["Normal"],
        fontName=font_bold,
        fontSize=10,
        leading=13,
        spaceAfter=3,
    )
    style_link = ParagraphStyle(
        "LinkStyle",
        parent=getSampleStyleSheet()["Normal"],
        fontName=font_name,
        fontSize=9,
        leading=12,
        textColor=colors.HexColor("#1a73e8"),
    )

    def render_math_image(latex_str):
        """Рендерит формулу в PNG с правильным размером под text_size."""
        target_height_mm = text_size * 0.9
        latex_key = hashlib.md5(
            f"{latex_str}_{text_size}".encode()
        ).hexdigest()
        img_path = os.path.join(LATEX_DIR, f"inline_{latex_key}.png")
        path = latex_to_image(latex_str, img_path,
                              font_size=text_size,
                              target_height_mm=target_height_mm)
        if path:
            try:
                img = PILImage.open(path)
                w, h = img.size
                max_w = PAGE_WIDTH - 40 * mm
                scale = min(max_w / w, 1.0)
                h_pt = int(h * scale * 72 / 150)  # px → pt (DPI=150)
                return path, w * scale, h_pt
            except Exception:
                return None, None, None
        return None, None, None

    def process_inline_math(el):
        """
        Обрабатывает блочные/inline-элементы как единый блок.
        Формулы <span class="math"> встраиваются через <img> в HTML
        reportlab-Paragraph, чтобы оставаться в потоке текста.
        """
        if el.name is None:
            # NavigableString
            text = str(el).strip()
            if text:
                return Paragraph(escape_xml(text), style_text)
            return None

        if el.name in ("script", "style", "iframe"):
            return None

        if el.name in ("p", "div", "span"):
            # Проверяем, есть ли внутри math или img
            has_math = el.select("span.math")
            has_img = el.select("img")

            if not has_math and not has_img:
                # Обычный текст — без формул/картинок
                text = el.get_text(strip=True)
                if text:
                    return Paragraph(escape_xml(text), style_text)
                return None

            # Сборка HTML-фрагмента для Paragraph
            html_parts = []
            for child in el.children:
                if child.name is None:
                    text = str(child).strip()
                    if text:
                        html_parts.append(escape_xml(text))
                    continue

                if child.name == "span" and "math" in child.get("class", []):
                    latex = child.get_text()
                    if latex.strip():
                        path, w, h_pt = render_math_image(latex)
                        if path:
                            html_parts.append(
                                f'<img src="{path}" width="{w}" height="{h_pt}"/>'
                            )
                        else:
                            fallback = escape_xml(
                                latex.replace("\\(", "")
                                     .replace("\\)", "")
                                     .replace("\\[", "")
                                     .replace("\\]", "")
                            )
                            html_parts.append(fallback)
                        continue

                if child.name == "img":
                    src = child.get("src", "")
                    if src and "math100.ru" in src:
                        full_url = urljoin(VARIANT_URL, src)
                        fname = hashlib.md5(full_url.encode()).hexdigest() + ".png"
                        fpath = os.path.join(IMAGES_DIR, fname)
                        if download_image(full_url, fpath):
                            try:
                                img = PILImage.open(fpath)
                                iw, ih = img.size
                                max_w = PAGE_WIDTH - 40 * mm
                                max_h = 150 * mm
                                sc = min(max_w / iw, max_h / ih, 1.0)
                                html_parts.append(
                                    f'<img src="{fpath}" width="{iw*sc}" height="{ih*sc}"/>'
                                )
                            except Exception as e:
                                print(f"  [!] Ошибка чтения изображения: {e}")
                            continue

                # Рекурсивно обрабатываем вложенные элементы
                nested = process_inline_math(child)
                if nested is not None:
                    if isinstance(nested, Paragraph):
                        html_parts.append(nested.text)
                    else:
                        html_parts.append(nested)

            combined = "".join(html_parts).strip()
            if combined:
                return Paragraph(combined, style_text)
            return None

        if el.name == "h1":
            return None

        if el.name == "h3":
            text = el.get_text(strip=True)
            if text and "Решение задач" not in text:
                return Paragraph(f"<b>{escape_xml(text)}</b>", style_text)
            return None

        # Прочие элементы — рекурсия
        parts = []
        for child in el.children:
            r = process_inline_math(child)
            if r is not None:
                if isinstance(r, Paragraph):
                    parts.append(r.text)
                else:
                    parts.append(r)
        if parts:
            return "".join(parts)
        return None

    # Обрабатываем все дочерние элементы контента
    for child in content.children:
        r = process_inline_math(child)
        if r is not None:
            flowables.append(r)

    # Если ничего не нашли, пробуем весь текст
    if not flowables or len(flowables) <= 1:
        full_text = content.get_text(separator="\n", strip=True)
        if full_text:
            for line in full_text.split("\n"):
                line = line.strip()
                if line:
                    flowables.append(Paragraph(escape_xml(line), style_text))

    # Ссылка на решение
    flowables.append(Spacer(1, 6 * mm))
    flowables.append(Paragraph("Решение и ответ:", style_link_title))
    flowables.append(Paragraph(
        f'<a href="{solution_url}" color="#1a73e8">{solution_url}</a>',
        style_link
    ))

    # Разделитель
    flowables.append(Spacer(1, 8 * mm))
    flowables.append(Table(
        [[""]],
        colWidths=[PAGE_WIDTH - 40 * mm],
        rowHeights=[0.5],
    ))
    flowables.append(Spacer(1, 8 * mm))

    return flowables


def escape_xml(text):
    """Экранирует спецсимволы для XML/reportlab."""
    text = text.replace("&", "&amp;")
    text = text.replace("<", "&lt;")
    text = text.replace(">", "&gt;")
    text = text.replace('"', "&quot;")
    text = text.replace("'", "&apos;")
    return text


def build_pdf(variant_url, output_path):
    """Основная функция: скачивает задания и собирает PDF."""
    print(f"=== Генератор PDF вариантов ЕГЭ ===")
    print(f"Страница варианта: {variant_url}")
    print(f"Выходной файл: {output_path}")
    print()

    # Создаём директории
    os.makedirs(IMAGES_DIR, exist_ok=True)
    os.makedirs(LATEX_DIR, exist_ok=True)

    # Регистрируем шрифты
    print("[1/4] Поиск шрифтов...")
    font_name, font_bold = register_fonts()
    print(f"  Шрифт: {font_name}")

    # Получаем список задач
    print("[2/4] Загрузка списка задач...")
    task_urls = get_task_urls(variant_url)
    print(f"  Найдено задач: {len(task_urls)}")

    # Скачиваем и парсим каждую задачу
    print("[3/4] Загрузка и парсинг задач...")
    all_flowables = []

    for i, url in enumerate(task_urls, 1):
        task_num = i
        print(f"  Задача {task_num}/{len(task_urls)}: {url}")

        try:
            html = fetch_page(url)
            task_data = parse_task(html, url)
            if not task_data:
                print(f"  [!] Не удалось распарсить задачу {task_num}")
                continue

            flowables = html_to_flowables(
                task_data["html"], task_num,
                task_data["solution_url"],
                task_data["images"],
                font_name, font_bold,
            )
            all_flowables.extend(flowables)

        except Exception as e:
            print(f"  [!] Ошибка: {e}")

        # Задержка чтобы не нагружать сервер
        if i < len(task_urls):
            time.sleep(REQUEST_DELAY)

    if not all_flowables:
        print("\n[!] Не удалось загрузить ни одной задачи!")
        return

    # Собираем PDF
    print("[4/4] Сборка PDF...")
    doc = SimpleDocTemplate(
        output_path,
        pagesize=A4,
        leftMargin=20 * mm,
        rightMargin=20 * mm,
        topMargin=20 * mm,
        bottomMargin=20 * mm,
    )

    # Добавляем заголовок документа
    style_doc_title = ParagraphStyle(
        "DocTitle",
        fontName=font_bold,
        fontSize=20,
        leading=26,
        alignment=TA_CENTER,
        spaceAfter=8,
    )
    style_doc_subtitle = ParagraphStyle(
        "DocSubtitle",
        fontName=font_name,
        fontSize=12,
        leading=16,
        alignment=TA_CENTER,
        spaceAfter=20,
    )

    title_flowables = [
        Spacer(1, 30 * mm),
        Paragraph("ЕГЭ Профиль", style_doc_title),
        Paragraph("Вариант заданий с math100.ru", style_doc_subtitle),
        Paragraph(
            f'<a href="{variant_url}" color="#1a73e8">{variant_url}</a>',
            style_doc_subtitle
        ),
        Spacer(1, 20 * mm),
        PageBreak(),
    ]

    doc.build(title_flowables + all_flowables)

    file_size = os.path.getsize(output_path) / 1024
    print(f"\n=== Готово! ===")
    print(f"Файл: {output_path} ({file_size:.0f} КБ)")
    print(f"Задач: {len(task_urls)}")


# ── Точка входа ───────────────────────────────────────────────

def make_unique_filename(variant_url):
    """
    Формирует уникальное имя выходного PDF на основе URL варианта.
    Например: https://math100.ru/prof-ege-2027-10-1/  ->  prof-ege-2027-10-1.pdf
    Если сегмент не определяется, добавляет соль-хэш от URL.
    """
    # Извлекаем последний meaningful сегмент пути
    segs = [s for s in variant_url.rstrip("/").split("/") if s]
    base = None
    if segs:
        candidate = segs[-1]
        # Нормализуем до безопасных символов
        candidate = re.sub(r"[^A-Za-z0-9_-]", "_", candidate)
        if candidate:
            base = candidate

    if not base:
        base = "variant"

    # Если такое имя уже существует в текущей папке, добавляем числовой суффикс
    counter = 0
    candidate = f"{base}.pdf"
    while os.path.exists(candidate):
        counter += 1
        candidate = f"{base}_{counter}.pdf"

    return candidate


if __name__ == "__main__":
    variant_url = VARIANT_URL
    output = None

    if len(sys.argv) > 1:
        variant_url = sys.argv[1]
    if len(sys.argv) > 2:
        output = sys.argv[2]

    if output is None:
        output = make_unique_filename(variant_url)
        print(f"Выходной файл: {output}")

    build_pdf(variant_url, output)
