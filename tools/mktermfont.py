#!/usr/bin/env python3
"""Bakes the terminal font into src/gfx/termfont.bin (linked into the kernel
with objcopy): DejaVu Sans Mono, regular + bold, in 4 cell sizes
(8x16, 10x20, 12x24, 16x32), anti-aliased alpha per cell, indexed by
Unicode code point. Box drawing, block elements, braille and powerline
glyphs are not baked - the kernel draws those itself, crisp at any size.

  toolchain/pyenv/bin/python tools/mktermfont.py     (needs pillow + fonttools)

Layout (little endian):
  "SMTF" u32 nsizes u32 nreg u32 nbold
  u32 cps_reg[nreg]  u32 cps_bold[nbold]            sorted code points
  per size: u8 cw, ch, baseline, 0; u32 off_reg, off_bold
  alpha bytes: glyph i of a style/size at off + i*cw*ch
"""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "gfx", "termfont.bin")
REG = os.environ.get("MONO", "/usr/share/fonts/TTF/DejaVuSansMono.ttf")
BOLD = os.environ.get("MONO_BOLD", "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf")

SIZES = [(8, 16), (10, 20), (12, 24), (16, 32)]
RANGES = [(0x20, 0x7E), (0xA0, 0xFF), (0x100, 0x17F), (0x370, 0x3FF), (0x400, 0x4FF),
          (0x2010, 0x205E), (0x20A0, 0x20BF), (0x2100, 0x214F), (0x2190, 0x21FF),
          (0x2200, 0x22FF), (0x2300, 0x23FF), (0x25A0, 0x25FF), (0x2600, 0x26FF),
          (0x2700, 0x27BF), (0x27C0, 0x27FF), (0xFFFD, 0xFFFD)]
BOLD_RANGES = [(0x20, 0x7E), (0xA0, 0xFF), (0x100, 0x17F), (0x370, 0x3FF), (0x400, 0x4FF),
               (0x2010, 0x205E)]


def cps_of(path, ranges):
    cmap = TTFont(path).getBestCmap()
    return [c for a, b in ranges for c in range(a, b + 1) if c in cmap]


def bake(path, cps, cw, ch):
    px = int(cw / 0.6021)                  # DejaVu Sans Mono advance = 0.602 em
    base = round(ch * 0.75)
    f = ImageFont.truetype(path, px)
    out = bytearray()
    for c in cps:
        img = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(img).text((0, base), chr(c), font=f, fill=255, anchor="ls")
        out += img.tobytes()
    return base, out


def main():
    reg, bold = cps_of(REG, RANGES), cps_of(BOLD, BOLD_RANGES)
    head = struct.pack("<4sIII", b"SMTF", len(SIZES), len(reg), len(bold))
    head += struct.pack(f"<{len(reg)}I", *reg) + struct.pack(f"<{len(bold)}I", *bold)
    table_at = len(head)
    head += bytes(12 * len(SIZES))
    blobs, pos, table = [], len(head), b""
    for cw, ch in SIZES:
        base, r = bake(REG, reg, cw, ch)
        _, b = bake(BOLD, bold, cw, ch)
        table += struct.pack("<BBBBII", cw, ch, base, 0, pos, pos + len(r))
        blobs += [r, b]
        pos += len(r) + len(b)
        print(f"{cw}x{ch}: {len(r) + len(b)} bytes", file=sys.stderr)
    data = bytearray(head)
    data[table_at:table_at + len(table)] = table
    for b in blobs:
        data += b
    open(OUT, "wb").write(data)
    print(f"wrote {OUT}: {len(reg)} glyphs (+{len(bold)} bold), {len(data) >> 10} KiB", file=sys.stderr)


if __name__ == "__main__":
    main()
