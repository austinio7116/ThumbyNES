#!/usr/bin/env python3
"""Render the picker tab strip in several background-colour themes.

Composites the hand-painted 12x8 icons in tools/tab_icons/ into a
real-shape tab strip (128x11, 7 cells of width 18, NES set as the
active cell) under each theme, then stacks all themes vertically
with a labelled header. The output is meant to be opened side-by-
side with the device for picking a direction.

Each theme defines:
    inactive_bg   : RGB triple — fills inactive cells
    active_bg     : RGB triple — fills the active cell
    underline     : RGB triple — 1-px highlight under the active cell

The icon tint stays orange (COL_TITLE 0xFD20) on the active cell and
COL_DIM grey on inactive cells (with the SMS/MD darker override),
matching the device's runtime tint logic. Literal accent pixels in
the icons render as-is regardless of theme.

Run any time you tweak the theme list. Writes
tools/tab_icons/_themes.png (native) and _themes_8x.png (upscaled).
"""
from __future__ import annotations

import os

from PIL import Image, ImageDraw, ImageFont

# Geometry mirrors nes_picker.c::draw_tab_bar.
FB_W            = 128
TAB_BAR_H       = 11        # 10 px of cell + 1 px separator below
N_TABS          = 7
CELL_W          = FB_W // N_TABS    # = 18 — matches the 7-tab worst case
ICON_W, ICON_H  = 12, 8

ICONS = ["star", "nes", "sms", "gg", "gb", "md", "pce"]   # FAV first, then ROM_SYS_* order

# Pixel-colour conventions in the source PNGs (see render_tab_icons.py).
TRANSP_KEY = (255, 0, 255)
TINT_KEY   = (255, 166, 0)

# Active-tab tint = COL_TITLE 0xFD20 → ~(255, 166, 0). Inactive default
# = COL_DIM 0x8410 → ~(131, 130, 131). SMS/MD inactive uses our darker
# override 0x3186 → ~(49, 49, 49).
TINT_ACTIVE       = (255, 166, 0)
TINT_INACTIVE     = (132, 130, 132)
TINT_INACTIVE_DIM = (49, 49, 49)
DARKER_DIM_ICONS  = {"sms", "md"}

ACTIVE_INDEX = 1   # which tab is "active" in the mockups — NES (slot 1 after FAV)

# name → (inactive_bg, active_bg, underline, separator).
#
# Separator is a saturated mid-tone of the theme's hue family —
# bright enough to read as a clear divider line under the strip,
# but staying in the same colour family as the cells so it doesn't
# look like a random accent stripe. Underline keeps the warm
# yellow/orange accent (the "active tab" affordance is universal).
THEMES = [
    # name,            inactive_bg,    active_bg,      underline,        separator
    ("current (grey)", (16, 22, 22),   (60, 60, 60),   (255, 166,   0), (140, 140, 140)),
    ("forest",         ( 8, 32, 16),   (24, 64, 32),   (255, 166,   0), ( 56, 156,  72)),
    ("pine",           (20, 50, 28),   (44, 96, 56),   (255, 200,  60), ( 90, 200, 110)),
    ("navy",           ( 8, 16, 48),   (24, 44, 88),   (255, 166,   0), ( 60, 110, 200)),
    ("deep teal",      ( 8, 40, 40),   (24, 76, 76),   (255, 200,  60), ( 50, 170, 170)),
    ("aubergine",      (32, 12, 36),   (64, 28, 72),   (255, 200,  60), (170,  72, 190)),
    ("sepia",          (36, 24, 12),   (76, 52, 24),   (255, 220, 100), (210, 150,  80)),
    ("dark slate-blue",(16, 20, 36),   (40, 48, 80),   (255, 166,   0), (110, 130, 200)),
]


def load_icon(name: str) -> Image.Image:
    return Image.open(
        os.path.join(os.path.dirname(__file__), "tab_icons", f"{name}.png")
    ).convert("RGB")


