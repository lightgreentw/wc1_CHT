#!/usr/bin/env python3
"""
bdf_to_czht_fnt.py -- Convert a BDF bitmap font into a CZHT-format .FNT
file compatible with CHINESE.FNT (as used by this MI1 Ultimate Talkie
Edition Chinese-display fork).

============================================================================
CZHT FILE FORMAT (reverse-engineered + confirmed against CHINESE.FNT and
the reference ttf_to_czht_fnt.html tool):

  Header (8 bytes):
    4 bytes   magic       "CZHT"
    1 byte    0x00
    1 byte    0x01        (format version)
    2 bytes   count       number of glyph records, little-endian uint16

  Then `count` glyph records, each `2 + N*bpr` bytes:
    2 bytes   code        the glyph's lookup code, little-endian uint16
                           (ASCII 0x20-0x7E, then Big5 0xA140-0xF9FE in
                           CHINESE.FNT; whatever code space you target)
    N*bpr bytes  bitmap   1bpp, row-major, MSB = leftmost pixel,
                           bpr = ceil(N/8) bytes per row, N rows.
                           (N=16 -> 32 bytes/glyph, matching CHINESE.FNT)

  Confirmed against CHINESE.FNT: header count=13800 (LE u16 at offset 6),
  glyph size 2+32=34 bytes, codes 0x20-0x7E then standard Big5 codepoints,
  first 10 records decode cleanly to "space ! " # $ % & ' ( )" with no
  seams -- i.e. this is a byte-exact match to the real file, not a guess.
============================================================================

GLYPH POSITIONING (per BDF FONT_ASCENT/FONT_DESCENT + per-glyph BBX):

  BDF's BBX line is "BBX bbw bbh bbxoff bbyoff":
    bbw, bbh      = the glyph's own ink bounding box, width x height
    bbxoff,bbyoff = offset of that box's lower-left corner from the
                    glyph origin (bbyoff can be negative -> descender)

  Within a fixed N-row cell (default 16, i.e. FONT_ASCENT+FONT_DESCENT):
    top  = FONT_ASCENT - bbyoff - bbh     (row where the glyph's own
                                            bitmap top row lands)
    left = bbxoff                         (column where the glyph's own
                                            bitmap left edge lands)

  The glyph's own bbh x bbw bitmap (as stored in the BDF, MSB-first,
  ceil(bbw/8) bytes/row) is copied into the NxN canvas at (top, left),
  clipped to the canvas bounds if it overflows.
============================================================================

SOURCE vs. TARGET CHARSET (v2)

  A BDF's ENCODING value on each glyph is just an integer -- BDF itself
  doesn't force it to mean anything in particular. Two different worlds
  of BDF exist in practice:
    - BDFs generated from a Unicode-cmapped font (most modern tools):
      ENCODING is the glyph's Unicode codepoint directly.
    - BDFs generated from (or authored directly as) a legacy Big5-family
      bitmap font: ENCODING is the raw Big5 / Big5-HKSCS / Big5-UAO /
      Big5E two-byte code.
  v1 of this tool only ever handled the first case (it silently assumed
  ENCODING == Unicode). v2 adds a SOURCE charset step that interprets
  the BDF's own ENCODING values, independent of the TARGET charset step
  that decides what codes the output .FNT is keyed by:

      BDF ENCODING --[source charset]--> Unicode --[target charset]--> output code

  --source-charset {auto,unicode,big5,cp950,big5hkscs} (default: auto)
      How to read the BDF's own ENCODING values. 'auto' inspects the
      BDF's header (CHARSET_REGISTRY/FONT XLFD name, if present) and the
      numeric shape of the ENCODING values actually used (do they look
      like Big5 lead/trail byte pairs, or like Unicode CJK codepoints?)
      and prints its reasoning. Override this if a source BDF's own
      encoding is known ahead of time, or if the auto-guess looks wrong.
  --source-mapping-file FILE
      For source fonts that use Big5-UAO, Big5E, Big5-2003, or any other
      extension not covered by a built-in Python codec -- see "mapping
      file" below. Overrides --source-charset entirely when given.

  --target-charset {unicode,big5,cp950,big5hkscs} (default: big5, i.e.
      unchanged v1 behaviour / CHINESE.FNT-compatible)
      What code space the OUTPUT .FNT file is keyed by.
  --target-mapping-file FILE (alias: --mapping-file, the v1 flag name)
      Same idea as --source-mapping-file but for the output code space.
      Overrides --target-charset entirely when given.

  MAPPING FILE FORMAT: a "CODE UNICODE" table, one pair per line, each
  in 0xXXXX hex (with or without the "0x" prefix), '#' starts a comment,
  e.g. one of the b2u.txt tables distributed by moztw.org:
        http://moztw.org/docs/big5/table/big5_2003-b2u.txt
        http://moztw.org/docs/big5/table/cp950-b2u.txt
        http://moztw.org/docs/big5/table/uao241-b2u.txt
  This is the only way to handle Big5-UAO or Big5E exactly (Python has
  no built-in codec for either) -- for BOTH reading a UAO-encoded source
  BDF and writing a UAO-keyed output .FNT, on the relevant side.
============================================================================
"""
import argparse
import struct


