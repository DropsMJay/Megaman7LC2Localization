#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_cpp_table.py
===================

Generates CharEncoding.h -- the lookup table the MM7Loc DLL uses to convert
translated text (UTF-8, as written in TextStrings.json) into the single-byte
character codes the game's font system actually expects.

Background
----------
Mega Man 7's font isn't a real character-encoding table. Each 1-byte
character code is literally a pixel coordinate into a 128x128 texture split
into a 16x16 grid of 8x8-pixel cells:

    column = (byte & 0x0F) * 8
    row    = (byte >> 4)   * 8

`mm7_encoding.xml` documents what the *original, unmodified* Japanese font
draws in every one of those 256 cells (letters, digits, kana, punctuation,
etc.), figured out by hand, cell by cell.

Translators don't touch that file. Instead, when a cell's pixels get
redrawn to show a new character (e.g. turning an unused hiragana into an
accented Latin letter for a Portuguese translation), the new mapping goes
in the OVERRIDES dict below. This script combines the original XML with
your overrides and produces a ready-to-compile C++ header.

Usage
-----
    python build_cpp_table.py
    python build_cpp_table.py --xml mm7_encoding.xml --output CharEncoding.h

Typical translator workflow
----------------------------
1. Pick an unused cell (check mm7_encoding.xml for cells that are blank in
   the original font -- repurposing a blank cell is always safer than
   overwriting a real character).
2. Redraw that cell's pixels in BOTH font textures (COL0 = fill, COL1 =
   outline) to draw your new character.
3. Add an entry to OVERRIDES below: { byte_code: 'your_character' }.
4. Run this script to regenerate CharEncoding.h.
5. Rebuild the DLL.

Never hand-edit the generated CharEncoding.h -- always change OVERRIDES and
regenerate. Editing the generated file directly is how you end up with the
comment saying one character while the actual byte mapping is another.
"""

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path

# ---------------------------------------------------------------------------
# EDIT THIS SECTION: repurposed cells
# ---------------------------------------------------------------------------
# Maps byte_code -> new_character for any cell whose pixels have been
# hand-edited to draw something other than what mm7_encoding.xml documents.
# This does NOT modify mm7_encoding.xml (which stays an accurate record of
# the original, unmodified font) -- it's only applied when building the
# lookup table the DLL actually uses.
#
# Example: 0x90 originally drew hiragana 'あ' (documented in the XML), but
# a Portuguese translation redrew that cell to show 'Á' instead:
#
#     0x90: 'Á',
#
OVERRIDES = {
    0x90: 'Á',  # was 'あ'
    0x91: 'À',  # was 'い'
    0x92: 'Â',  # was 'う'
    0x93: 'Ã',  # was 'え'
    0x94: 'É',  # was 'お'
    0x95: 'Ê',  # was 'か'
    0x96: 'Í',  # was 'き'
    0x97: 'Ú',  # was 'く'
    0x98: 'Õ',  # was 'け'
    0x99: 'Ó',  # was 'こ'
    0x9A: 'Ô',  # was 'さ'
    0x9B: 'Ç',  # was 'し'
}
# ---------------------------------------------------------------------------


def load_entries_from_xml(xml_path: Path) -> list[tuple[int, str]]:
    """Reads mm7_encoding.xml and returns a list of (byte_code, character) pairs."""
    tree = ET.parse(xml_path)
    entries = []
    for entry in tree.getroot().findall('Entry'):
        code = int(entry.get('Encode'), 16)
        char = entry.get('Char')
        entries.append((code, char))
    return entries


def apply_overrides(entries: list[tuple[int, str]]) -> list[tuple[int, str]]:
    """Replaces the character for any byte code listed in OVERRIDES."""
    return [(code, OVERRIDES.get(code, char)) for code, char in entries]


def build_reverse_table(entries: list[tuple[int, str]]) -> list[tuple[str, int]]:
    """
    Inverts (byte_code -> character) into (character -> byte_code), which is
    the direction the DLL actually needs (it starts with a translated
    character and needs to know which byte to emit).

    A few characters legitimately appear at more than one byte code (the
    original font reuses some symbols/punctuation in more than one place).
    When that happens, we keep only the FIRST one encountered -- entries are
    processed in ascending byte-code order, so the lowest/most "standard"
    code wins and later duplicates are silently dropped.
    """
    entries_sorted = sorted(entries, key=lambda pair: pair[0])

    seen_chars = set()
    reverse_table = []
    for code, char in entries_sorted:
        if char in seen_chars:
            continue
        seen_chars.add(char)
        reverse_table.append((char, code))
    return reverse_table


def format_header(reverse_table: list[tuple[str, int]]) -> str:
    """Renders the final CharEncoding.h contents as a string."""
    lines = []
    for char, code in reverse_table:
        codepoint = ord(char)
        comment = char if char.isprintable() else f'U+{codepoint:04X}'
        lines.append(f"    {{ 0x{codepoint:04X}, 0x{code:02X} }}, // '{comment}'")

    overrides_summary = ', '.join(
        f"0x{code:02X}='{char}'" for code, char in sorted(OVERRIDES.items())
    ) or '(none)'

    return f"""// CharEncoding.h
//
// AUTO-GENERATED by build_cpp_table.py -- do not hand-edit this file.
// To change a mapping, edit OVERRIDES in build_cpp_table.py and re-run it.
//
// Maps a Unicode codepoint (decoded from the UTF-8 TextStrings.json) to the
// single-byte character code the game's font system expects. See the
// script's module docstring for how the game's font actually works.
//
// Currently repurposed cells: {overrides_summary}
#pragma once
#include <unordered_map>
#include <cstdint>

static const std::unordered_map<char32_t, unsigned char> g_charToByte = {{
{chr(10).join(lines)}
}};
"""


def main():
    parser = argparse.ArgumentParser(
        description="Generate CharEncoding.h from mm7_encoding.xml + OVERRIDES."
    )
    parser.add_argument(
        '--xml', type=Path, default=Path('mm7_encoding.xml'),
        help="Path to mm7_encoding.xml (default: %(default)s)"
    )
    parser.add_argument(
        '--output', type=Path, default=Path('CharEncoding.h'),
        help="Where to write the generated header (default: %(default)s)"
    )
    args = parser.parse_args()

    if not args.xml.exists():
        parser.error(f"Can't find {args.xml} -- pass --xml <path> if it's somewhere else.")

    entries = load_entries_from_xml(args.xml)
    entries = apply_overrides(entries)
    reverse_table = build_reverse_table(entries)
    header_text = format_header(reverse_table)

    args.output.write_text(header_text, encoding='utf-8')
    print(f"Wrote {len(reverse_table)} entries to {args.output}")


if __name__ == '__main__':
    main()