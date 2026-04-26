#!/usr/bin/env python3
"""Render the picker tab-bar icons (12x8) as PNGs for redrawing.

Re-implements the procedural drawing in device/nes_thumb.c so the output
matches what the device renders today. Writes two PNGs per icon:

    tab_icons/<name>.png      native 12x8
    tab_icons/<name>_8x.png   192x96, 8x nearest-neighbour for editing

After you redraw the native-size PNGs, run convert_tab_icons.py to emit
a C bitmap table that nes_thumb.c can blit directly.
"""
from __future__ import annotations

import os
from PIL import Image

W, H = 12, 8
SCALE = 1                       # device renders at scale 1 inside 12x8
UPSCALE = 8                     # editor-friendly upscale factor

# Background: magenta sentinel so the editor sees the "transparent" cells
# clearly. The on-device cell background is dim slate (0x10A2) or an
# active-tab grey (0x39E7) — using either of those would camouflage the
# accent black pixels (d-pad, connectors) and make editing painful.
BG = (255, 0, 255)

# Foreground tint used for the "active tab" colour on device (COL_TITLE =
# 0xFD20). Decoded from RGB565: R=31/31, G=41/63, B=0.
FG = (255, 166, 0)

# Hardcoded accent colours used inside the icons.
BLACK   = (0, 0, 0)
RED     = (255, 0, 0)               # 0xF800
GB_GREEN = (152, 188, 8)            # 0x9DE1 — Game Boy LCD on-pixel
GG_DARK  = (0, 41, 0)               # 0x0500 — dark green Game Gear screen


def new_canvas() -> Image.Image:
    return Image.new("RGB", (W, H), BG)


def fill_rect(img: Image.Image, x: int, y: int, w: int, h: int, c: tuple) -> None:
    px = img.load()
    for j in range(h):
        yy = y + j
        if yy < 0 or yy >= H:
            continue
        for i in range(w):
            xx = x + i
            if xx < 0 or xx >= W:
                continue
            px[xx, yy] = c


def icon_nes(img: Image.Image) -> None:
    s = SCALE
    cx, cy = W // 2, H // 2
    w, h = 12 * s, 6 * s
    x = cx - w // 2
    y = cy - h // 2
    fill_rect(img, x + s, y, w - 2 * s, h, FG)
    fill_rect(img, x, y + s, w, h - 2 * s, FG)
    btn_y = y + h // 2 - s // 2
    fill_rect(img, x + 7 * s, btn_y, s, s, BLACK)
    fill_rect(img, x + 9 * s, btn_y, s, s, BLACK)
    dx, dy = x + 2 * s, y + h // 2 - s
    fill_rect(img, dx, dy + s, 3 * s, s, BLACK)
    fill_rect(img, dx + s, dy, s, 3 * s, BLACK)