def render_strip(theme: tuple) -> Image.Image:
    """Render one 128x12 strip (11 strip + 1 px below for separator)."""
    name, inactive_bg, active_bg, underline, separator = theme
    img = Image.new("RGB", (FB_W, TAB_BAR_H + 1), inactive_bg)
    px = img.load()

    # Cell backgrounds.
    for i in range(N_TABS):
        x0 = i * CELL_W
        bg = active_bg if i == ACTIVE_INDEX else inactive_bg
        for yy in range(TAB_BAR_H - 1):           # rows 0..9
            for xx in range(CELL_W):
                px[x0 + xx, yy] = bg
        if i == ACTIVE_INDEX:
            # 1-px underline at row TAB_BAR_H - 2 = 9.
            for xx in range(CELL_W):
                px[x0 + xx, TAB_BAR_H - 2] = underline

    # Bottom separator — half-brightness of the inactive bg so it
    # reads as a deliberate line in the theme's hue family rather
    # than as a stray grey.
    for xx in range(FB_W):
        px[xx, TAB_BAR_H] = separator

    # Icons.
    for i, icon_name in enumerate(ICONS):
        active = (i == ACTIVE_INDEX)
        if active:
            tint = TINT_ACTIVE
        elif icon_name in DARKER_DIM_ICONS:
            tint = TINT_INACTIVE_DIM
        else:
            tint = TINT_INACTIVE
        bg = active_bg if active else inactive_bg
        icon = load_icon(icon_name).load()
        x0 = i * CELL_W + 2     # device draws at (cell_x + 2, 1)
        y0 = 1
        for yy in range(ICON_H):
            for xx in range(ICON_W):
                src = icon[xx, yy]
                if src == TRANSP_KEY:
                    out = bg
                elif src == TINT_KEY:
                    out = tint
                else:
                    out = src
                px[x0 + xx, y0 + yy] = out

    return img


def rgb565(rgb: tuple) -> int:
    r, g, b = rgb
    return ((r * 31 + 127) // 255) << 11 \
         | ((g * 63 + 127) // 255) << 5 \
         | ((b * 31 + 127) // 255)


def emit_c_themes() -> None:
    """Emit the C tab_theme_t initialiser block for nes_picker.c.
    Run this whenever THEMES is edited; copy the printed lines into
    the TAB_THEMES[] array. forest is index 0 (the default)."""
    print("/* Auto-generated from tools/render_tab_themes.py — keep in sync. */")
    print("static const tab_theme_t TAB_THEMES[] = {")
    for name, ib, ab, ul, sep in THEMES:
        if name == "current (grey)":
            continue   # not exposed in the picker theme list
        print(f"    {{ /* {name:<14} */ "
              f"0x{rgb565(ib):04x}, 0x{rgb565(ab):04x}, "
              f"0x{rgb565(ul):04x}, 0x{rgb565(sep):04x}, "
              f"\"{name}\" }},")
    print("};")


def main() -> None:
    UPSCALE = 8
    LABEL_H = 14
    PAD     = 2

    # Total: (label + strip + pad) per theme.
    row_h = LABEL_H + (TAB_BAR_H + 1) + PAD
    sheet_w = FB_W
    sheet_h = row_h * len(THEMES) + PAD

    sheet = Image.new("RGB", (sheet_w, sheet_h), (16, 16, 16))
    draw  = ImageDraw.Draw(sheet)

    # System font for labels (small, but at upscale=8 it's readable).
    try:
        font = ImageFont.load_default()
    except Exception:
        font = None

    for ti, theme in enumerate(THEMES):
        name = theme[0]
        y_label = PAD + ti * row_h
        y_strip = y_label + LABEL_H
        if font:
            draw.text((2, y_label + 2), name, fill=(220, 220, 220), font=font)
        else:
            draw.text((2, y_label + 2), name, fill=(220, 220, 220))
        strip = render_strip(theme)
        sheet.paste(strip, (0, y_strip))

    out_dir = os.path.join(os.path.dirname(__file__), "tab_icons")
    os.makedirs(out_dir, exist_ok=True)
    sheet.save(os.path.join(out_dir, "_themes.png"))
    sheet.resize(
        (sheet_w * UPSCALE, sheet_h * UPSCALE), Image.NEAREST
    ).save(os.path.join(out_dir, f"_themes_{UPSCALE}x.png"))

    print(f"wrote {len(THEMES)} themes to {out_dir}/_themes*.png "
          f"(NES tab active in each row)")
    print()
    emit_c_themes()


if __name__ == "__main__":
    main()