# ---------------------------------------------------------------------
# BDF parsing
# ---------------------------------------------------------------------

class BdfFont:
    def __init__(self):
        self.font_ascent = None
        self.font_descent = None
        self.default_size = None  # PIXEL_SIZE, informational only
        self.glyphs = {}  # RAW BDF ENCODING value -> Glyph (not necessarily Unicode!)
        # Header hints used for source-charset auto-detection, if present:
        self.xlfd_font_name = None    # the "FONT ..." line, e.g. "...-big5-0"
        self.charset_registry = None  # CHARSET_REGISTRY property, e.g. "big5" / "iso10646-1"
        self.charset_encoding = None  # CHARSET_ENCODING property

    @property
    def cell_size(self):
        if self.font_ascent is None or self.font_descent is None:
            return None
        return self.font_ascent + self.font_descent


class Glyph:
    __slots__ = ("bbw", "bbh", "bbxoff", "bbyoff", "rows")

    def __init__(self, bbw, bbh, bbxoff, bbyoff, rows):
        self.bbw = bbw
        self.bbh = bbh
        self.bbxoff = bbxoff
        self.bbyoff = bbyoff
        self.rows = rows  # list of ints, one per row, as literally stored


def parse_bdf(path):
    """Stream-parse a BDF file. Built for speed on very large files
    (tens of thousands of glyphs). font.glyphs is keyed by the RAW
    ENCODING value exactly as written in the file -- interpreting what
    that value means (Unicode? Big5?) is the job of the source Charset,
    applied later in convert()."""
    font = BdfFont()
    encoding = None
    bbx = None
    rows = []
    in_bitmap = False

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if in_bitmap:
                if line.startswith("ENDCHAR"):
                    if encoding is not None and encoding >= 0 and bbx is not None:
                        bbw, bbh, bbxoff, bbyoff = bbx
                        font.glyphs[encoding] = Glyph(bbw, bbh, bbxoff, bbyoff, rows)
                    in_bitmap = False
                    encoding = None
                    bbx = None
                    rows = []
                else:
                    rows.append(int(line, 16) if line else 0)
                continue

            if line.startswith("FONT_ASCENT "):
                font.font_ascent = int(line.split()[1])
            elif line.startswith("FONT_DESCENT "):
                font.font_descent = int(line.split()[1])
            elif line.startswith("PIXEL_SIZE "):
                font.default_size = int(line.split()[1])
            elif line.startswith("ENCODING "):
                encoding = int(line.split()[1])
            elif line.startswith("BBX "):
                parts = line.split()
                bbx = (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
            elif line.startswith("BITMAP"):
                in_bitmap = True
                rows = []
            elif line.startswith("FONT "):
                font.xlfd_font_name = line[len("FONT "):].strip()
            elif line.startswith("CHARSET_REGISTRY "):
                font.charset_registry = line.split(None, 1)[1].strip().strip('"')
            elif line.startswith("CHARSET_ENCODING "):
                font.charset_encoding = line.split(None, 1)[1].strip().strip('"')

    if font.font_ascent is None or font.font_descent is None:
        raise ValueError("BDF missing FONT_ASCENT/FONT_DESCENT global properties")
    return font


# ---------------------------------------------------------------------
# Glyph compositing: BDF glyph -> fixed NxN canvas bitmap
# ---------------------------------------------------------------------

def composite_glyph(glyph, ascent, n, dx=0, dy=0):
    """Place a BDF glyph into an NxN 1bpp canvas per the ascent/BBX
    formula. Returns a list of N ints, each an N-bit row value (bit
    N-1 = leftmost pixel), clipped to the canvas."""
    canvas = [0] * n
    if glyph is None:
        return canvas

    top = ascent - glyph.bbyoff - glyph.bbh + dy
    left = glyph.bbxoff + dx
    bpr = (glyph.bbw + 7) // 8
    row_bits = bpr * 8

    for i, rowval in enumerate(glyph.rows):
        r = top + i
        if r < 0 or r >= n:
            continue
        canvas_row = 0
        for col in range(glyph.bbw):
            bit = (rowval >> (row_bits - 1 - col)) & 1
            if not bit:
                continue
            cc = left + col
            if 0 <= cc < n:
                canvas_row |= 1 << (n - 1 - cc)
        canvas[r] |= canvas_row

    return canvas


def pack_canvas(canvas, n):
    bpr = (n + 7) // 8
    out = bytearray()
    for rowval in canvas:
        out += rowval.to_bytes(bpr, "big")
    return bytes(out)


# ---------------------------------------------------------------------
# Charset: wraps a built-in codec, an external mapping table, or plain
# Unicode-identity, behind one "code <-> Unicode" interface. Used for
# BOTH the source interpretation and the target code space (they are
# independent Charset instances, possibly of different kinds).
# ---------------------------------------------------------------------

def load_mapping_file(path):
    """Parse a moztw.org-style 'CODE UNICODE' mapping file into a dict
    {code: unicode_codepoint}. Used for either the source side (reading
    a Big5-UAO/Big5E-encoded BDF) or the target side (emitting a
    Big5-UAO/Big5E/Big5-2003-keyed .FNT)."""
    mapping = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                code = int(parts[0], 16)
                uni = int(parts[1], 16)
            except ValueError:
                continue
            mapping[code] = uni
    return mapping


class Charset:
    """Wraps a built-in Python codec, a loaded external mapping table,
    or plain Unicode-identity, behind one interface: code_to_unicode()
    + code_list(). codec_name == "unicode" means "the code IS the
    Unicode codepoint" (no conversion -- this was v1's hardcoded, only
    behaviour for reading a BDF's ENCODING values)."""

    def __init__(self, codec_name=None, mapping=None, lead_range=(0xA1, 0xFA)):
        self.codec_name = codec_name
        self.mapping = mapping  # dict code->unicode, or None if using codec_name
        self.lead_range = lead_range  # (lo, hi) exclusive hi, for codec-based enumeration

    def code_to_unicode(self, code):
        if code < 0x80:
            return code
        if self.mapping is not None:
            return self.mapping.get(code)
        if self.codec_name == "unicode":
            return code if code <= 0xFFFF else None
        hi, lo = (code >> 8) & 0xFF, code & 0xFF
        try:
            decoded = bytes([hi, lo]).decode(self.codec_name)
        except (UnicodeDecodeError, ValueError):
            return None
        if len(decoded) != 1:
            # A handful of big5hkscs codepoints decode to a 2-codepoint
            # compatibility sequence rather than one precomposed
            # character (no single Unicode codepoint exists for them).
            # Not representable in this format's single-codepoint model.
            return None
        return ord(decoded)

    def code_list(self, available_unicodes=None):
        """The full list of output codes this charset defines.
        available_unicodes (an iterable/keys-view of Unicode codepoints
        actually present in the source, after source-charset
        resolution) is only consulted for the 'unicode' kind, since
        there's no sensible way to "enumerate all of Unicode" -- for
        Big5-family codecs and mapping tables, the charset's own fixed
        code table is enumerated instead (with blanks for codes the
        source doesn't have a glyph for, matching CHINESE.FNT's fixed
        13800-entry structure)."""
        codes = list(range(0x20, 0x7F))
        if self.mapping is not None:
            codes += sorted(c for c in self.mapping if c >= 0x80)
            return codes
        if self.codec_name == "unicode":
            if available_unicodes:
                codes += sorted(u for u in available_unicodes if u >= 0x80)
            return codes
        # These 5 codepoints exist in Python's 'big5' codec but are
        # absent from the real CHINESE.FNT (see big5_code_list()
        # docstring) -- only exclude them when reproducing that exact
        # file's code space, not for other codecs/lead-byte ranges.
        excluded = {0xA140, 0xA1FE, 0xA240, 0xA2CC, 0xA2CE} if self.codec_name == "big5" else set()
        lo, hi = self.lead_range
        for lead in range(lo, hi):
            for trail in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
                code = (lead << 8) | trail
                if code in excluded:
                    continue
                if self.code_to_unicode(code) is not None:
                    codes.append(code)
        return codes


def big5_code_list():
    """All codepoints CHINESE.FNT actually contains: ASCII 0x20-0x7E,
    then every valid Big5 two-byte code 0xA140-0xF9FE (lead 0xA1-0xF9,
    trail 0x40-0x7E or 0xA1-0xFE), validated by successfully decoding
    via the 'big5' codec (skips unassigned codepoints).

    Excludes 5 codepoints present in Python's 'big5' codec but absent
    from the real CHINESE.FNT: 0xA140 (ideographic space, apparently
    omitted entirely -- U+3000 doesn't appear at any code in the real
    file), and 0xA1FE/0xA240/0xA2CC/0xA2CE, which are the well-known
    Big5 duplicate-mapping codepoints for full-width slash/backslash
    and 十/卅 -- CHINESE.FNT keeps only the canonical codes for those
    (0xA241/0xA242/0xA451/0xA4CA) and drops the duplicates. Verified
    by diffing our generated list against CHINESE.FNT's actual 13800
    codes: this is the complete and only difference.
    """
    return Charset("big5").code_list()


# ---------------------------------------------------------------------
# Source charset auto-detection
# ---------------------------------------------------------------------

def _is_big5_structural(code):
    """True if `code` has the byte shape of a real two-byte Big5-family
    code (lead 0xA1-0xF9, trail 0x40-0x7E or 0xA1-0xFE) -- regardless of
    whether it's actually assigned a character. This numeric range never
    overlaps with the Unicode codepoints real Chinese text uses (those
    start at 0x3000/0x4E00), so matching this shape is strong evidence
    the BDF's ENCODING values are raw Big5 codes, not Unicode."""
    lead, trail = (code >> 8) & 0xFF, code & 0xFF
    return 0xA1 <= lead <= 0xF9 and (0x40 <= trail <= 0x7E or 0xA1 <= trail <= 0xFE)


def _is_big5_ext_lead(code):
    """True if `code` has a lead byte in the 0x81-0xA0 range that plain
    Big5 leaves unused but Big5-HKSCS/Big5-UAO/Big5E all additionally
    use for extension characters."""
    lead, trail = (code >> 8) & 0xFF, code & 0xFF
    return 0x81 <= lead <= 0xA0 and (0x40 <= trail <= 0x7E or 0xA1 <= trail <= 0xFE)


def _is_cjk_unicode_like(code):
    """True if `code`, taken AS a Unicode codepoint, falls in a range
    real Chinese/CJK text plausibly uses (general punctuation, CJK
    punctuation and Unified Ideographs, CJK compatibility ideographs,
    halfwidth/fullwidth forms). Deliberately stops below 0xA000 so it
    never overlaps with genuine Big5 lead-byte codes."""
    return (
        0x2000 <= code <= 0x2BFF
        or 0x3000 <= code <= 0x9FFF
        or 0xF900 <= code <= 0xFAFF
        or 0xFF00 <= code <= 0xFFEF
    )


def _decodes_ok(code, codec_name):
    hi, lo = (code >> 8) & 0xFF, code & 0xFF
    try:
        bytes([hi, lo]).decode(codec_name)
        return True
    except (UnicodeDecodeError, ValueError):
        return False


def detect_source_charset(font, explicit="auto", explicit_mapping=None, verbose=True):
    """Resolve the Charset that interprets the BDF's own raw ENCODING
    values. Priority: explicit mapping file > explicit --source-charset
    > header hints (CHARSET_REGISTRY / FONT XLFD name) combined with a
    structural guess from the ENCODING values actually used."""
    if explicit_mapping:
        mapping = load_mapping_file(explicit_mapping)
        if verbose:
            print(f"Source charset: external mapping file '{explicit_mapping}' ({len(mapping)} codes).")
        return Charset(mapping=mapping)

    if explicit and explicit != "auto":
        if verbose:
            print(f"Source charset: '{explicit}' (explicit).")
        return Charset(explicit)

    codes = [c for c in font.glyphs if c >= 0x80]
    total = len(codes)
    if total == 0:
        if verbose:
            print("Source charset: no double-byte glyphs found (ASCII-only BDF) -- nothing to detect.")
        return Charset("unicode")

    big5_struct = sum(1 for c in codes if _is_big5_structural(c))
    ext_lead = [c for c in codes if _is_big5_ext_lead(c)]
    cjk_like = sum(1 for c in codes if _is_cjk_unicode_like(c))
    struct_ratio = big5_struct / total
    cjk_ratio = cjk_like / total

    hint_text = " ".join(filter(None, [font.charset_registry, font.xlfd_font_name])).lower()
    header_hint = None
    if "iso10646" in hint_text or "unicode" in hint_text:
        header_hint = "unicode"
    elif "hkscs" in hint_text:
        header_hint = "big5hkscs"
    elif "big5" in hint_text or "cp950" in hint_text:
        header_hint = "big5"

    if verbose:
        print(f"Source charset detection: {total} non-ASCII ENCODING values -- "
              f"{big5_struct} ({struct_ratio:.0%}) have the Big5 lead/trail byte shape, "
              f"{len(ext_lead)} use the 0x81-0xA0 extended lead-byte range, "
              f"{cjk_like} ({cjk_ratio:.0%}) look like plausible Unicode/CJK codepoints.")
        if header_hint:
            print(f"  BDF header hint (CHARSET_REGISTRY / FONT name): suggests '{header_hint}'.")

    if struct_ratio > 0.9 or header_hint in ("big5", "big5hkscs"):
        if ext_lead:
            hkscs_ok = sum(1 for c in ext_lead if _decodes_ok(c, "big5hkscs"))
            big5_ok = sum(1 for c in ext_lead if _decodes_ok(c, "big5"))
            if hkscs_ok >= big5_ok and hkscs_ok > 0:
                charset = Charset("big5hkscs")
                resolved = hkscs_ok
            else:
                charset = Charset(header_hint if header_hint in ("big5", "big5hkscs") else "big5")
                resolved = big5_ok
            if verbose:
                print(f"  -> Guessing '{charset.codec_name}'.")
            unresolved = len(ext_lead) - resolved
            if unresolved and verbose:
                print(f"  NOTE: {unresolved} extended-lead-byte codes (0x81-0xA0) don't decode under "
                      f"standard Big5/Big5-HKSCS -- these look like Big5-UAO/Big5E/Big5+ extension "
                      f"characters. Use --source-mapping-file with the matching table (e.g. "
                      f"uao241-b2u.txt from http://moztw.org/docs/big5/table/) to include them.")
        else:
            charset = Charset("big5")
            if verbose:
                print("  -> Guessing 'big5'.")
        return charset

    if cjk_ratio > 0.5 or header_hint == "unicode":
        if verbose:
            print("  -> Guessing 'unicode' (BDF ENCODING values are Unicode codepoints).")
        return Charset("unicode")

    if verbose:
        print("  -> AMBIGUOUS: could not confidently determine the source charset; defaulting to "
              "'unicode' (v1's original assumption). Pass --source-charset or --source-mapping-file "
              "to override if this is wrong.")
    return Charset("unicode")


# ---------------------------------------------------------------------
# Main conversion
# ---------------------------------------------------------------------

def convert(bdf_path, out_path, size=16, dx=0, dy=0,
            source_charset_name="auto", source_mapping_file=None,
            target_charset_name="big5", target_mapping_file=None,
            lead_range=(0xA1, 0xFA), ascii_only=False, ext_only=False, verbose=True):
    font = parse_bdf(bdf_path)
    if verbose:
        print(f"BDF loaded: FONT_ASCENT={font.font_ascent} FONT_DESCENT={font.font_descent} "
              f"(cell height={font.cell_size}), {len(font.glyphs)} glyphs, "
              f"PIXEL_SIZE={font.default_size}")
        if font.cell_size != size:
            print(f"NOTE: BDF cell height ({font.cell_size}) != target canvas size "
                  f"({size}); using --size {size} as the output canvas regardless "
                  f"(glyphs positioned within it via the ascent formula, clipped if needed).")

    # --- Step 1: interpret the BDF's own ENCODING values (source charset) ---
    source_charset = detect_source_charset(font, explicit=source_charset_name,
                                            explicit_mapping=source_mapping_file, verbose=verbose)

    glyphs_by_unicode = {}
    collisions = 0
    for raw_code, glyph in font.glyphs.items():
        u = source_charset.code_to_unicode(raw_code)
        if u is None:
            continue
        if u in glyphs_by_unicode:
            collisions += 1
        glyphs_by_unicode[u] = glyph
    if verbose:
        msg = (f"Source interpretation: {len(glyphs_by_unicode)}/{len(font.glyphs)} raw BDF glyphs "
               f"resolved to a Unicode codepoint.")
        if collisions:
            msg += f" ({collisions} codepoint collisions -- later glyph in file order wins.)"
        print(msg)

    # --- Step 2: decide the output code space (target charset) ---
    if target_mapping_file:
        target_mapping = load_mapping_file(target_mapping_file)
        target_charset = Charset(mapping=target_mapping)
        if verbose:
            print(f"Target charset: external mapping file '{target_mapping_file}' ({len(target_mapping)} codes).")
    else:
        target_charset = Charset(target_charset_name, lead_range=lead_range)
        if verbose:
            print(f"Target charset: '{target_charset_name}'.")

    all_codes = target_charset.code_list(available_unicodes=glyphs_by_unicode.keys())
    if ascii_only:
        codes = list(range(0x20, 0x7F))
    elif ext_only:
        codes = [c for c in all_codes if c >= 0x80]
    else:
        codes = all_codes
    if verbose:
        label = (f"mapping file ({len(target_charset.mapping)} codes)" if target_charset.mapping is not None
                  else f"codec '{target_charset.codec_name}'")
        print(f"Target code space: {label} -> {len(codes)} total codepoints to emit.")

    bpr = (size + 7) // 8
    record_size = 2 + size * bpr

    n_found = 0
    n_no_unicode = 0     # table has no Unicode target for this code (or explicitly 0x0000)
    n_no_glyph_pua = 0   # has a Unicode target in the PUA block, but font has no glyph for it
    n_no_glyph_other = 0 # has a normal (non-PUA) Unicode target, but font has no glyph for it
    records = bytearray()
    missing_samples = []

    for code in codes:
        cp = target_charset.code_to_unicode(code)
        glyph = glyphs_by_unicode.get(cp) if cp else None
        if glyph is not None:
            n_found += 1
        else:
            if not cp:
                n_no_unicode += 1
                reason = "no-unicode-mapping"
            elif 0xE000 <= cp <= 0xF8FF:
                n_no_glyph_pua += 1
                reason = "PUA-not-in-font"
            else:
                n_no_glyph_other += 1
                reason = "not-in-font"
            if len(missing_samples) < 20:
                missing_samples.append((code, cp, reason))
        canvas = composite_glyph(glyph, font.font_ascent, size, dx, dy)
        bitmap = pack_canvas(canvas, size)
        records += struct.pack("<H", code)
        records += bitmap

    header = b"CZHT" + b"\x00\x01" + struct.pack("<H", len(codes))
    out = header + bytes(records)

    with open(out_path, "wb") as f:
        f.write(out)

    n_missing = n_no_unicode + n_no_glyph_pua + n_no_glyph_other
    if verbose:
        print(f"Wrote {out_path}: {len(codes)} glyphs ({n_found} found in BDF, "
              f"{n_missing} missing -> blank), {len(out)} bytes, "
              f"{size}x{size} canvas, {record_size} bytes/record.")
        if n_missing:
            print(f"  Missing breakdown: {n_no_unicode} had no Unicode target in the mapping "
                  f"table (nothing to look up), {n_no_glyph_pua} mapped to a Private-Use-Area "
                  f"codepoint (U+E000-U+F8FF) that the font doesn't define -- common for UAO/"
                  f"Big5-2003 extension codes, since standard fonts don't include PUA glyphs -- "
                  f"and {n_no_glyph_other} mapped to a normal Unicode codepoint the font simply "
                  f"doesn't have a glyph for.")
        if missing_samples:
            parts = [
                f"0x{code:04X}->" + (f"U+{cp:04X}({reason})" if cp else "(no-unicode)")
                for code, cp, reason in missing_samples
            ]
            print("  First missing codes: " + ", ".join(parts)
                  + (" ..." if n_missing > len(missing_samples) else ""))

    return len(codes), n_found, n_missing


def _float_or_int(s):
    v = float(s)
    return int(v) if v == int(v) else v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bdf", help="Input BDF font file")
    ap.add_argument("out", help="Output .FNT file (CZHT format)")
    ap.add_argument("--size", type=int, default=16, help="Canvas size, N for NxN (default 16)")
    ap.add_argument("--dx", type=_float_or_int, default=0, help="Manual horizontal nudge added to BBX xoffset")
    ap.add_argument("--dy", type=_float_or_int, default=0, help="Manual vertical nudge added to computed top")
    ap.add_argument("--ascii-only", action="store_true", help="Only emit ASCII 0x20-0x7E (skip the double-byte charset)")
    ap.add_argument("--big5-only", dest="ext_only", action="store_true",
                     help="Only emit double-byte/non-ASCII codepoints (skip ASCII). Name kept from v1; "
                          "applies to whatever --target-charset is (not just Big5).")

    src = ap.add_argument_group("source charset (how to read the input BDF's own ENCODING values)")
    src.add_argument("--source-charset", choices=["auto", "unicode", "big5", "cp950", "big5hkscs"],
                      default="auto",
                      help="'auto' (default) inspects the BDF header and the shape of its ENCODING "
                           "values to guess whether they're Unicode codepoints or raw Big5-family "
                           "codes, and prints its reasoning. Set explicitly to skip detection. "
                           "Ignored if --source-mapping-file is given.")
    src.add_argument("--source-mapping-file", default=None,
                      help="Path to an external 'CODE UNICODE' table (same format as "
                           "--target-mapping-file) describing what the BDF's ENCODING values mean. "
                           "Required for Big5-UAO/Big5E/Big5-2003-encoded source BDFs, since none of "
                           "those are built-in Python codecs. Overrides --source-charset.")

    tgt = ap.add_argument_group("target charset (what code space the output .FNT is keyed by)")
    tgt.add_argument("--target-charset", choices=["unicode", "big5", "cp950", "big5hkscs"], default="big5",
                      help="Code space for the output .FNT's lookup codes (default: big5, matching "
                           "CHINESE.FNT). Use 'unicode' to key the output by Unicode codepoint instead. "
                           "Ignored if --target-mapping-file is given.")
    tgt.add_argument("--mapping-file", "--target-mapping-file", dest="target_mapping_file", default=None,
                      help="Path to an external 'CODE UNICODE' mapping table (hex pairs, one per "
                           "line, '#' comments allowed) -- e.g. one of the b2u.txt tables at "
                           "http://moztw.org/docs/big5/table/ for Big5-2003, UAO, CP950, etc. "
                           "This is the way to target Big5E/UAO specifically, or any newer/custom "
                           "code table not covered by --target-charset. Overrides --target-charset. "
                           "(--mapping-file is the old v1 flag name, kept as an alias.)")
    ap.add_argument("--lead-range", default=None, metavar="LO-HI",
                     help="Override the lead-byte range scanned when the TARGET charset is a codec "
                          "(big5/cp950/big5hkscs), hex, e.g. 81-FA to include the 0x81-0xA0 extension "
                          "block (only useful with a codec that actually maps those bytes -- "
                          "big5hkscs partially does; for exact Big5E/UAO output use --mapping-file).")
    ap.add_argument("--quiet", action="store_true", help="Suppress informational/progress output")
    args = ap.parse_args()

    lead_range = (0xA1, 0xFA)
    if args.lead_range:
        lo_s, hi_s = args.lead_range.split("-")
        lead_range = (int(lo_s, 16), int(hi_s, 16) + 1)

    convert(args.bdf, args.out, size=args.size, dx=int(args.dx), dy=int(args.dy),
            source_charset_name=args.source_charset, source_mapping_file=args.source_mapping_file,
            target_charset_name=args.target_charset, target_mapping_file=args.target_mapping_file,
            lead_range=lead_range, ascii_only=args.ascii_only, ext_only=args.ext_only,
            verbose=not args.quiet)


if __name__ == "__main__":
    main()
