#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
embed_mathjax_fonts.py — сборочный шаг: встраивание шрифтов MathJax 3 (CHTML)
в виде data: URIs (base64), чтобы браузер не делал НИ ОДНОГО cross-origin
запроса на шрифты (схема qrc: не в CORS-whitelist Chromium; data: — в whitelist).

ПОЧЕМУ МЕТКА — JS, А НЕ CSS (важно, читай перед правкой):
  В этом репозитории CSS-файлов с @font-face НЕТ. Механика загрузки шрифтов
  в MathJax 3.2.2 (resources/mathjax/es5/tex-mml-chtml.js, единственный
  JS-файл MathJax в qrc):
    1. В JS (модуль CHTML output) есть объект defaultFonts — 22 шаблона вида
       src: 'url("%%URL%%/MathJax_<Name>.woff") format("woff")'.
    2. При старте MathJax addFontURLs() делает u.src.replace(/%%URL%%/, fontURL),
       где fontURL = Package.resolvePath("output/chtml/fonts/woff-v2") =
       <директория скрипта>/output/chtml/fonts/woff-v2
       (для qrc-копии: qrc:/mathjax/es5/output/chtml/fonts/woff-v2).
    3. MathJax инжектит собранные @font-face в стили документа; браузер
       запрашивает qrc:/.../*.woff — кросс-оригин к origin страницы
       (https://math100.ru / null) -> CORS-блок -> фолбэк на системные шрифты.
  Поэтому скрипт заменяет В САМОМ JS-ФАЙЛЕ каждый
       url("%%URL%%/<Name>.woff")  ->  url("data:font/woff;base64,...")
  После замены плейсхолдер %%URL%% в src отсутствует и replace() в
  addFontURLs() ничего не делает — data: URI остаётся, запросов на шрифты нет.
  (Литерал регулярки /%%URL%%/ в коде replace() остаётся — это код, не URL.)
  Скрипт ТАКЖЕ (generic-путь, на будущее/на всякий случай) переписывает
  url(...woff/woff2/ttf) в *.css файлах дерева, если такие появятся.

Идемпотентность:
  - url(...), уже начинающиеся с data:, не трогаются;
  - JS-замена ищет только плейсхолдер %%URL%% внутри url(...) — в уже
    встроенном файле таких ссылок нет.
  - out-дерево пересоздаётся с нуля при каждом запуске.

Только stdlib: pathlib, re, base64, argparse, shutil, sys.

Запуск (из корня проекта):
    python3 scripts/embed_mathjax_fonts.py
    python3 scripts/embed_mathjax_fonts.py --src resources/mathjax \
        --out build/mathjax-embedded --qrc build/mathjax-embedded.qrc

Выходные коды:
    0 — успех (включая предупреждения: шрифты не найдены / не переписано
        ни одного url / остались ссылки);
    1 — жёсткая ошибка (нет src, src не каталог, сбой копирования/записи).
"""

import argparse
import base64
import re
import shutil
import sys
from pathlib import Path

FONT_EXTS = (".woff", ".woff2", ".ttf")
EXT_MIME = {
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".ttf": "font/ttf",
}
# Magic bytes для контрольной сверки (важный факт: файлы
# resources/mathjax/fonts/*.woff — это WOFF v1, magic "wOFF", а НЕ woff2,
# несмотря на "woff-v2" в URL-пути, который запрашивает MathJax).
MAGIC_MIME = {
    b"wOF2": "font/woff2",
    b"wOFF": "font/woff",
    b"\x00\x01\x00\x00": "font/ttf",
    b"\x00\x00\x00\xdc": "font/ttf",  # OpenCollection
}

# url(<value>.woff|woff2|ttf) — кавычки опциональны (CSS допускает без них).
URL_RE = re.compile(r"""url\(\s*(?P<q>["']?)(?P<val>[^"')]+?)(?P=q)\s*\)""",
                    re.IGNORECASE)
# JS-шаблон MathJax CHTML: url("%%URL%%/<Name>.woff") — только с плейсхолдером.
JS_FONT_URL_RE = re.compile(
    r"""url\(\s*(?P<q>["'])\s*%%URL%%/(?P<name>[^"'\(\)]+?\.(?:woff2?|ttf))\s*(?P=q)\s*\)""",
    re.IGNORECASE)
# Контрольная регулярка: остались ли url(...) на шрифты, НЕ data:.
REMAINING_RE = re.compile(
    r"""url\(\s*["']?(?!data:)[^"')]*?\.(?:woff2?|ttf)""", re.IGNORECASE)


class Report:
    def __init__(self):
        self.warnings = []
        self.rewritten = []            # (file_rel, old_ref, data_uri_len)
        self.css_files_scanned = 0
        self.css_files_rewritten = 0
        self.css_urls = 0
        self.js_files_scanned = 0
        self.js_files_rewritten = 0
        self.embedded = {}             # name -> dict(mime=, orig=, uri_len=)
        self.copied_files = 0
        self.copied_bytes = 0

    def warn(self, msg):
        self.warnings.append(msg)
        print("  [WARN] " + msg)


def read_text(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def rel(p, root):
    try:
        return str(p.relative_to(root))
    except ValueError:
        return str(p)


def build_data_uri(font_path, rep, ctx):
    """data: URI для файла шрифта + контрольная сверка magic bytes."""
    ext = font_path.suffix.lower()
    data = font_path.read_bytes()
    head = data[:4]
    mime = EXT_MIME[ext]
    if head in MAGIC_MIME and MAGIC_MIME[head] != mime:
        rep.warn("%s: magic %r не совпадает с расширением %s (MIME по "
                 "расширению: %s)" % (ctx, head, ext, mime))
    b64 = base64.b64encode(data).decode("ascii")
    uri = "data:%s;base64,%s" % (mime, b64)
    rep.embedded[font_path.name] = {
        "mime": mime, "orig": len(data), "uri_len": len(uri)}
    return uri


def process_css_file(css, out_root, rep, font_map):
    """Generic-путь: url(...woff/woff2/ttf) в CSS -> data: URI.
    В этом репозитории CSS-файлов с шрифтами НЕТ (механика — через JS, см.
    докстринг), путь оставлен для надёжности/будущего."""
    try:
        text = read_text(css)
    except (OSError, UnicodeDecodeError) as e:
        rep.warn("css: не удалось прочитать %s: %s" % (rel(css, out_root), e))
        return
    changed = False

    def sub(m):
        nonlocal changed
        val = m.group("val").strip()
        if val.startswith("data:"):                    # идемпотентность
            return m.group(0)
        if not val.lower().endswith(FONT_EXTS):
            return m.group(0)
        rep.css_urls += 1
        # Встраиваем только относительные пути, разрешимые от каталога css.
        if re.match(r"^[a-zA-Z][a-zA-Z0-9+.\-]*:", val) or val.startswith("//"):
            rep.warn("css %s: внешний/абсолютный url %s — не встраивается"
                     % (rel(css, out_root), val))
            return m.group(0)
        cand = (css.parent / val).resolve()
        name = cand.name
        if name in font_map:
            uri = font_map[name]
        elif cand.is_file():
            uri = build_data_uri(cand, rep, "css %s" % rel(css, out_root))
            font_map[name] = uri
        else:
            rep.warn("css %s: файл %s не найден — url не изменён"
                     % (rel(css, out_root), val))
            return m.group(0)
        changed = True
        rep.rewritten.append((rel(css, out_root), val, len(uri)))
        return 'url("%s")' % uri

    new_text = URL_RE.sub(sub, text)
    if new_text != text:
        write_text(css, new_text)
        rep.css_files_rewritten += 1
    rep.css_files_scanned += 1


def process_js_file(js, out_root, rep, font_map, font_index):
    """Метка реального бага: url("%%URL%%/<Name>.woff") в JS -> data: URI."""
    try:
        text = read_text(js)
    except (OSError, UnicodeDecodeError) as e:
        rep.warn("js: не удалось прочитать %s: %s" % (rel(js, out_root), e))
        return
    rep.js_files_scanned += 1
    if "%%URL%%" not in text:
        return  # уже встроен или не MathJax-файл (идемпотентность)
    changed = False
    size_before = len(text.encode("utf-8"))

    def sub(m):
        nonlocal changed
        ref = m.group("name")
        fname = ref.split("/")[-1]
        if fname not in font_map:
            hit = next((f for f in font_index if f.name == fname), None)
            if hit is None:
                rep.warn("js %s: шрифт %s из url(%%URL%%/...) не найден в "
                         "дереве — ссылка не изменена" % (rel(js, out_root), ref))
                return m.group(0)
            font_map[fname] = build_data_uri(hit, rep, "js %s" % rel(js, out_root))
        uri = font_map[fname]
        changed = True
        rep.rewritten.append((rel(js, out_root), ref, len(uri)))
        return 'url("%s")' % uri

    new_text = JS_FONT_URL_RE.sub(sub, text)
    if changed:
        write_text(js, new_text)
        rep.js_files_rewritten += 1
    return size_before, len(new_text.encode("utf-8")) if changed else size_before


def count_remaining(out_root, rep):
    """Сколько в дереве осталось url(...) на шрифты без data: (цель — 0)."""
    remaining = 0
    for p in out_root.rglob("*"):
        if not p.is_file() or p.suffix.lower() not in (".js", ".css", ".html", ".mjs"):
            continue
        try:
            t = read_text(p)
        except (OSError, UnicodeDecodeError):
            continue
        n = len(REMAINING_RE.findall(t))
        remaining += n
        if n:
            rep.warn("%s: осталось %d url(...) на шрифты без data:"
                     % (rel(p, out_root), n))
        # %%URL%% вне литерала регулярки /%%URL%%/ (это код, не ссылка)
        leftover = len(re.findall(r"%%URL%%", t)) - len(re.findall(r"/%%URL%%/", t))
        if leftover:
            rep.warn("%s: осталось %d необработанных ссылок %%URL%%/..."
                     % (rel(p, out_root), leftover))
    return remaining


def main(argv=None):
    # Windows CI (ран 35444421956): stdout процесса-кастом-команды уходит в
    # пайп, и Python по умолчанию кодирует его ANSI-кодовой страницей
    # консоли (на англо-раннере — cp1252: ЛЮБАЯ кириллица = UnicodeEncodeError
    # -> MSB8066 -> падает сборка). Лог-захват GH Actions — UTF-8, поэтому
    # явно ставим UTF-8. Тот же урок, что и для PowerShell-скриптов:
    # нативный процесс не должен полагаться на locale.
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError, OSError):
            pass

    ap = argparse.ArgumentParser(
        description="Встраивание шрифтов MathJax 3 (CHTML) как data: URIs "
                    "на этапе сборки (анти-CORS-блок qrc-шрифтов в Chromium).")
    ap.add_argument("--src", default="resources/mathjax",
                    help="дерево MathJax в исходнике (по умолчанию resources/mathjax)")
    ap.add_argument("--out", default="build/mathjax-embedded",
                    help="куда копировать и встраивать "
                         "(по умолчанию build/mathjax-embedded)")
    ap.add_argument("--qrc", default=None,
                    help="опционально: куда генерировать .qrc с алиасами mathjax/* "
                         "(пути на build-дерево). По умолчанию не генерируется.")
    ap.add_argument("--qrc-include-fonts", action="store_true",
                    help="включить в генерируемый .qrc и сами файлы шрифтов "
                         "(по умолчанию только JS — после встраивания браузер "
                         "файлы шрифтов не запрашивает)")
    args = ap.parse_args(argv)

    src = Path(args.src)
    out = Path(args.out)

    print("=== embed_mathjax_fonts ===")
    print("src: %s" % src)
    print("out: %s" % out)

    if not src.is_dir():
        print("ERROR: src не найден или не каталог: %s" % src, file=sys.stderr)
        return 1

    # 1) Полная копия дерева src -> out (out пересоздаётся — идемпотентно).
    #    Дерево маленькое (24 файла / ~1.5 MB), копируем всё целиком: проще и
    #    надёжнее, чем вычленять поддерево по resources.qrc (1 js + 23 шрифта).
    if out.exists():
        shutil.rmtree(out)
    try:
        shutil.copytree(src, out)
    except OSError as e:
        print("ERROR: не удалось скопировать %s -> %s: %s" % (src, out, e),
              file=sys.stderr)
        return 1

    rep = Report()
    for p in out.rglob("*"):
        if p.is_file():
            rep.copied_files += 1
            rep.copied_bytes += p.stat().st_size
    print("копия: %d файлов, %d B (%.1f KB)"
          % (rep.copied_files, rep.copied_bytes, rep.copied_bytes / 1024.0))

    # 2) Индекс шрифтов дерева out (по базовому имени).
    font_index = sorted(p for p in out.rglob("*")
                        if p.is_file() and p.suffix.lower() in FONT_EXTS)
    if not font_index:
        rep.warn("в дереве не найдено ни одного шрифта (.woff/.woff2/.ttf) — "
                 "встраивать нечего (предупреждение, не ошибка)")
    font_map = {}
    for p in font_index:
        if p.name in font_map:
            rep.warn("дубликат имени шрифта: %s (используется первый)" % p.name)
            continue
        font_map[p.name] = build_data_uri(p, rep, p.name)

    # 3) CSS-путь (generic; в этом репо CSS с шрифтами отсутствуют).
    css_files = sorted({p for pat in ("*.css", "*.CSS") for p in out.rglob(pat)})
    for p in css_files:
        process_css_file(p, out, rep, font_map)

    # 4) JS-путь (метка бага: defaultFonts в tex-mml-chtml.js).
    js_files = sorted({p for pat in ("*.js", "*.JS") for p in out.rglob(pat)})
    for p in js_files:
        process_js_file(p, out, rep, font_map, font_index)

    # 5) Контроль результата.
    remaining = count_remaining(out, rep)

    # 6) Отчёт.
    used_names = sorted({r[1].split("/")[-1] for r in rep.rewritten})
    unused_names = sorted(set(font_map) - set(used_names))
    used_orig = sum(rep.embedded[n]["orig"] for n in used_names if n in rep.embedded)
    used_uri = sum(rep.embedded[n]["uri_len"] for n in used_names if n in rep.embedded)

    print("")
    print("--- ОТЧЁТ ---")
    print("css: просканировано %d, переписано %d (ссылок на шрифты в css: %d)"
          % (rep.css_files_scanned, rep.css_files_rewritten, rep.css_urls))
    print("js : просканировано %d, переписано %d"
          % (rep.js_files_scanned, rep.js_files_rewritten))
    print("шрифтов в дереве: %d; встроено data: URI: %d"
          % (len(font_map), len(used_names)))
    print("оригинальный размер встроенных шрифтов: %d B (%.1f KB)"
          % (used_orig, used_orig / 1024.0))
    print("размер data: URI (base64 + префиксы): %d B (%.1f KB)"
          % (used_uri, used_uri / 1024.0))
    if unused_names:
        sz = sum(rep.embedded[n]["orig"] for n in unused_names if n in rep.embedded)
        print("НЕ встроены (нет ссылок в js/css): %s [%.1f KB]"
              % (", ".join(unused_names), sz / 1024.0))
    print("url() переписано всего: %d (первые 5):" % len(rep.rewritten))
    for (f, ref, uri_len) in rep.rewritten[:5]:
        print("  %s: url(\"%%URL%%/%s\") -> data:...;base64,%d симв."
              % (f, ref, uri_len))
    if len(rep.rewritten) > 5:
        print("  ... ещё %d" % (len(rep.rewritten) - 5))
    print("осталось url(...) на woff/woff2/ttf без data:: %d" % remaining)

    ok = (remaining == 0 and (rep.rewritten or not font_index))
    if rep.warnings:
        print("предупреждений: %d" % len(rep.warnings))
    print("ИТОГ: %s" % (
        "OK — шрифты встроены data: URI, cross-origin запросов на шрифты "
        "быть не должно" if ok else
        "ВНИМАНИЕ — остались ссылки или шрифты не встроены, см. выше"))

    # 7) Опциональная генерация .qrc (для CMake-подключения, см. отчёт task).
    if args.qrc:
        qrc_path = Path(args.qrc)
        js_file = out / "es5" / "tex-mml-chtml.js"
        if not js_file.is_file():
            print("ERROR: --qrc задан, но %s отсутствует в out" % js_file,
                  file=sys.stderr)
            return 1
        # ВАЖНО: относительные пути (rcc резолвит их от директории .qrc-файла).
        # Абсолютные пути сломали бы CI (checkout на другом пути, task-16b fix).
        qrc_base = qrc_path.resolve().parent
        lines = ["<RCC>", '    <qresource prefix="/">']
        lines.append('        <file alias="mathjax/es5/tex-mml-chtml.js">%s</file>'
                     % js_file.resolve().relative_to(qrc_base).as_posix())
        if args.qrc_include_fonts:
            for f in sorted(font_index):
                af = f.resolve().relative_to(qrc_base).as_posix()
                lines.append('        <file alias="mathjax/fonts/%s">%s</file>'
                             % (f.name, af))
                lines.append('        <file alias="mathjax/es5/output/chtml/fonts/woff-v2/%s">%s</file>'
                             % (f.name, af))
        lines.append("    </qresource>")
        lines.append("</RCC>")
        qrc_path.parent.mkdir(parents=True, exist_ok=True)
        write_text(qrc_path, "\n".join(lines) + "\n")
        print("сгенерировано: %s (%s)"
              % (qrc_path, "js + файлы шрифтов" if args.qrc_include_fonts
                 else "только js — шрифты уже внутри JS data: URI"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
