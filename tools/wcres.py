#!/usr/bin/env python3
"""
wcres.py — Wing Commander Kilrathi Saga resource extractor
Python port of Bekenn/wcdx dist/wcres/src/main.cpp

Extracts individual resources from GAMEDAT archive files used by
Wing Commander 1 and Wing Commander 2.

Usage (single file):
  wcres.py -o <output_path>  -extract <resource_index> <input_file>
  wcres.py -o <output_dir>   -extract-all              <input_file>
  wcres.py -o <output_path>  -pack                     <input_file>...

Usage (whole GAMEDAT directory – batch mode):
  wcres.py -o <output_dir>   -extract-all              <gamedat_dir>
    → mirrors the directory layout under <output_dir>
    → each archive FOO.FNT becomes <output_dir>/FOO.FNT/<N> files

Requires only the Python standard library (no third-party packages).
"""

import sys
import os
import struct
import argparse
from pathlib import Path


# ---------------------------------------------------------------------------
# LZW bit-stream reader  (mirrors bit_input_stream in the original C++)
# ---------------------------------------------------------------------------

class BitInputStream:
    """Read an integer stream of variable-width codes from a byte source."""

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._pos = 0
        self._bit_pos = 8
        self._cur_byte = 0

    def read(self, bit_width: int) -> int:
        result = 0
        dst_bit = 0
        remaining = bit_width

        while remaining:
            if self._bit_pos == 8:
                if self._pos >= len(self._data):
                    raise EOFError("Unexpected end of compressed data")
                self._cur_byte = self._data[self._pos]
                self._pos += 1
                self._bit_pos = 0

            bits_used = min(remaining, 8 - self._bit_pos)
            chunk = (self._cur_byte >> self._bit_pos) & ((1 << bits_used) - 1)
            self._bit_pos += bits_used
            remaining -= bits_used
            result |= chunk << dst_bit
            dst_bit += bits_used

        return result


# ---------------------------------------------------------------------------
# LZW decompressor  (mirrors extract_compressed in the original C++)
# ---------------------------------------------------------------------------

MIN_CODE_WIDTH     = 9
MAX_CODE_WIDTH     = 12
INITIAL_TABLE_SIZE = 0x102
RESET_CODE         = 0x100
STOP_CODE          = 0x101


def decompress(data: bytes) -> bytes:
    """Decompress an LZW-encoded resource payload and return the raw bytes."""
    reader = BitInputStream(data)
    out = bytearray()

    code = reader.read(MIN_CODE_WIDTH)
    if code == STOP_CODE:
        return bytes(out)
    if code != RESET_CODE:
        raise ValueError("Compressed stream missing initial reset code")

    table = [(0, 0)] * (1 << MAX_CODE_WIDTH)

    while True:
        code_width  = MIN_CODE_WIDTH
        threshold   = 1 << code_width
        table_size  = INITIAL_TABLE_SIZE
        prev_code   = RESET_CODE

        while True:
            code = reader.read(code_width)
            if code == RESET_CODE or code == STOP_CODE:
                break

            index = code
            if index > table_size:
                raise ValueError(f"Decompressor table index {index} out of range")

            if index == table_size:
                if prev_code == RESET_CODE:
                    raise ValueError("Decompressor table index out of range (after reset)")
                index = prev_code

            stack = []
            tmp = index
            while tmp > 0xFF:
                stack.append(table[tmp][1])
                tmp = table[tmp][0]
            first_value = tmp

            out.append(first_value)
            for b in reversed(stack):
                out.append(b)

            if prev_code != RESET_CODE:
                table[table_size] = (prev_code, first_value)
                if code == table_size:
                    out.append(first_value)
                table_size += 1
                if table_size == threshold and code_width < MAX_CODE_WIDTH:
                    code_width += 1
                    threshold <<= 1

            prev_code = code

        if code == STOP_CODE:
            break

    return bytes(out)


# ---------------------------------------------------------------------------
# Archive validation & reading
# ---------------------------------------------------------------------------

