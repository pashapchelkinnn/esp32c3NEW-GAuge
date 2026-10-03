#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Полный набор проверок прошивки Smart Gauge (без компилятора ESP32).

1. site_assets.h == автогенерация из index.html/styles.css/app.js/i18n.js
2. mochi_frames.h: MOCHI_W/H <= 160, каждый фрейм = ceil(W/8)*H байт,
   таблица указателей полная и без дыр; симуляция drawMochiFrame()
3. 160X160Logo.c: data_size == w*h*2 (TRUE_COLOR при LV_COLOR_DEPTH=16)
4. gear72.c: заголовок lv_font_t согласован (размер/символы)
5. .ino: баланс {} (строги к скобкам вне комментариев), все web->on(...)
   обработчики объявлены, extern-и имеют определения в проекте
6. сайт: app.js/i18n.js синтаксис (node --check / vm), ключи i18n покрыты,
   id из app.js существуют в index.html, все fetch-URL есть в роутерах .ino
7. make_gimg.py: генерация тестового .gimg + парсер-валидатор по правилам
   mediaOpenFile()/mediaUploadCb() из прошивки

Запуск:  python3 tools/tests/run_all.py   (exit 0 = всё зелёное)
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import build_site  # noqa: E402

FAILED = []
PASSED = []


def ok(name):
    PASSED.append(name)
    print(f"  [OK]   {name}")


def fail(name, why):
    FAILED.append(name)
    print(f"  [FAIL] {name}: {why}")


def check(name, fn):
    try:
        msg = fn()
        ok(name + (f" — {msg}" if msg else ""))
    except AssertionError as e:
        fail(name, e)
    except Exception as e:
        fail(name, f"исключение: {e!r}")


# ---------------------------------------------------------------- 1. site_assets
def t_site_assets():
    fresh = build_site.build(ROOT)
    cur = (ROOT / "site_assets.h").read_bytes()
    assert fresh == cur, "site_assets.h расходится с автогенерацией (запусти tools/build_site.py)"
    txt = cur.decode()
    for _, sym in build_site.ASSETS:
        m = re.search(rf"static const uint32_t {sym}_LEN = (\d+);", txt)
        assert m, f"{sym}_LEN не найден"
        arr = build_site.extract_array(txt, sym)
        assert arr is not None, f"{sym} массив не найден"
        assert int(m.group(1)) == len(arr), f"{sym}_LEN={m.group(1)} != длина массива {len(arr)}"
    return "бит-в-бит совпадает, _LEN корректны"


# ---------------------------------------------------------------- 2. mochi frames
def parse_mochi():
    src = (ROOT / "mochi_frames.h").read_text()
    W = int(re.search(r"#define MOCHI_W (\d+)", src).group(1))
    H = int(re.search(r"#define MOCHI_H (\d+)", src).group(1))
    N = int(re.search(r"#define MOCHI_FRAMES (\d+)", src).group(1))
    frames = {}
    for m in re.finditer(r"static const uint8_t mochi_frame_(\d+)\[\] PROGMEM = \{(.*?)\};", src, re.S):
        frames[int(m.group(1))] = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
    tbl = re.search(r"static const uint8_t\* mochi_frames\[\] PROGMEM = \{(.*?)\};", src, re.S)
    ptrs = [int(n) for n in re.findall(r"mochi_frame_(\d+)", tbl.group(1))] if tbl else []
    return W, H, N, frames, ptrs


