#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Автогенерация site_assets.h из файлов сайта.

Использование:
    python3 tools/build_site.py            # собрать site_assets.h из корня проекта
    python3 tools/build_site.py --check    # проверить, что site_assets.h актуален (CI)
    python3 tools/build_site.py -o out.h   # собрать в другой файл

Формат выхода идентичен конвертеру site_converter.html:
  static const uint8_t ИМЯ[] PROGMEM = { 0x.., x32 в строке };
  static const uint32_t ИМЯ_LEN = N;
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path

# имя файла сайта -> имя массива в прошивке (порядок = порядок в .h)
ASSETS = [
    ("styles.css", "STYLES_CSS"),
    ("index.html", "INDEX_HTML"),
    ("app.js",     "APP_JS"),
    ("i18n.js",    "I18N_JS"),
]

BYTES_PER_LINE = 32


def bytes_to_c_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), BYTES_PER_LINE):
        chunk = data[i:i + BYTES_PER_LINE]
        lines.append(",".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def build(root: Path) -> bytes:
    out = ["#pragma once", "#include <stdint.h>"]
    report = []
    for fname, sym in ASSETS:
        p = root / fname
        if not p.is_file():
            print(f"ОШИБКА: не найден файл сайта: {p}", file=sys.stderr)
            sys.exit(1)
        data = p.read_bytes()
        out.append(f"static const uint8_t {sym}[] PROGMEM = {{")
        out.append(bytes_to_c_array(data))
        out.append("};")
        out.append(f"static const uint32_t {sym}_LEN = {len(data)};")
        md5 = hashlib.md5(data).hexdigest()[:8]
        report.append(f"  {fname:<12} -> {sym:<11} {len(data):>6} байт  md5:{md5}")
    sys.stderr.write("Сгенерировано:\n" + "\n".join(report) + "\n")
    return "".join(s + "\n" for s in out).encode("ascii")


def extract_array(header_text: str, sym: str):
    """Вернуть bytes массива SYM из содержимого site_assets.h или None."""
    m = re.search(rf"static const uint8_t {sym}\[\] PROGMEM = \{{(.*?)\}};",
                  header_text, re.S)
    if not m:
        return None
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))


def check(root: Path, target: Path) -> bool:
    """True, если target уже совпадает со свежей генерацией."""
    if not target.is_file():
        print(f"ПРОВЕРКА FAIL: {target.name} отсутствует!")
        return False
    fresh = build(root)
    current = target.read_bytes()
    if fresh == current:
        print(f"ПРОВЕРКА OK: {target.name} актуален ({len(current)} байт)")
        return True
    print(f"ПРОВЕРКА FAIL: {target.name} устарел — запусти: python3 tools/build_site.py")
    cur_txt = current.decode("ascii", "replace")
    for fname, sym in ASSETS:
        arr = extract_array(cur_txt, sym)
        disk = (root / fname).read_bytes() if (root / fname).is_file() else None
        if arr is None:
            print(f"  {sym}: не найден в текущем {target.name}")
        elif arr != disk:
            print(f"  {sym}: РАЗОШЁЛСЯ (в .h {len(arr)} байт, на диске {len(disk)} байт)")
        else:
            print(f"  {sym}: ok")
    return False


def main():
    ap = argparse.ArgumentParser(description="Генерация site_assets.h из файлов сайта")
    ap.add_argument("--root", default=".", help="папка с index.html/styles.css/app.js/i18n.js")
    ap.add_argument("-o", "--output", default=None, help="куда писать (по умолчанию <root>/site_assets.h)")
    ap.add_argument("--check", action="store_true", help="только проверка актуальности")
    a = ap.parse_args()

    root = Path(a.root).resolve()
    target = Path(a.output) if a.output else root / "site_assets.h"

    if a.check:
        sys.exit(0 if check(root, target) else 1)

    data = build(root)
    old = target.read_bytes() if target.is_file() else None
    if old == data:
        print(f"{target.name}: без изменений ({len(data)} байт)")
    else:
        target.write_bytes(data)
        print(f"{target.name}: записано {len(data)} байт")


if __name__ == "__main__":
    main()
