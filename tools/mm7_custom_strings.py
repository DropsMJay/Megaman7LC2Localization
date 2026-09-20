#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mm7_custom_strings.py
======================

A "strings"-like scanner for Mega Man 7's own 1-byte font codepage, instead
of ASCII/UTF-16 like every generic tool assumes.

WHY THIS EXISTS
---------------
MM7's font isn't a real text encoding: each byte 0x00-0xFF is a coordinate
into a 16x16 grid of glyph cells (see build_cpp_table.py / CharEncoding.h
from the MM7Loc project). Bytes 0x20-0x7A happen to line up with ASCII
(that's why English lines show up in a normal Ghidra/strings search), but
Japanese kana live at 0x80-0xFD in an arbitrary mapping that no standard
string-search tool understands. This means Japanese text is invisible to
"strings", Ghidra's String Search, etc. -- not because it isn't there, but
because nothing recognizes those byte runs as text.

This script scans a binary for runs of bytes that ARE valid codes in this
specific codepage (using the known mapping from CharEncoding.h), the same
way "strings" hunts for runs of printable ASCII. Any run found is decoded
back to readable text (kana shown as the actual character; unmapped bytes
shown as [XX]) so you can spot real dialogue vs. noise.

USAGE
-----
    python mm7_custom_strings.py MMLC2.exe
    python mm7_custom_strings.py MMLC2.exe --min-len 4
    python mm7_custom_strings.py MMLC2.exe --out hits.txt

Reads the same byte->char table as CharEncoding.h (kept in sync manually
below -- regenerate from mm7_encoding.xml/build_cpp_table.py if that table
changes).
"""

import argparse
import sys

# Byte -> Unicode codepoint, taken directly from CharEncoding.h (g_charToByte,
# inverted). Note 0x90-0x9B are the MM7Loc PT-BR overrides (accented Latin);
# in an UNMODIFIED/original exe those same byte values are hiragana instead,
# so if you're scanning a stock/unpatched exe looking for Japanese, treat
# hits containing 0x90-0x9B specially -- decode_byte() below flags them.
BYTE_TO_CHAR = {
    0x20: ' ', 0x21: '!', 0x22: '\u201D', 0x23: '#', 0x25: '\u25BC',
    0x27: "'", 0x28: '(', 0x29: ')', 0x2A: '*', 0x2B: '\u25AE',
    0x2C: ',', 0x2D: '-', 0x2E: '.', 0x2F: '/',
    0x30: '0', 0x31: '1', 0x32: '2', 0x33: '3', 0x34: '4',
    0x35: '5', 0x36: '6', 0x37: '7', 0x38: '8', 0x39: '9',
    0x3A: ':', 0x3B: ';', 0x3C: '<', 0x3D: '=', 0x3E: '>', 0x3F: '?',
    0x40: '\u00A9',
    0x41: 'A', 0x42: 'B', 0x43: 'C', 0x44: 'D', 0x45: 'E', 0x46: 'F',
    0x47: 'G', 0x48: 'H', 0x49: 'I', 0x4A: 'J', 0x4B: 'K', 0x4C: 'L',
    0x4D: 'M', 0x4E: 'N', 0x4F: 'O', 0x50: 'P', 0x51: 'Q', 0x52: 'R',
    0x53: 'S', 0x54: 'T', 0x55: 'U', 0x56: 'V', 0x57: 'W', 0x58: 'X',
    0x59: 'Y', 0x5A: 'Z', 0x5B: '[', 0x5C: '\u00A5', 0x5D: ']',
    0x5E: '^', 0x5F: '_', 0x60: '`',
    0x61: 'a', 0x62: 'b', 0x63: 'c', 0x64: 'd', 0x65: 'e', 0x66: 'f',
    0x67: 'g', 0x68: 'h', 0x69: 'i', 0x6A: 'j', 0x6B: 'k', 0x6C: 'l',
    0x6D: 'm', 0x6E: 'n', 0x6F: 'o', 0x70: 'p', 0x71: 'q', 0x72: 'r',
    0x73: 's', 0x74: 't', 0x75: 'u', 0x76: 'v', 0x77: 'w', 0x78: 'x',
    0x79: 'y', 0x7A: 'z',
    0x80: '\u30A9', 0x81: '\u30A7',
    # 0x90-0x9B: MM7Loc override (accented Latin). Original font = hiragana.
    0x90: '\u00C1', 0x91: '\u00C0', 0x92: '\u00C2', 0x93: '\u00C3',
    0x94: '\u00C9', 0x95: '\u00CA', 0x96: '\u00CD', 0x97: '\u00DA',
    0x98: '\u00D5', 0x99: '\u00D3', 0x9A: '\u00D4', 0x9B: '\u00C7',
    0x9C: '\u3059', 0x9D: '\u305B', 0x9E: '\u305D', 0x9F: '\u305F',
    0xA0: '\u3061', 0xA1: '\u3064', 0xA2: '\u3066', 0xA3: '\u3068',
    0xA4: '\u306A', 0xA5: '\u306B', 0xA6: '\u306C', 0xA7: '\u306D',
    0xA8: '\u306E', 0xA9: '\u306F', 0xAA: '\u3072', 0xAB: '\u3075',
    0xAC: '\u3078', 0xAD: '\u307B', 0xAE: '\u307E', 0xAF: '\u307F',
    0xB0: '\u3080', 0xB1: '\u3081', 0xB2: '\u3082', 0xB3: '\u3084',
    0xB4: '\u3086', 0xB5: '\u3088', 0xB6: '\u3089', 0xB7: '\u308A',
    0xB8: '\u308B', 0xB9: '\u308C', 0xBA: '\u308D', 0xBB: '\u308F',
    0xBC: '\u3092', 0xBD: '\u3093', 0xBE: '\u3083', 0xBF: '\u3085',
    0xC0: '\u3087', 0xC1: '\u3063',
    0xC4: '\u300C', 0xC5: '\u300D', 0xC6: '\u30FB', 0xC7: '\u309C',
    0xC8: '\u309B',
    0xCB: '\u30E3', 0xCC: '\u30E5', 0xCD: '\u30E7', 0xCE: '\u30C3',
    0xCF: '\u30FC',
    0xD0: '\u30A2', 0xD1: '\u30A4', 0xD2: '\u30A6', 0xD3: '\u30A8',
    0xD4: '\u30AA', 0xD5: '\u30AB', 0xD6: '\u30AD', 0xD7: '\u30AF',
    0xD8: '\u30B1', 0xD9: '\u30B3', 0xDA: '\u30B5', 0xDB: '\u30B7',
    0xDC: '\u30B9', 0xDD: '\u30BB', 0xDE: '\u30BD', 0xDF: '\u30BF',
    0xE0: '\u30C1', 0xE1: '\u30C4', 0xE2: '\u30C6', 0xE3: '\u30C8',
    0xE4: '\u30CA', 0xE5: '\u30CB', 0xE6: '\u30CC', 0xE7: '\u30CD',
    0xE8: '\u30CE', 0xE9: '\u30CF', 0xEA: '\u30D2', 0xEB: '\u30D5',
    0xEC: '\u30D8', 0xED: '\u30DB', 0xEE: '\u30DE', 0xEF: '\u30DF',
    0xF0: '\u30E0', 0xF1: '\u30E1', 0xF2: '\u30E2', 0xF3: '\u30E4',
    0xF4: '\u30E6', 0xF5: '\u30E8', 0xF6: '\u30E9', 0xF7: '\u30EA',
    0xF8: '\u30EB', 0xF9: '\u30EC', 0xFA: '\u30ED', 0xFB: '\u30EF',
    0xFC: '\u30F2', 0xFD: '\u30F3',
    0xFF: '\u30A3',
}

# Bytes whose *original* (unmodified) meaning we don't know (MM7Loc
# repurposed them). Flag runs containing these so you know to double-check.
REPURPOSED_BYTES = set(range(0x90, 0x9C))

VALID_BYTES = set(BYTE_TO_CHAR.keys())


def decode_byte(b):
    if b in BYTE_TO_CHAR:
        return BYTE_TO_CHAR[b]
    return f'[{b:02X}]'


def scan(data, min_len):
    """Yield (offset, decoded_str, has_repurposed_bytes) for each run of
    valid-codepage bytes of length >= min_len, terminated by 0x00 or an
    invalid byte (mirroring how `strings` finds printable runs)."""
    n = len(data)
    i = 0
    while i < n:
        b = data[i]
        if b in VALID_BYTES:
            start = i
            run = bytearray()
            has_repurposed = False
            while i < n and data[i] in VALID_BYTES:
                if data[i] in REPURPOSED_BYTES:
                    has_repurposed = True
                run.append(data[i])
                i += 1
            if len(run) >= min_len:
                decoded = ''.join(decode_byte(x) for x in run)
                yield start, decoded, has_repurposed
        else:
            i += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('binary', help='Path to MMLC2.exe (or any file) to scan')
    ap.add_argument('--min-len', type=int, default=4,
                     help='Minimum run length to report (default: 4)')
    ap.add_argument('--out', default=None,
                     help='Write results to this file instead of stdout')
    ap.add_argument('--kana-only', action='store_true',
                     help='Only show runs containing at least one byte >= 0x80 '
                          '(i.e. skip pure-ASCII hits you already found)')
    args = ap.parse_args()

    with open(args.binary, 'rb') as f:
        data = f.read()

    out = open(args.out, 'w', encoding='utf-8') if args.out else sys.stdout

    count = 0
    for offset, decoded, has_repurposed in scan(data, args.min_len):
        if args.kana_only and not any(ord(c) > 0x7A for c in decoded if not c.startswith('[')):
            # crude check: skip runs that decoded entirely within ASCII range
            if not any(c not in [chr(x) for x in range(0x20, 0x7B)] for c in decoded):
                continue
        flag = '  [contains repurposed 0x90-0x9B bytes]' if has_repurposed else ''
        print(f'0x{offset:08X}: {decoded}{flag}', file=out)
        count += 1

    if args.out:
        out.close()
    print(f'\n{count} runs found (min_len={args.min_len}).', file=sys.stderr)


if __name__ == '__main__':
    main()