def t_mochi():
    W, H, N, frames, ptrs = parse_mochi()
    assert W <= 160 and H <= 160, f"#error в скетче: {W}x{H} > 160x160"
    exp_len = ((W + 7) // 8) * H
    bad = [i for i, b in frames.items() if len(b) != exp_len]
    assert not bad, f"фреймы неверного размера ({exp_len} байт ожидалось): {bad[:5]}"
    assert len(frames) == N, f"MOCHI_FRAMES={N}, а фреймов {len(frames)}"
    assert ptrs == list(range(N)), "таблица указателей неполная/не по порядку"
    # индексация анимаций из mochiTick: кадры до 59 должны существовать
    assert N >= 60, f"прошивка использует кадры 0..59, а их только {N}"
    return f"{W}x{H}, {N} фреймов по {exp_len} байт"


def t_mochi_render_sim():
    """Симулируем drawMochiFrame(): чтение row[x>>3]>>(7-(x&7)) не за границами."""
    W, H, N, frames, _ = parse_mochi()
    for idx in (0, 12, 29, 40, 59):
        f = frames[idx]
        for y in range(H):
            base = (y * W) // 8
            assert base + (W - 1) // 8 < len(f), f"frame {idx}: выход за буфер в строке {y}"
    return "все используемые кадры рендерятся без выхода за границы"


# ---------------------------------------------------------------- 3. logo
def t_logo():
    src = (ROOT / "160X160Logo.c").read_text()
    w = int(re.search(r"\.header\.w = (\d+)", src).group(1))
    h = int(re.search(r"\.header\.h = (\d+)", src).group(1))
    ds = int(re.search(r"\.data_size = (\d+) \* LV_COLOR_SIZE / 8", src).group(1))
    assert ds == w * h, f"data_size {ds} != w*h {w*h} (TRUE_COLOR требует ровно w*h пикселей)"
    map_m = re.search(r"uint8_t img_logo_map\[\] = \{(.*?)\n\};", src, re.S)
    body = map_m.group(1)
    # отбрасываем блоки #if для другой глубины цвета: считаем TRUE_COLOR ветку
    chunks = re.findall(r"/\*Pixel format:.*?\*/(.*?)(?=/\*Pixel format:|#if|#endif|\Z)", body, re.S)
    hexes = [hx for chunk in chunks for hx in re.findall(r"0x([0-9a-fA-F]{2})", chunk)]
    # первая секция (до #elif) соответствует 16-битному TRUE_COLOR
    first = re.split(r"#elif|#else", body)[0]
    n16 = len(re.findall(r"0x[0-9a-fA-F]{2}", first))
    assert n16 == w * h * 2, f"байт данных {n16}, ожидалось {w*h*2} (w*h*2 для RGB565)"
    return f"logo {w}x{h}, data_size согласован, {n16} байт данных"


# ---------------------------------------------------------------- 4. font
def t_font():
    src = (ROOT / "gear72.c").read_text()
    assert "const lv_font_t gear72" in src or "lv_font_t gear72 =" in src, "определение gear72 не найдено"
    size = int(re.search(r"\.line_height = (\d+)", src).group(1))
    glyphs = len(re.findall(r"\.bitmap_index = ", src)) + 1  # + .notdef
    syms = re.search(r"--symbols ([^ ]+)", src)
    expect = len(set(syms.group(1))) if syms else None
    assert syms is None or glyphs == expect + 1 or glyphs == expect, \
        f"glyph_count={glyphs}, а символов в opt {expect}(+notdef)"
    dsc = re.search(r"\.dsc = \(const lv_font_t\)\{ \{(.*?)\}\}", src, re.S)
    assert dsc, "dsc шрифта не найден"
    ranges = re.findall(r"\{(\d+), (\d+), (\w+)", dsc.range(1) if False else dsc.group(1))
    assert ranges, "нет range-таблицы"
    return f"шрифт ok, line_height={size}, глифов={glyphs}"


# ---------------------------------------------------------------- 5. sketch
def strip_cpp(src: str) -> str:
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r'"(?:\\.|[^"\\])*"', '""', src)
    src = re.sub(r"'(?:\\.|[^'\\])'", "''", src)
    return src


def t_sketch_braces():
    src = (ROOT / "1ESP32_C3_wireles_obd_mochi_web_pictures.ino").read_text()
    s = strip_cpp(src)
    assert s.count("{") == s.count("}"), f"баланс {{}}: {s.count('{')} vs {s.count('}')}"
    assert s.count("(") == s.count(")"), f"баланс (): {s.count('(')} vs {s.count(')')}"
    return f"{src.count(chr(10))+1} строк, скобки сбалансированы"


def t_sketch_routes():
    src = (ROOT / "1ESP32_C3_wireles_obd_mochi_web_pictures.ino").read_text()
    handlers = set(re.findall(r"web->on\(\"[^\"]+\",HTTP_\w+,(\w+)", src))
    declared = set(re.findall(r"\bstatic\s+(?:void|bool|int)\s+(\w+)\s*\(", src))
    missing = {h for h in handlers if h not in declared}
    assert not missing, f"обработчики роутов не определены: {missing}"
    return f"{len(handlers)} именованных обработчиков объявлены"


