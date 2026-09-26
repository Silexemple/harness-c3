#!/usr/bin/env python3
"""Compose the README mockups from the UI simulator's PPM frames.

Pixel fidelity rule: every screen pixel comes from the real ui.c rendered by
LVGL (see build.sh) — this script only cuts the round panel shape, mounts it
in a bezel with a soft shadow, and arranges the boards. Text and frame are
presentation, the screens are not redrawn.

Usage:
    python3 compose_mockups.py /tmp/frames ../../docs/assets

Outputs:
    <out>/screens/<nn>_<name>.png   round 240x240 screen, transparent outside
    <out>/mockup-hero.png           boot + offline + home×2 + question
    <out>/mockup-flows.png          multi-select + toast + empty tab

Requires Pillow. A Montserrat TTF path can be given via MONTSERRAT_DIR
(default /tmp/fonts, falls back to DejaVu Sans).
"""

import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4  # supersampling factor for smooth circles/text

BG_TOP = (0x1b, 0x17, 0x12)      # warm dark, low saturation
BG_BOT = (0x13, 0x10, 0x0c)
INK = (0xe9, 0xe3, 0xd8)         # warm white
DIM = (0xa4, 0x9b, 0x8a)         # warm dim
BEZEL = (0x24, 0x27, 0x2d)
BEZEL_EDGE = (0x3c, 0x41, 0x49)
BEZEL_INNER = (0x14, 0x16, 0x1a)

DEVICE_R = 132   # bezel outer radius (screen is r=120)
GAP = 56
MARGIN = 90


def load_font(name, size):
    dirs = [os.environ.get("MONTSERRAT_DIR", "/tmp/fonts")]
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size * S)
    import matplotlib
    p = os.path.join(os.path.dirname(matplotlib.__file__),
                     "mpl-data/fonts/ttf/DejaVuSans.ttf")
    return ImageFont.truetype(p, size * S)


def round_screen(ppm_path):
    """240x240 render -> round screen tile with bezel, at Sx supersampling."""
    img = Image.open(ppm_path).convert("RGB")
    img = img.resize((240 * S, 240 * S), Image.NEAREST)
    r = DEVICE_R * S
    tile = Image.new("RGBA", (r * 2 + 16 * S, r * 2 + 16 * S), (0, 0, 0, 0))
    cx = cy = tile.width // 2

    # soft shadow on its own blurred layer
    sh = Image.new("RGBA", tile.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse(
        [cx - r + 4 * S, cy - r + 14 * S, cx + r - 4 * S, cy + r + 18 * S],
        fill=(0, 0, 0, 120))
    sh = sh.filter(ImageFilter.GaussianBlur(9 * S))
    tile.alpha_composite(sh)

    d = ImageDraw.Draw(tile)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=BEZEL)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=BEZEL_EDGE, width=2 * S)
    d.ellipse([cx - 122 * S, cy - 122 * S, cx + 122 * S, cy + 122 * S],
              outline=BEZEL_INNER, width=2 * S)

    # round mask for the screen content (mask must match the pasted image)
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse([0, 0, img.width - 1, img.height - 1], fill=255)
    tile.paste(img, (cx - 120 * S, cy - 120 * S), mask)
    return tile


def board(frames, captions, title, subtitle, out_path):
    n = len(frames)
    title_h = 150 if title else 60
    w = MARGIN * 2 + n * DEVICE_R * 2 + (n - 1) * GAP
    h = title_h + DEVICE_R * 2 + 96
    cv = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))

    # warm vertical gradient background
    grad = Image.new("RGB", (1, h * S))
    for y in range(h * S):
        t = y / (h * S - 1)
        grad.putpixel((0, y), tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t)
                                    for i in range(3)))
    cv.paste(grad.resize((w * S, h * S)), (0, 0))
    cv = cv.convert("RGBA")

    d = ImageDraw.Draw(cv)
    if title:
        f_t = load_font("Montserrat-SemiBold.ttf", 40)
        f_s = load_font("Montserrat-Regular.ttf", 21)
        tw = d.textlength(title, font=f_t)
        d.text(((w * S - tw) / 2, 46 * S), title, font=f_t, fill=INK)
        sw = d.textlength(subtitle, font=f_s)
        d.text(((w * S - sw) / 2, 104 * S), subtitle, font=f_s, fill=DIM)

    f_c = load_font("Montserrat-Medium.ttf", 18)
    y_dev = title_h * S
    for i, (fr, cap) in enumerate(zip(frames, captions)):
        x = (MARGIN + i * (DEVICE_R * 2 + GAP)) * S
        tile = round_screen(fr)
        cv.alpha_composite(tile, (x - 8 * S, y_dev - 8 * S))
        cw = d.textlength(cap, font=f_c)
        cx = x + DEVICE_R * S
        d.text((cx - cw / 2, y_dev + (DEVICE_R * 2 + 26) * S), cap,
               font=f_c, fill=DIM)

    cv = cv.resize((w, h), Image.LANCZOS)
    cv.convert("RGB").save(out_path, quality=95)
    print("wrote", out_path)


def main():
    frames_dir, out_dir = sys.argv[1], sys.argv[2]
    scr_dir = os.path.join(out_dir, "screens")
    os.makedirs(scr_dir, exist_ok=True)

    names = ["01_boot", "02_offline", "03_home_running", "04_home_waiting",
             "05_question_single", "06_question_multi", "07_home_done_toast",
             "08_home_empty"]
    paths = {}
    for n in names:
        p = os.path.join(frames_dir, n + ".ppm")
        paths[n] = p
        # individual round screen, transparent background (native 240 px)
        img = Image.open(p).convert("RGB")
        mask = Image.new("L", img.size, 0)
        ImageDraw.Draw(mask).ellipse([0, 0, 239, 239], fill=255)
        out = Image.new("RGBA", img.size, (0, 0, 0, 0))
        out.paste(img, (0, 0), mask)
        out.save(os.path.join(scr_dir, n + ".png"))

    board([paths["01_boot"], paths["02_offline"], paths["03_home_running"],
           paths["04_home_waiting"], paths["05_question_single"]],
          ["Démarrage", "Hors connexion", "Agent actif",
           "En attente", "Question"],
          "Harness C3",
          "ESP32-C3-MINI-1U · GC9A01 240×240 · rendus LVGL v9 réels",
          os.path.join(out_dir, "mockup-hero.png"))

    board([paths["06_question_multi"], paths["07_home_done_toast"],
           paths["08_home_empty"]],
          ["Multi-sélection", "Notification", "Onglet vide"],
          "Répondre depuis le poignet",
          "Choix multiples · toast · état vide — sans quitter l'écran",
          os.path.join(out_dir, "mockup-flows.png"))


if __name__ == "__main__":
    main()
