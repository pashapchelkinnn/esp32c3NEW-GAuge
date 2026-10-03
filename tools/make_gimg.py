#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Генератор media.gimg для Smart Gauge (ESP32-2424S012, GC9A01 240x240).

Формат (строго как проверяет прошивка, см. mediaOpenFile/mediaUploadCb):
  [0..3]   'GIMG'          магия
  [4]      version = 2
  [5]      frames count    (1..MEDIA_MAX_FRAMES=4)
  [6]      fps             (прошивка ограничивает 1..12)
  [7..15]  0               (резерв)
  далее    N x 115200 байт RGB565 BE кадров 240x240

Использование:
  python3 tools/make_gimg.py out.gimg frame1.png [frame2.jpg ...] [--fps 10] [--size 240]
  python3 tools/make_gimg.py out.gimg anim.gif --from-gif [--fps-override 10]
"""
import argparse
import struct
import sys

try:
    from PIL import Image
except ImportError:
    Image = None

MAGIC = b"GIMG"
VERSION = 2
FW_W, FW_H = 240, 240
MAX_FRAMES = 4          # MEDIA_MAX_FRAMES в прошивке
MAX_FPS = 12            # прошивка режет fps выше 12
FRAME_BYTES = FW_W * FW_H * 2


def rgb565_be(px: int) -> int:
    """RGB888 packed int -> RGB565 big-endian word (как ждёт LVGL TRUE_COLOR_HI)."""
    r, g, b = (px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def encode_frame(img: "Image.Image") -> bytes:
    """Любое изображение -> RGB565 BE, точно 240x240 (contain на чёрный фон)."""
    img = img.convert("RGBA")
    canvas = Image.new("RGBA", (FW_W, FW_H), (0, 0, 0, 255))
    im = img.copy()
    im.thumbnail((FW_W, FW_H), Image.LANCZOS)
    canvas.paste(im, ((FW_W - im.width) // 2, (FW_H - im.height) // 2), im)
    rgb = canvas.convert("RGB")
    data = rgb.tobytes()  # 240*240*3, порядок R,G,B
    out = bytearray(FRAME_BYTES)
    j = 0
    for i in range(0, len(data), 3):
        w = rgb565_be((data[i] << 16) | (data[i + 1] << 8) | data[i + 2])
        out[j] = w >> 8
        out[j + 1] = w & 0xFF
        j += 2
    return bytes(out)


def load_frames(path: str, from_gif: bool):
    if Image is None:
        sys.exit("Нужен Pillow: pip install Pillow")
    img = Image.open(path)
    if from_gif and getattr(img, "n_frames", 1) > 1:
        frames = []
        for i in range(img.n_frames):
            img.seek(i)
            frames.append(encode_frame(img.convert("RGBA")))
        dur = img.info.get("duration", 100)
        fps = max(1, min(MAX_FPS, round(1000 / dur)))
        return frames, fps
    return [encode_frame(img)], 10


def main():
    ap = argparse.ArgumentParser(description="Сборка media.gimg")
    ap.add_argument("output", help="куда писать .gimg")
    ap.add_argument("inputs", nargs="+", help="картинки (до 4) или гиф с --from-gif")
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--from-gif", action="store_true", help="развернуть GIF в кадры")
    a = ap.parse_args()

    frames = []
    fps = a.fps
    for p in a.inputs:
        fr, f_fps = load_frames(p, a.from_gif)
        frames.extend(fr)
        if a.from_gif:
            fps = f_fps

    if not (1 <= len(frames) <= MAX_FRAMES):
        sys.exit(f"ОШИБКА: прошивка принимает 1..{MAX_FRAMES} кадров, получилось {len(frames)}."
                 f" Для GIF уменьшите число кадров (каждый N-й).")
    if not (1 <= fps <= MAX_FPS):
        sys.exit(f"ОШИБКА: fps должен быть 1..{MAX_FPS}, получено {fps}")

    hdr = bytearray(struct.pack("<4sBBB", MAGIC, VERSION, len(frames), fps))
    hdr += bytes(16 - len(hdr))
    blob = bytes(hdr) + b"".join(frames[:MAX_FRAMES])

    with open(a.output, "wb") as f:
        f.write(blob)

    need = 16 + len(frames) * FRAME_BYTES
    print(f"{a.output}: {len(frames)} кадр(ов), fps={fps}, {len(blob)} байт "
          f"(ожидание прошивки: >= {need}) {'OK' if len(blob) >= need else 'FAIL'}")


if __name__ == "__main__":
    main()