def icon_sms(img: Image.Image) -> None:
    s = SCALE
    cx, cy = W // 2, H // 2
    body_w, body_h = 8 * s, 6 * s
    x = cx - body_w // 2
    y = cy - body_h // 2 + s
    fill_rect(img, x, y, body_w, body_h, FG)
    handle_w = 4 * s
    fill_rect(img, cx - handle_w // 2, y - 2 * s, handle_w, 2 * s, FG)
    label_y = y + body_h // 2 - s // 2
    if label_y < y:
        label_y = y
    fill_rect(img, x, label_y, body_w, s, RED)
    fill_rect(img, x + s, y + body_h - s, s, s, BLACK)
    fill_rect(img, x + body_w - 2 * s, y + body_h - s, s, s, BLACK)


def icon_gb(img: Image.Image) -> None:
    s = SCALE
    cx, cy = W // 2, H // 2
    w, h = 6 * s, 9 * s
    x = cx - w // 2
    y = cy - h // 2
    fill_rect(img, x, y, w, h, FG)
    sw, sh = 4 * s, 3 * s
    lx = x + (w - sw) // 2
    ly = y + s
    fill_rect(img, lx, ly, sw, sh, BLACK)
    fill_rect(img, lx + 1, ly + 1, sw - 2, sh - 2, GB_GREEN)
    fill_rect(img, x + s, y + h - 3 * s, w - 2 * s, s, BLACK)
    fill_rect(img, x + w - 2 * s, y + h - s, s, s, BLACK)


def icon_gg(img: Image.Image) -> None:
    s = SCALE
    cx, cy = W // 2, H // 2
    w, h = 6 * s, 10 * s
    x = cx - w // 2
    y = cy - h // 2
    fill_rect(img, x, y + s, w, h - 2 * s, FG)
    fill_rect(img, x + s, y, w - 2 * s, h, FG)
    sw, sh = 4 * s, 3 * s
    fill_rect(img, x + (w - sw) // 2, y + 2 * s, sw, sh, GG_DARK)
    gy = y + h - 2 * s
    fill_rect(img, x + 2 * s, gy, s, s, BLACK)
    fill_rect(img, x + 3 * s, gy, s, s, BLACK)


def icon_md(img: Image.Image) -> None:
    s = SCALE
    cx, cy = W // 2, H // 2
    w, h = 10 * s, 8 * s
    x = cx - w // 2
    y = cy - h // 2
    fill_rect(img, x, y, w, h, FG)
    fill_rect(img, x + s, y + h - s, w - 2 * s, s, BLACK)
    fill_rect(img, x + 2 * s, y + s,     4 * s, s, BLACK)
    fill_rect(img, x + 2 * s, y + 3 * s, 4 * s, s, BLACK)
    fill_rect(img, x + 2 * s, y + 5 * s, 4 * s, s, BLACK)


def icon_pce(img: Image.Image) -> None:
    s = SCALE
    cx, cy = W // 2, H // 2
    w, h = 6 * s, 10 * s
    x = cx - w // 2
    y = cy - h // 2
    fill_rect(img, x, y, w, h, FG)
    fill_rect(img, x + s, y + h - s, w - 2 * s, s, BLACK)
    fill_rect(img, x + s, y + 2 * s, w - 2 * s, s, BLACK)


def icon_star(img: Image.Image) -> None:
    s = SCALE
    cx, cy = W // 2, H // 2
    pat = [
        [0, 0, 0, 0, 1, 0, 0, 0, 0],
        [0, 0, 0, 0, 1, 0, 0, 0, 0],
        [0, 0, 0, 1, 1, 1, 0, 0, 0],
        [1, 1, 1, 1, 1, 1, 1, 1, 1],
        [0, 1, 1, 1, 1, 1, 1, 1, 0],
        [0, 0, 1, 1, 1, 1, 1, 0, 0],
        [0, 0, 1, 1, 0, 1, 1, 0, 0],
        [0, 1, 1, 0, 0, 0, 1, 1, 0],
        [0, 1, 0, 0, 0, 0, 0, 1, 0],
    ]
    x0 = cx - (9 * s) // 2
    y0 = cy - (9 * s) // 2
    for j, row in enumerate(pat):
        for i, on in enumerate(row):
            if on:
                fill_rect(img, x0 + i * s, y0 + j * s, s, s, FG)


ICONS = [
    ("nes",  icon_nes),
    ("sms",  icon_sms),
    ("gb",   icon_gb),
    ("gg",   icon_gg),
    ("md",   icon_md),
    ("pce",  icon_pce),
    ("star", icon_star),
]


def main() -> None:
    out_dir = os.path.join(os.path.dirname(__file__), "tab_icons")
    os.makedirs(out_dir, exist_ok=True)

    sheet = Image.new("RGB", (W * len(ICONS), H), BG)
    for idx, (name, draw) in enumerate(ICONS):
        img = new_canvas()
        draw(img)
        img.save(os.path.join(out_dir, f"{name}.png"))
        big = img.resize((W * UPSCALE, H * UPSCALE), Image.NEAREST)
        big.save(os.path.join(out_dir, f"{name}_{UPSCALE}x.png"))
        sheet.paste(img, (idx * W, 0))

    sheet.save(os.path.join(out_dir, "_sheet.png"))
    sheet.resize((W * len(ICONS) * UPSCALE, H * UPSCALE), Image.NEAREST).save(
        os.path.join(out_dir, f"_sheet_{UPSCALE}x.png")
    )

    print(f"wrote {len(ICONS)} icons + sheet to {out_dir}")


if __name__ == "__main__":
    main()