def t_sketch_extern():
    src = (ROOT / "1ESP32_C3_wireles_obd_mochi_web_pictures.ino").read_text()
    ext = re.findall(r"extern const lv_img_dsc_t (\w+);", src)
    logo = (ROOT / "160X160Logo.c").read_text()
    for name in ext:
        assert re.search(rf"lv_img_dsc_t {name}\s*=", logo), f"{name} не определён в 160X160Logo.c"
    assert "LV_FONT_DECLARE(gear72);" in src
    assert re.search(r"const lv_font_t gear72", (ROOT / "gear72.c").read_text()), "gear72 не определён"
    return f"extern {ext} + gear72 имеют определения"


def t_sketch_symbols():
    """Ключевые константы из .h используются корректно."""
    src = (ROOT / "1ESP32_C3_wireles_obd_mochi_web_pictures.ino").read_text()
    _, _, N, _, _ = parse_mochi()
    used = [int(x) for x in re.findall(r"frame=(\d+)", src)] + \
           [int(x) for x in re.findall(r"frame>(\d+)", src)] + \
           [int(x) for x in re.findall(r"drawMochiFrame\((\d+)", src)]
    assert max(used) < N, f"анимация ссылается на кадр {max(used)}, но фреймов всего {N}"
    refs = re.findall(r"(INDEX_HTML|APP_JS|I18N_JS|STYLES_CSS)(?:_LEN)?", src)
    ah = (ROOT / "site_assets.h").read_text()
    for sym in {"INDEX_HTML", "APP_JS", "I18N_JS", "STYLES_CSS"}:
        assert f"{sym}[] PROGMEM" in ah and f"{sym}_LEN" in ah, f"{sym} нет в site_assets.h"
        assert sym in refs, f"{sym} не используется в скетче"
    return "кадры анимаций в диапазоне, все ассеты подключены"


# ---------------------------------------------------------------- 6. сайт
def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def t_js_syntax():
    for f in ("app.js", "i18n.js"):
        r = run(["node", "--check", str(ROOT / f)])
        assert r.returncode == 0, f"{f}: {r.stderr.strip()[:300]}"
    return "node --check: app.js, i18n.js без синтаксических ошибок"


def t_html_ids():
    html = (ROOT / "index.html").read_text()
    js = (ROOT / "app.js").read_text()
    ids = set(re.findall(r'id="([^"]+)"', html))
    used = set(re.findall(r"\$\('#([\w-]+)'\)", js)) | set(re.findall(r"getElementById\('([\w-]+)'\)", js))
    missing = used - ids
    assert not missing, f"id из app.js отсутствуют в index.html: {sorted(missing)}"
    return f"{len(used)} селекторов '#id' из app.js присутствуют в разметке"


def t_i18n_keys():
    js = (ROOT / "app.js").read_text() + (ROOT / "index.html").read_text()
    i18n = (ROOT / "i18n.js").read_text()
    used = set(re.findall(r"t\('([\w.]+)'", js)) | set(re.findall(r'data-i18n="([\w.]+)"', js))
    ru_block = re.search(r"I18N\.ru\s*=\s*\{(.*?)\n\}", i18n, re.S)
    en_block = re.search(r"I18N\.en\s*=\s*\{(.*?)\n\}", i18n, re.S)
    assert ru_block and en_block, "блоки I18N.ru / I18N.en не найдены"
    ru = set(re.findall(r"'([\w.]+)'\s*:", ru_block.group(1)))
    en = set(re.findall(r"'([\w.]+)'\s*:", en_block.group(1)))
    miss_ru = used - ru
    miss_en = used - en
    assert not miss_ru, f"ключей нет в RU: {sorted(miss_ru)}"
    assert not miss_en, f"ключей нет в EN: {sorted(miss_en)}"
    only_ru = ru - en
    assert not only_ru, f"ключи есть только в RU: {sorted(only_ru)}"
    return f"{len(used)} используемых ключей покрыты RU+EN, словари синхронны ({len(ru)} ключей)"


