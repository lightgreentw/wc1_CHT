#!/usr/bin/env python3
"""
wcfont.py — Wing Commander Kilrathi Saga font extractor
Python port of Bekenn/wcdx dist/wc2font/src/main.cpp,
extended to also handle Wing Commander 1 font resources.

Reads a raw font resource (extracted from FONTS.FNT via wcres.py) and
exports glyphs as PNG images or a combined font-strip PNG.

Usage:
  wcfont.py -o <output_path> -extract-glyph <index>         <font_resource>
  wcfont.py -o <output_dir>  -extract-all-glyphs [-prefix P] <font_resource>
  wcfont.py -o <output_path> -extract-font-strip             <font_resource>

The tool auto-detects whether the resource came from WC1 or WC2:
  • WC2 format: uint16 height, uint8[256] widths, 2×uint16[256] file_positions
  • WC1 format: different header — detected automatically by heuristic

Requires: Pillow  (pip install Pillow)
"""

import sys
import os
import struct
import argparse
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, List

try:
    from PIL import Image
except ImportError:
    sys.exit("Error: Pillow is required.  Install it with:  pip install Pillow")


# ---------------------------------------------------------------------------
# WC2 built-in palette (256-colour, 6-bit VGA → 8-bit RGB)
# Extracted from the palette resource embedded in the original wc2font binary.
# This is the standard Kilrathi Saga UI palette used for both WC1 and WC2 fonts.
# ---------------------------------------------------------------------------

# fmt: off
_WC2_RAW_PALETTE_6BIT = [
    # R   G   B  (6-bit VGA values, 0-63)
     0,  0,  0,   0,  0, 42,   0, 42,  0,   0, 42, 42,
    42,  0,  0,  42,  0, 42,  42, 21,  0,  42, 42, 42,
    21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,
    63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63,
     0,  0,  0,   5,  5,  5,   8,  8,  8,  11, 11, 11,
    14, 14, 14,  17, 17, 17,  20, 20, 20,  24, 24, 24,
    28, 28, 28,  32, 32, 32,  36, 36, 36,  40, 40, 40,
    45, 45, 45,  50, 50, 50,  56, 56, 56,  63, 63, 63,
     0,  0, 63,  16,  0, 63,  31,  0, 63,  47,  0, 63,
    63,  0, 63,  63,  0, 47,  63,  0, 31,  63,  0, 16,
    63,  0,  0,  63, 16,  0,  63, 31,  0,  63, 47,  0,
    63, 63,  0,  47, 63,  0,  31, 63,  0,  16, 63,  0,
     0, 63,  0,   0, 63, 16,   0, 63, 31,   0, 63, 47,
     0, 63, 63,   0, 47, 63,   0, 31, 63,   0, 16, 63,
    31, 31, 63,  39, 31, 63,  47, 31, 63,  55, 31, 63,
    63, 31, 63,  63, 31, 55,  63, 31, 47,  63, 31, 39,
    63, 31, 31,  63, 39, 31,  63, 47, 31,  63, 55, 31,
    63, 63, 31,  55, 63, 31,  47, 63, 31,  39, 63, 31,
    31, 63, 31,  31, 63, 39,  31, 63, 47,  31, 63, 55,
    31, 63, 63,  31, 55, 63,  31, 47, 63,  31, 39, 63,
    45, 45, 63,  49, 45, 63,  54, 45, 63,  58, 45, 63,
    63, 45, 63,  63, 45, 58,  63, 45, 54,  63, 45, 49,
    63, 45, 45,  63, 49, 45,  63, 54, 45,  63, 58, 45,
    63, 63, 45,  58, 63, 45,  54, 63, 45,  49, 63, 45,
    45, 63, 45,  45, 63, 49,  45, 63, 54,  45, 63, 58,
    45, 63, 63,  45, 58, 63,  45, 54, 63,  45, 49, 63,
     0,  0, 28,   7,  0, 28,  14,  0, 28,  21,  0, 28,
    28,  0, 28,  28,  0, 21,  28,  0, 14,  28,  0,  7,
    28,  0,  0,  28,  7,  0,  28, 14,  0,  28, 21,  0,
    28, 28,  0,  21, 28,  0,  14, 28,  0,   7, 28,  0,
     0, 28,  0,   0, 28,  7,   0, 28, 14,   0, 28, 21,
     0, 28, 28,   0, 21, 28,   0, 14, 28,   0,  7, 28,
    14, 14, 28,  17, 14, 28,  21, 14, 28,  24, 14, 28,
    28, 14, 28,  28, 14, 24,  28, 14, 21,  28, 14, 17,
    28, 14, 14,  28, 17, 14,  28, 21, 14,  28, 24, 14,
    28, 28, 14,  24, 28, 14,  21, 28, 14,  17, 28, 14,
    14, 28, 14,  14, 28, 17,  14, 28, 21,  14, 28, 24,
    14, 28, 28,  14, 24, 28,  14, 21, 28,  14, 17, 28,
    20, 20, 28,  22, 20, 28,  24, 20, 28,  26, 20, 28,
    28, 20, 28,  28, 20, 26,  28, 20, 24,  28, 20, 22,
    28, 20, 20,  28, 22, 20,  28, 24, 20,  28, 26, 20,
    28, 28, 20,  26, 28, 20,  24, 28, 20,  22, 28, 20,
    20, 28, 20,  20, 28, 22,  20, 28, 24,  20, 28, 26,
    20, 28, 28,  20, 26, 28,  20, 24, 28,  20, 22, 28,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
]
# fmt: on