def is_wc_archive(data: bytes) -> bool:
    """
    Quick sanity-check: does this look like a WC GAMEDAT archive?
    Returns False for files that are clearly not archives (too small,
    impossible offsets, etc.) so we can skip non-archive files gracefully.
    """
    if len(data) < 8:
        return False
    try:
        file_size      = struct.unpack_from("<I", data, 0)[0]
        first_entry    = struct.unpack_from("<I", data, 4)[0]
        first_offset   = first_entry & 0x00FFFFFF
        # first_offset must be at least 8 (two uint32s) and a multiple of 4
        if first_offset < 8 or first_offset % 4 != 0:
            return False
        # first_offset must be within the file
        if first_offset > len(data):
            return False
        # file_size should roughly match actual size (allow ±4 bytes for alignment)
        if abs(file_size - len(data)) > 4:
            return False
        return True
    except struct.error:
        return False


def read_directory(data: bytes):
    """
    Parse the resource directory.
    Returns a list of (offset, size, resource_type) tuples.
    """
    file_size      = struct.unpack_from("<I", data, 0)[0]
    first_entry    = struct.unpack_from("<I", data, 4)[0]
    first_offset   = first_entry & 0x00FFFFFF
    resource_count = (first_offset - 4) // 4

    entries = []
    for i in range(resource_count):
        desc_offset = 4 + 4 * i
        raw         = struct.unpack_from("<I", data, desc_offset)[0]
        rtype       = raw >> 24
        roffset     = raw & 0x00FFFFFF

        next_desc = desc_offset + 4
        if next_desc < first_offset:
            next_raw    = struct.unpack_from("<I", data, next_desc)[0]
            next_offset = next_raw & 0x00FFFFFF
            rsize       = next_offset - roffset
        else:
            rsize = file_size - roffset

        entries.append((roffset, rsize, rtype))

    return entries


def extract_resource(data: bytes, index: int) -> bytes:
    """Extract and decompress (if needed) a single resource by index."""
    entries = read_directory(data)
    if index >= len(entries):
        raise IndexError(
            f"Resource index {index} out of range "
            f"(file has {len(entries)} resources)"
        )

    roffset, rsize, rtype = entries[index]
    payload = data[roffset : roffset + rsize]

    if rtype == 1:
        uncompressed_size = struct.unpack_from("<I", payload, 0)[0]
        raw = decompress(payload[4:])
        if len(raw) != uncompressed_size:
            raise RuntimeError(
                f"Resource {index}: size mismatch "
                f"(expected {uncompressed_size}, got {len(raw)})"
            )
        return raw
    else:
        return payload


def pack_resources(input_paths: list, output_path: str) -> None:
    """Create a new archive from a list of raw resource files."""
    blobs = [Path(p).read_bytes() for p in input_paths]
    count = len(blobs)

    header_size = 4 + 4 * count
    offsets = []
    cursor = header_size
    for b in blobs:
        offsets.append(cursor)
        cursor += len(b)
    total_size = cursor

    buf = bytearray()
    buf += struct.pack("<I", total_size)
    for off in offsets:
        buf += struct.pack("<I", off & 0x00FFFFFF)
    for b in blobs:
        buf += b

    Path(output_path).write_bytes(bytes(buf))
    print(f"Packed {count} resource(s) → {output_path}")


# ---------------------------------------------------------------------------
# Extract helpers
# ---------------------------------------------------------------------------