def t_api_routes_match():
    """Каждый URL из fetch/post в app.js существует среди web->on в скетче."""
    js = (ROOT / "app.js").read_text()
    src = (ROOT / "1ESP32_C3_wireles_obd_mochi_web_pictures.ino").read_text()
    routes = set(re.findall(r'web->on\("([^"]+)"', src))
    urls = set(re.findall(r"(?:api|post)\('(/[^']+)'", js)) | set(re.findall(r"open\('POST','(/[^']+)'\)", js))
    xhr = set(re.findall(r"xhr\.open\('POST','(/[^']+)'\)", js))
    urls |= xhr
    dynamic = {u for u in urls if "%" in u or "'" in u}
    urls -= dynamic
    missing = {u for u in urls if u not in routes}
    assert not missing, f"app.js зовёт несуществующие роуты: {sorted(missing)}"
    return f"{len(urls)} клиентских URL соответствуют {len(routes)} серверным роутам"


# ---------------------------------------------------------------- 7. gimg
def t_gimg_roundtrip():
    import struct as st
    out = Path("/tmp/test_media.gimg")
    # генерируем тестовое изображение через PIL, если есть; иначе — вручную валидный файл
    script = ROOT / "tools" / "make_gimg.py"
    png = Path("/tmp/test_frame.png")
    r = run([sys.executable, "-c",
             "from PIL import Image; im=Image.new('RGB',(100,80),(255,120,0)); im.save('/tmp/test_frame.png')"])
    if r.returncode == 0:
        rr = run([sys.executable, str(script), str(out), str(png), "--fps", "8"])
        assert rr.returncode == 0, rr.stderr
        blob = out.read_bytes()
    else:  # Pillow недоступен — собираем заголовок вручную и проверяем парсер
        hdr = bytearray(st.pack("<4sBBB", b"GIMG", 2, 1, 8)) + bytes(9)
        blob = bytes(hdr) + b"\x00" * (240 * 240 * 2)
    # --- валидация ровно по логике прошивки (mediaOpenFile + mediaUploadCb) ---
    def fw_validate(data):
        if len(data) < 16: return "короткий заголовок"
        h = data[:16]
        if h[:4] != b"GIMG" or h[4] != 2: return "магия/версия"
        frames, fps = h[5], h[6]
        if not (1 <= frames <= 4): return f"frames={frames} вне 1..4 (MEDIA_MAX_FRAMES)"
        fs = 240 * 240 * 2
        if len(data) < 16 + frames * fs: return "файл короче обещанных кадров"
        if len(data) > 600000: return f"размер {len(data)} > лимита загрузки 600000"
        return None
    err = fw_validate(blob)
    assert err is None, f"прошивка отвергла бы файл: {err}"
    # негативные кейсы: 5 кадров, битая магия, обрезанный файл
    bad1 = bytearray(blob); bad1[5] = 5
    assert fw_validate(bytes(bad1)), "валидатор должен отвергать 5 кадров"
    bad2 = bytearray(blob); bad2[0:4] = b"XXXX"
    assert fw_validate(bytes(bad2)), "валидатор должен отвергать битую магию"
    assert fw_validate(blob[:100]), "валидатор должен отвергать обрезанный файл"
    return f"media.gimg принят валидатором прошивки ({len(blob)} байт)"


# ---------------------------------------------------------------- main
def main():
    print("== Проверки прошивки Smart Gauge ==")
    tests = [
        ("site_assets.h актуален (автогенерация)", t_site_assets),
        ("mochi_frames.h структура", t_mochi),
        ("mochi render-симуляция", t_mochi_render_sim),
        ("160X160Logo.c согласован", t_logo),
        ("gear72.c шрифт", t_font),
        (".ino баланс скобок", t_sketch_braces),
        (".ino роуты/обработчики", t_sketch_routes),
        (".ino extern-символы", t_sketch_extern),
        (".ino использование ассетов", t_sketch_symbols),
        ("JS синтаксис (node)", t_js_syntax),
        ("HTML id <-> app.js", t_html_ids),
        ("i18n ключи RU/EN", t_i18n_keys),
        ("API-роуты сайта <-> прошивка", t_api_routes_match),
        ("media.gimg round-trip", t_gimg_roundtrip),
    ]
    for name, fn in tests:
        check(name, fn)
    print(f"\nИТОГ: {len(PASSED)} OK, {len(FAILED)} FAIL")
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