def _build_palette_rgb() -> List[int]:
    """Convert the 6-bit VGA palette to 8-bit and return a flat [R,G,B,...] list."""
    out = []
    for v in _WC2_RAW_PALETTE_6BIT:
        # VGA 6-bit → 8-bit: multiply by 255/63 ≈ 4.048; simpler: (v << 2) | (v >> 4)
        out.append((v * 255 + 31) // 63)
    return out

PALETTE_RGB = _build_palette_rgb()   # 768 ints: [R0,G0,B0, R1,G1,B1, ...]


def palette_index_to_rgb(idx: int) -> tuple:
    base = idx * 3
    return (PALETTE_RGB[base], PALETTE_RGB[base + 1], PALETTE_RGB[base + 2])


# ---------------------------------------------------------------------------
# WC1 font format helpers
# ---------------------------------------------------------------------------

def _is_wc1_font(data: bytes) -> bool:
    """
    Heuristic to distinguish WC1 from WC2 font resources.

    WC2 format starts with:
      uint16  font_height    (typically 8..16)
      uint16  color_index    (0..255)
      uint8[256] widths

    WC1 font resources are simpler: they begin directly with a 1-byte height
    followed immediately by 256 one-byte width entries, then pixel data.
    The key distinguishing feature: in the WC2 header the second uint16
    (color_index) should be in range 0–255, but the third byte interpreted as
    a font_height uint16 would be absurdly large.  We test by reading both
    interpretations and choosing the plausible one.

    More reliable: total file size.
    WC2 layout:  2 + 2 + 256 + 256 + 256 + pixel_data  (header ≥ 772 bytes)
    WC1 layout:  1 + 256 + 256 + pixel_data              (header ≥ 513 bytes)
    """
    if len(data) < 4:
        return False

    # Try WC2 header
    font_height_wc2 = struct.unpack_from("<H", data, 0)[0]
    color_index_wc2 = struct.unpack_from("<H", data, 2)[0]

    # A legitimate WC2 font height is 6–16; color index 0–255
    wc2_plausible = (1 <= font_height_wc2 <= 24) and (color_index_wc2 <= 255)

    # Try WC1 header: first byte is height
    font_height_wc1 = data[0]
    wc1_plausible = 1 <= font_height_wc1 <= 24

    # WC2 header has two uint16s (4 bytes) before widths; WC1 has one uint8 (1 byte).
    # If WC2 interpretation is plausible, prefer it; otherwise fall back to WC1.
    # Extra check: WC2 minimum size
    min_wc2_header = 2 + 2 + 256 + 256 + 256   # = 772
    if wc2_plausible and len(data) >= min_wc2_header:
        return False   # looks like WC2
    if wc1_plausible:
        return True
    return False       # assume WC2 if all else fails


# ---------------------------------------------------------------------------
# Glyph data structures
# ---------------------------------------------------------------------------

@dataclass
class GlyphInfo:
    index: int
    width: int
    height: int
    pixels: Optional[bytes] = None   # raw palette-indexed pixel data, row-major


# ---------------------------------------------------------------------------
# WC2 font reader  (matches read_font() in the original C++ wc2font)
# ---------------------------------------------------------------------------

def read_wc2_font(data: bytes) -> List[GlyphInfo]:
    """
    Parse a WC2 font resource (extracted raw bytes from fonts.fnt via wcres).

    Binary layout:
      uint16  font_height
      uint16  color_index   (ignored; duplicative of pixel data)
      uint8[256] widths     (one per ASCII slot 0–255)
      uint8[256] file_pos_lo
      uint8[256] file_pos_hi
      <pixel data>
    """
    offset = 0
    font_height = struct.unpack_from("<H", data, offset)[0]; offset += 2
    _color_index = struct.unpack_from("<H", data, offset)[0]; offset += 2   # noqa: F841

    widths = list(data[offset : offset + 256]); offset += 256

    pos_lo = list(data[offset : offset + 256]); offset += 256
    pos_hi = list(data[offset : offset + 256]); offset += 256

    glyphs = []
    for i in range(256):
        w = widths[i]
        h = font_height
        file_pos = pos_lo[i] | (pos_hi[i] << 8)

        g = GlyphInfo(index=i, width=w, height=h)
        pixel_count = w * h
        if pixel_count > 0:
            g.pixels = data[file_pos : file_pos + pixel_count]
        glyphs.append(g)

    return glyphs


# ---------------------------------------------------------------------------
# WC1 font reader
# ---------------------------------------------------------------------------

def read_wc1_font(data: bytes) -> List[GlyphInfo]:
    """
    Parse a WC1 font resource.

    The WC1 font format is simpler than WC2:
      uint8        font_height
      uint8[256]   widths
      uint8[256]   file_pos_lo
      uint8[256]   file_pos_hi
      <pixel data>

    (No color_index field; 1-byte height instead of 2-byte.)
    Pixel data starts immediately after the directory at byte 1+256+512 = 769.
    """
    offset = 0
    font_height = data[offset]; offset += 1

    widths = list(data[offset : offset + 256]); offset += 256

    pos_lo = list(data[offset : offset + 256]); offset += 256
    pos_hi = list(data[offset : offset + 256]); offset += 256
    # offset is now 769 = start of pixel area

    glyphs = []
    for i in range(256):
        w = widths[i]
        h = font_height
        file_pos = pos_lo[i] | (pos_hi[i] << 8)

        g = GlyphInfo(index=i, width=w, height=h)
        pixel_count = w * h
        if pixel_count > 0:
            if file_pos + pixel_count > len(data):
                # Gracefully handle truncated data
                g.pixels = None
            else:
                g.pixels = data[file_pos : file_pos + pixel_count]
        glyphs.append(g)

    return glyphs


def read_font(data: bytes, force: Optional[str] = None) -> List[GlyphInfo]:
    """
    Auto-detect font format and return list of GlyphInfo.
    force: 'wc1' or 'wc2' to override auto-detection.
    """
    if force == "wc1":
        is_wc1 = True
    elif force == "wc2":
        is_wc1 = False
    else:
        is_wc1 = _is_wc1_font(data)

    if is_wc1:
        print("Detected WC1 font format")
        return read_wc1_font(data)
    else:
        print("Detected WC2 font format")
        return read_wc2_font(data)


# ---------------------------------------------------------------------------
# PNG output helpers
# ---------------------------------------------------------------------------

def _pixels_to_rgba(pixels: bytes, width: int, height: int) -> Image.Image:
    """
    Convert palette-indexed pixel data to an RGBA PIL image.
    Index 0 is treated as transparent (background).
    """
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    px = img.load()
    for row in range(height):
        for col in range(width):
            idx = pixels[row * width + col]
            if idx == 0:
                px[col, row] = (0, 0, 0, 0)      # transparent
            else:
                r, g, b = palette_index_to_rgb(idx)
                px[col, row] = (r, g, b, 255)
    return img


def save_glyph_png(glyph: GlyphInfo, path: str) -> None:
    if glyph.pixels is None or glyph.width == 0 or glyph.height == 0:
        raise ValueError(f"Glyph {glyph.index} has no pixel data")
    img = _pixels_to_rgba(glyph.pixels, glyph.width, glyph.height)
    img.save(path, "PNG")


def save_font_strip_png(glyphs: List[GlyphInfo], path: str) -> None:
    """Concatenate all glyphs horizontally into one strip image."""
    total_width = sum(g.width for g in glyphs)
    height = glyphs[0].height if glyphs else 1

    strip = Image.new("RGBA", (total_width, height), (0, 0, 0, 0))
    x = 0
    for g in glyphs:
        if g.pixels and g.width > 0:
            glyph_img = _pixels_to_rgba(g.pixels, g.width, g.height)
            strip.paste(glyph_img, (x, 0))
        x += g.width

    strip.save(path, "PNG")


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def cmd_extract_glyph(glyphs: List[GlyphInfo], index: int, output: str) -> None:
    if index >= len(glyphs):
        raise IndexError(f"Glyph index {index} out of range (font has {len(glyphs)} glyphs)")
    g = glyphs[index]
    if not g.pixels or g.width == 0:
        raise ValueError(f"Glyph {index} is empty (zero-sized)")
    save_glyph_png(g, output)
    print(f"Extracted glyph {index} ('{chr(index) if 32 <= index < 127 else '?'}') "
          f"{g.width}×{g.height} → {output}")


def cmd_extract_all_glyphs(
    glyphs: List[GlyphInfo], output_dir: str, prefix: str = ""
) -> None:
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for g in glyphs:
        if not g.pixels or g.width == 0 or g.height == 0:
            continue
        filename = f"{prefix}{g.index}.png"
        path = out / filename
        save_glyph_png(g, str(path))
        label = chr(g.index) if 32 <= g.index < 127 else f"0x{g.index:02X}"
        print(f"  [{g.index:3d}] {label!r:5s} {g.width:2d}×{g.height:2d} → {filename}")
        count += 1
    print(f"Done. {count} glyph(s) written to {out}")


def cmd_extract_font_strip(glyphs: List[GlyphInfo], output: str) -> None:
    save_font_strip_png(glyphs, output)
    total_w = sum(g.width for g in glyphs)
    h = glyphs[0].height if glyphs else 0
    print(f"Font strip {total_w}×{h} → {output}")


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="wcfont.py",
        description=(
            "Wing Commander Kilrathi Saga font extractor\n"
            "Supports both WC1 and WC2 font resource files (auto-detected)."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Workflow:
  # Step 1: extract raw font resource from the archive
  python wcres.py -o fonts_dir -extract-all GAMEDAT/FONTS.FNT

  # Step 2: convert a specific resource to PNG glyphs
  python wcfont.py -o glyphs/ -extract-all-glyphs fonts_dir/0

  # Or get a single combined strip
  python wcfont.py -o font_strip.png -extract-font-strip fonts_dir/0

  # Force WC1 or WC2 format parsing
  python wcfont.py --format wc1 -o glyphs/ -extract-all-glyphs myfont

Examples using glyph index (ASCII value):
  python wcfont.py -o A.png -extract-glyph 65 fonts_dir/0    # letter 'A'
  python wcfont.py -o space.png -extract-glyph 32 fonts_dir/0
""",
    )

    p.add_argument("-o", dest="output", metavar="OUTPUT_PATH", required=True,
                   help="Output PNG file or directory")
    p.add_argument("--format", dest="fmt", choices=["wc1", "wc2", "auto"],
                   default="auto",
                   help="Force font format (default: auto-detect)")
    p.add_argument("-prefix", dest="prefix", default="",
                   metavar="PREFIX",
                   help="Filename prefix for -extract-all-glyphs")

    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("-extract-glyph", dest="glyph_index", type=int, metavar="INDEX",
                      help="Extract a single glyph by ASCII index (0–255)")
    mode.add_argument("-extract-all-glyphs", dest="extract_all", action="store_true",
                      help="Extract all non-empty glyphs as individual PNG files")
    mode.add_argument("-extract-font-strip", dest="font_strip", action="store_true",
                      help="Extract all glyphs concatenated into one horizontal PNG strip")

    p.add_argument("input", metavar="FONT_RESOURCE",
                   help="Raw font resource file (output of wcres.py)")

    return p


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)

    data = Path(args.input).read_bytes()
    force = None if args.fmt == "auto" else args.fmt
    glyphs = read_font(data, force)

    if args.glyph_index is not None:
        cmd_extract_glyph(glyphs, args.glyph_index, args.output)
    elif args.extract_all:
        cmd_extract_all_glyphs(glyphs, args.output, prefix=args.prefix)
    elif args.font_strip:
        cmd_extract_font_strip(glyphs, args.output)


if __name__ == "__main__":
    try:
        main()
    except (IndexError, ValueError, FileNotFoundError, struct.error) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