def cmd_extract_all_from_bytes(data: bytes, out_dir: Path, label: str = "") -> int:
    """
    Extract all resources from `data` into `out_dir`.
    Returns number of resources extracted.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    entries = read_directory(data)
    for i in range(len(entries)):
        try:
            raw = extract_resource(data, i)
            dest = out_dir / str(i)
            dest.write_bytes(raw)
            print(f"  {label}[{i}] → {dest}  ({len(raw):,} bytes)")
        except Exception as e:
            print(f"  {label}[{i}] SKIP: {e}", file=sys.stderr)
    return len(entries)


def cmd_extract_all(input_path: Path, output_path: str) -> None:
    """
    -extract-all dispatcher.
    • If input_path is a FILE  → extract its resources into output_path/
    • If input_path is a DIR   → walk all files, skip non-archives,
                                  mirror structure under output_path/
    """
    if input_path.is_dir():
        # ── Batch / directory mode ──────────────────────────────────────────
        out_root = Path(output_path)
        archive_files = sorted(
            f for f in input_path.iterdir() if f.is_file()
        )
        if not archive_files:
            print(f"No files found in {input_path}", file=sys.stderr)
            return

        total_archives = 0
        total_resources = 0
        skipped = 0

        for src in archive_files:
            data = src.read_bytes()
            if not is_wc_archive(data):
                print(f"  Skipping {src.name}  (not a WC archive)")
                skipped += 1
                continue

            out_dir = out_root / src.name
            print(f"\n── {src.name} ──")
            n = cmd_extract_all_from_bytes(data, out_dir, label=f"{src.name}/")
            total_archives += 1
            total_resources += n

        print(
            f"\n{'─'*60}\n"
            f"Done. Processed {total_archives} archive(s), "
            f"{total_resources} resource(s) total."
            + (f"  ({skipped} non-archive file(s) skipped)" if skipped else "")
        )

    else:
        # ── Single-file mode ────────────────────────────────────────────────
        data = input_path.read_bytes()
        if not is_wc_archive(data):
            raise ValueError(
                f"{input_path} does not look like a WC archive.\n"
                "Make sure you point to a file inside GAMEDAT, e.g. GAMEDAT\\FONTS.FNT"
            )
        out_dir = Path(output_path) if output_path else Path.cwd()
        n = cmd_extract_all_from_bytes(data, out_dir)
        print(f"\nDone. {n} resource(s) extracted to {out_dir}")


def cmd_extract_one(input_path: Path, index: int, output_path: str) -> None:
    data = input_path.read_bytes()
    raw = extract_resource(data, index)
    dest = Path(output_path)
    dest.write_bytes(raw)
    print(f"Extracted resource {index} → {dest}  ({len(raw):,} bytes)")


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="wcres.py",
        description="Wing Commander Kilrathi Saga GAMEDAT resource tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Extract ONE resource from a single archive file
  python wcres.py -o out.bin -extract 0 GAMEDAT\\FONTS.FNT

  # Extract ALL resources from a single archive file
  python wcres.py -o fonts\\   -extract-all GAMEDAT\\FONTS.FNT

  # Extract ALL resources from every archive in the GAMEDAT directory
  python wcres.py -o out\\     -extract-all GAMEDAT\\

  # Pack raw files back into an archive
  python wcres.py -o FONTS_NEW.FNT -pack 0 1 2 3
""",
    )
    p.add_argument("-o", dest="output", metavar="OUTPUT_PATH",
                   help="Output file or directory")

    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("-extract", dest="extract_index", type=int, metavar="INDEX",
                      help="Extract a single resource by index")
    mode.add_argument("-extract-all", dest="extract_all", action="store_true",
                      help="Extract all resources (also accepts a directory as input)")
    mode.add_argument("-pack", dest="pack", action="store_true",
                      help="Pack input files into a new archive at OUTPUT_PATH")

    p.add_argument("inputs", nargs="+", metavar="INPUT",
                   help="Input archive file, GAMEDAT directory, or (with -pack) multiple files")
    return p


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)

    # ── Pack mode ────────────────────────────────────────────────────────────
    if args.pack:
        if not args.output:
            parser.error("-o is required with -pack")
        pack_resources(args.inputs, args.output)
        return

    # ── Extract modes need exactly one INPUT ─────────────────────────────────
    if len(args.inputs) != 1:
        parser.error("Exactly one INPUT is required for -extract / -extract-all")

    input_path = Path(args.inputs[0])

    if args.extract_all:
        if not args.output:
            parser.error("-o is required with -extract-all")
        cmd_extract_all(input_path, args.output)

    else:  # -extract INDEX
        if not input_path.is_file():
            parser.error(f"{input_path} is not a file. -extract requires a single archive file.")
        if args.output is None:
            parser.error("-o is required with -extract")
        cmd_extract_one(input_path, args.extract_index, args.output)


if __name__ == "__main__":
    try:
        main()
    except (IndexError, ValueError, RuntimeError, FileNotFoundError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
