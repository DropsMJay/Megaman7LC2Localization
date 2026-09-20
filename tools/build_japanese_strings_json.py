#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_japanese_strings_json.py
=================================

Builds GameTextJP.json (the file JapaneseText.cpp's
PatchJapaneseStrings() reads) from:
  1. A --pairs-file in the same format find_jp_text_sources.py produces
     (index / dest_RVA / source_RVA per line) -- dest_RVA is what the
     C++ patcher needs to know WHICH staging slot to overwrite.
  2. The raw .exe, to capture each entry's real header bytes (the small
     numeric words before the actual text starts) so the C++ side can
     reconstruct them verbatim -- see JapaneseText.h's comment on why
     this matters.

By default this just copies the ORIGINAL Japanese text through
unchanged (so you can verify the round-trip -- patch in, get the same
text out, confirms the pipeline works before translating anything).
Edit the "text" field of individual entries in the resulting JSON to
actually translate them.

USAGE
-----
    python build_japanese_strings_json.py MMLC2.exe ghidra_pairs_clean.txt --out GameTextJP.json

    # to only include specific entries (by index), rather than all of them:
    python build_japanese_strings_json.py MMLC2.exe ghidra_pairs_clean.txt --only 9,10,18 --out GameTextJP.json

    # to check an already-translated GameTextJP.json for the most common
    # box-transition mistake (a [DrawClosingMark] not immediately followed by "\n",
    # which renders the next box's text starting from wherever the cursor
    # was left instead of the left margin):
    python build_japanese_strings_json.py --validate GameTextJP.json
"""

import argparse
import json
import re
import struct
import sys


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
    0x90: '\u3042', 0x91: '\u3044', 0x92: '\u3046', 0x93: '\u3048',
    0x94: '\u304A', 0x95: '\u304B', 0x96: '\u304D', 0x97: '\u304F',
    0x98: '\u3051', 0x99: '\u3053', 0x9A: '\u3055', 0x9B: '\u3057',
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
    # CONFIRMED (cross-referenced against 5 known screenshots): the game
    # uses dedicated full-width punctuation codes distinct from the
    # ASCII-range 0x21/0x3F used by English text.
    0xC2: '\uFF1F',  # '？' (full-width question mark)
    0xC3: '\uFF01',  # '！' (full-width exclamation mark)
}

# CONFIRMED (extracted from UndefinedFunction_1400017b0's own decompile,
# cross-referenced 30/30 against entries known to be cut off by [0001] --
# every one of them has a non-zero third field here, zero exceptions).
# Maps table INDEX -> RVA of that entry's auxiliary pointer table
# (this is plVar2[2] in LAB_140056260's decompile). Confirmed live: for
# entry 168/170, resolving through this table produced the literal
# string "UFO".
AUX_TABLE_BY_ENTRY_INDEX = {
    0: 0x577b28, 9: 0x571428, 10: 0x573098, 20: 0x578138, 25: 0x57a3a0,
    27: 0x578618,
    28: 0x573878, 36: 0x575d98, 37: 0x57a5b8, 38: 0x579b58, 39: 0x5741a0,
    43: 0x5750d8, 45: 0x578248, 57: 0x579620, 59: 0x577608, 60: 0x578ca0,
    61: 0x577418, 62: 0x578148, 63: 0x575bf0, 64: 0x575d18, 65: 0x575948,
    66: 0x5753a8, 68: 0x572848, 69: 0x576db0, 70: 0x57a250, 71: 0x579aa0,
    72: 0x573540, 73: 0x574f58, 74: 0x5781d0, 75: 0x5756e0, 76: 0x579280,
    77: 0x579280, 78: 0x579280, 79: 0x579280, 80: 0x579280, 81: 0x579280,
    82: 0x579280, 83: 0x579280, 91: 0x579280, 144: 0x579418, 149: 0x578230,
    155: 0x5763c8, 158: 0x576658, 160: 0x576308, 161: 0x575630, 163: 0x5749d8,
    164: 0x5727f8, 165: 0x579b48, 167: 0x575618, 168: 0x576a78, 170: 0x576a78,
}


def try_read_ascii_string(pe, rva, min_len=1, max_len=200):
    """Same helper find_literal_ptrs.py already uses -- reads a printable,
    null-terminated ASCII string starting exactly at rva, or None."""
    try:
        chunk = pe.read_bytes(rva, max_len)
    except ValueError:
        return None
    out = []
    for b in chunk:
        if b == 0:
            break
        if 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            return None
    else:
        return None
    if len(out) < min_len:
        return None
    return ''.join(out)


def resolve_aux_table_jump(pe, entry_index, uVar5):
    """CONFIRMED (live instrumentation): word 0x0100 means 'read the NEXT
    word as an index (uVar5) into this entry's auxiliary pointer table,
    resolve the pointer there, and read a plain ASCII C-string from it.'
    Returns the resolved string, or None if we can't resolve it (entry
    has no aux table, or something about it doesn't look like a clean
    string -- falls back to the old raw-bracket behavior in that case)."""
    aux_table_rva = AUX_TABLE_BY_ENTRY_INDEX.get(entry_index)
    if aux_table_rva is None:
        return None
    try:
        slot_bytes = pe.read_bytes(aux_table_rva + uVar5 * 8, 8)
        target_va = struct.unpack('<Q', slot_bytes)[0]
        target_rva = pe.va_to_rva(target_va)
    except (ValueError, struct.error):
        return None
    return try_read_ascii_string(pe, target_rva)

# CONFIRMED (9/9 matches across 5 known screenshots): 0x0C combines with
# the FOLLOWING character to apply dakuten (て->で, か->が, etc).
DAKUTEN_MAP = {
    'か': 'が', 'き': 'ぎ', 'く': 'ぐ', 'け': 'げ', 'こ': 'ご',
    'さ': 'ざ', 'し': 'じ', 'す': 'ず', 'せ': 'ぜ', 'そ': 'ぞ',
    'た': 'だ', 'ち': 'ぢ', 'つ': 'づ', 'て': 'で', 'と': 'ど',
    'は': 'ば', 'ひ': 'び', 'ふ': 'ぶ', 'へ': 'べ', 'ほ': 'ぼ',
    'カ': 'ガ', 'キ': 'ギ', 'ク': 'グ', 'ケ': 'ゲ', 'コ': 'ゴ',
    'サ': 'ザ', 'シ': 'ジ', 'ス': 'ズ', 'セ': 'ゼ', 'ソ': 'ゾ',
    'タ': 'ダ', 'チ': 'ヂ', 'ツ': 'ヅ', 'テ': 'デ', 'ト': 'ド',
    'ハ': 'バ', 'ヒ': 'ビ', 'フ': 'ブ', 'ヘ': 'ベ', 'ホ': 'ボ',
}
# CONFIRMED NEW (found decoding entry 21 against a known screenshot --
# "[0D]ハワーアッ[0D]フ" -> "パワーアップ"): 0x0D is the han-dakuten
# sibling of 0x0C, same "combines with the following character" rule.
HANDAKUTEN_MAP = {
    'は': 'ぱ', 'ひ': 'ぴ', 'ふ': 'ぷ', 'へ': 'ぺ', 'ほ': 'ぽ',
    'ハ': 'パ', 'ヒ': 'ピ', 'フ': 'プ', 'ヘ': 'ペ', 'ホ': 'ポ',
}


class PEImage(object):
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        self._parse()

    def _parse(self):
        data = self.data
        if data[0:2] != b'MZ':
            raise ValueError("Nao parece ser um PE valido.")
        e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
        coff_off = e_lfanew + 4
        machine, num_sections = struct.unpack_from('<HH', data, coff_off)
        size_opt_header = struct.unpack_from('<H', data, coff_off + 16)[0]
        opt_header_off = coff_off + 20
        magic = struct.unpack_from('<H', data, opt_header_off)[0]
        if magic == 0x20b:
            self.image_base = struct.unpack_from('<Q', data, opt_header_off + 24)[0]
        else:
            self.image_base = struct.unpack_from('<I', data, opt_header_off + 28)[0]
        section_table_off = opt_header_off + size_opt_header
        self.sections = []
        for i in range(num_sections):
            off = section_table_off + i * 40
            virt_size, virt_addr, raw_size, raw_ptr = struct.unpack_from('<IIII', data, off + 8)
            self.sections.append({'virt_addr': virt_addr, 'virt_size': virt_size,
                                   'raw_ptr': raw_ptr, 'raw_size': raw_size})

    def rva_to_offset(self, rva):
        for s in self.sections:
            if s['virt_addr'] <= rva < s['virt_addr'] + max(s['virt_size'], s['raw_size']):
                return s['raw_ptr'] + (rva - s['virt_addr'])
        raise ValueError("RVA 0x%x fora de qualquer secao." % rva)

    def read_bytes(self, rva, n):
        off = self.rva_to_offset(rva)
        return self.data[off:off + n]

    def va_to_rva(self, va):
        return va - self.image_base


def is_valid_char_word(b):
    if len(b) < 2:
        return False
    lo, hi = b[0], b[1]
    return hi == 0 and lo != 0 and lo in BYTE_TO_CHAR


def is_strong_text_signal(b):
    if len(b) < 2:
        return False
    lo, hi = b[0], b[1]
    return hi == 0 and lo >= 0x80 and lo in BYTE_TO_CHAR


def is_dakuten_lead(word_bytes, next_bytes):
    """True if word_bytes is a dakuten/han-dakuten trigger (0x0C/0x0D)
    immediately followed by a valid base-kana word. Found via a real
    example: entries like 19 ('がってん', "got it!"), 60 ('ジャンクマン',
    "Junk Man") had their FIRST character silently swallowed into the
    header, because is_strong_text_signal() only looks at raw byte
    value >=0x80 -- the dakuten TRIGGER word itself (0x0C/0x0D, a small
    value) never passes that check on its own, even though it's the
    genuine start of a real word once you look one word ahead. Without
    this, the boundary scan below would correctly find "real text
    nearby" (thanks to the REST of the sentence) but mark the start one
    full character (2 words) too late -- same category of problem as
    the missing-parameter bug (see the big comment further down), just
    for the very FIRST character of an entry's real text instead of a
    header instruction's parameter."""
    if word_bytes is None or next_bytes is None or len(word_bytes) < 2 or len(next_bytes) < 2:
        return False
    lo, hi = word_bytes[0], word_bytes[1]
    return hi == 0 and lo in (0x0C, 0x0D) and is_valid_char_word(next_bytes)


def is_aux_table_lead(pe, entry_index, word_bytes, index_bytes):
    """True if word_bytes is the 0x0100 aux-table-jump marker AND it
    resolves to real, non-empty ASCII text. Same category of gap as
    is_dakuten_lead() above, but for embedded Latin/credits text (e.g.
    entry 0's "AND CAPCOM"/"ALL STAFF") instead of a single voiced kana
    character. is_strong_text_signal() can NEVER catch this on its own
    -- ASCII letters (0x41-0x5A etc) are all below 0x80, so plain-Latin
    content never looks "strong" by that check, no matter where it
    sits. Without this, entries whose real translatable content is
    (partly or entirely) embedded Latin text via the aux table -- the
    whole "shared credits setup" family (entries 0, 69, 70, 73-76, and
    likely others) -- keep that content permanently stuck in
    header_bytes, invisible to translation, regardless of the
    instruction-alignment fix elsewhere in this file (that fix only
    guarantees header_bytes doesn't split an instruction in half; it
    says nothing about WHERE the boundary should be to expose real
    content as translatable text).
    """
    if word_bytes is None or index_bytes is None or len(word_bytes) < 2 or len(index_bytes) < 2:
        return False
    lo, hi = word_bytes[0], word_bytes[1]
    if not (lo == 0x00 and hi == 0x01):
        return False
    index = index_bytes[0] | (index_bytes[1] << 8)
    resolved = resolve_aux_table_jump(pe, entry_index, index)
    return bool(resolved)


def decode_word_array(pe, rva, entry_index, max_chars=400):
    out = []
    cur = rva
    count = 0
    while count < max_chars:
        try:
            b = pe.read_bytes(cur, 2)
        except ValueError:
            break
        if len(b) < 2:
            break
        lo, hi = b[0], b[1]
        if lo == 0 and hi == 0:
            break  # true terminator -- the ONLY legitimate stop condition
        if hi != 0:
            # CONFIRMED (live instrumentation, see chat history): word
            # 0x0100 specifically means "jump to auxiliary string table"
            # -- the game reads the NEXT word as an index into a
            # per-entry pointer table, resolves it, and switches to
            # reading a plain ASCII C-string from there (confirmed live:
            # resolved to literal "UFO" for one known entry). We can't
            # follow that pointer statically here (it requires knowing
            # each entry's own auxiliary-table address, which isn't
            # captured by this script yet), so flag it clearly instead
            # of silently truncating -- this is NOT the end of the
            # entry's real content, just where our extractor currently
            # gives up.
            if lo == 0x00 and hi == 0x01:
                # uVar5 (o indice na tabela auxiliar) e a PROXIMA palavra
                # do MESMO array -- nao vem de lugar nenhum externo.
                try:
                    next_word = pe.read_bytes(cur + 2, 2)
                    uVar5 = next_word[0] | (next_word[1] << 8)
                except ValueError:
                    uVar5 = None

                resolved = None
                if uVar5 is not None:
                    resolved = resolve_aux_table_jump(pe, entry_index, uVar5)

                if resolved is not None:
                    out.append(resolved)
                    cur += 4  # consome tanto o [0001] quanto a palavra de indice
                    count += 2
                else:
                    # nao conseguimos resolver (entrada sem tabela auxiliar
                    # conhecida, ou o ponteiro nao apontou pra texto limpo)
                    # -- mantem o comportamento antigo, marcado com clareza.
                    out.append('[AUX_TABLE_JUMP:UNRESOLVED]')
                    cur += 2
                    count += 1
                continue

        # CONFIRMED (9/9 screenshots): PAIRED 08 08 = visible line break.
        # CONFIRMED (same dispatch-table slot as idx=8, which handles
        # BOTH the paired "[NewLine]" case below AND a lone single call):
        # per the jump table, index 8 -> LAB_140056970, the EXACT SAME
        # function already decompiled for [NewLine]:
        #   cursor = ((anchor ^ cursor) & 0x1F ^ cursor) + 0x20
        # -- i.e. "return to left margin + advance by one step". A
        # PAIRED "08 08" calls this TWICE in a row (a full visible line
        # break); a LONE "08" (as seen in the post-[DrawClosingMark] footer)
        # calls it just ONCE -- same mechanism, half the effect. Not a
        # separate mystery command, just the same one invoked once
        # instead of twice.
        if lo == 0x08:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2 and nxt[0] == 0x08 and nxt[1] == 0:
                out.append('[NewLine]')
                cur += 4
                count += 2
                continue
            out.append('[AdvanceCursorLine]')
            cur += 2
            count += 1
            continue

        # CONFIRMED RULE: 0x0C combines with the FOLLOWING character to
        # apply dakuten. JapaneseText.cpp's encoder re-derives the
        # [0x0C, base] pair from the precomposed character.
        if lo == 0x0C:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2 and nxt[1] == 0:
                base_char = BYTE_TO_CHAR.get(nxt[0])
                if base_char in DAKUTEN_MAP:
                    out.append(DAKUTEN_MAP[base_char])
                    cur += 4
                    count += 2
                    continue
            out.append('[0C]')
            cur += 2
            count += 1
            continue

        # CONFIRMED NEW: 0x0D combines with the FOLLOWING character to
        # apply han-dakuten.
        if lo == 0x0D:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2 and nxt[1] == 0:
                base_char = BYTE_TO_CHAR.get(nxt[0])
                if base_char in HANDAKUTEN_MAP:
                    out.append(HANDAKUTEN_MAP[base_char])
                    cur += 4
                    count += 2
                    continue
            out.append('[0D]')
            cur += 2
            count += 1
            continue
        
        # RENAMED from the old, misleading [NEWBOX]: confirmed via
        # dynamic debugging (x64dbg breakpoint + call stack on the box
        # object's constructor, see MM7Loc_INVESTIGATION_SUMMARY.md,
        # 3.10) that this marker does NOT create or destroy any box
        # object -- the box is created exactly ONCE per entry
        # (FUN_140057a10), never per this marker. It's purely a glyph
        # draw (0xCA, the closing full-width period "。").
        #
        # >>> IMPORTANT FOR TRANSLATORS/ANYONE HAND-EDITING "text" <<<
        # [DrawClosingMark] itself does NOT reset the left margin for the new
        # box. Confirmed live by decompiling BOTH sides of this:
        #   - idx=10's handler ([SetLeftMargin], see below) is what
        #     actually sets the margin -- and it only runs where it
        #     literally appears in the word stream (typically once, in
        #     the header, before any real text).
        #   - [NewLine] (the "\n" pair 08 08) is what RE-APPLIES that
        #     saved margin to the cursor for a new line -- it reads the
        #     anchor value [SetLeftMargin] wrote into [+0x32] and copies
        #     it back into the live cursor [+0x30].
        # So if a box after [DrawClosingMark] doesn't start with "\n", the
        # cursor just continues from wherever the PREVIOUS box left off
        # (usually far to the right) -- the new box's text renders
        # visibly shifted/warped instead of starting at the left
        # margin. Always put "\n" as the very first thing after
        # [DrawClosingMark] unless you've specifically confirmed the box should
        # continue from the old cursor position. Run this script with
        # --validate on your edited JSON to check for this automatically.
        if lo == 0xCA:
            out.append('[DrawClosingMark]')
            cur += 2
            count += 1
            continue

        # CONFIRMED RULE: 0x07 pauses ONCE for the NEXT word's value, in
        # frames (proven from FUN_1400568b0's decrement-and-compare
        # loop -- no fixed-point math, so it can't be anything but a
        # simple frame counter). Fires once, then normal pacing resumes.
        if lo == 0x07:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2 and nxt[1] == 0:
                out.append('[WaitFrames][%02X]' % nxt[0])
                cur += 4
                count += 2
                continue
            out.append('[07]')
            cur += 2
            count += 1
            continue

        # CONFIRMED RULE (live instrumentation, exact value match on
        # both 0x08 and 0x10 test cases): 0x02 sets [+0x3f], a PERSISTENT
        # per-character delay -- unlike 0x07, this doesn't fire once and
        # go away: it becomes the new "frames per character" rate for
        # every character until another [02] changes it again.
        if lo == 0x02:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2 and nxt[1] == 0:
                out.append('[SetTypeSpeed][%02X]' % nxt[0])
                cur += 4
                count += 2
                continue
            out.append('[02]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=3's handler): reads a parameter
        # word into [+0x34] -- a field already known from earlier
        # investigation as "per-entry typing pace (fixed-point)", but
        # previously no writer had been found for it. Distinct from
        # [SetTypeSpeed] (idx=2), which writes [+0x3f]/[+0x40] instead.
        # HYPOTHESIS (not proven): sets a BASE/initial typing speed for
        # the whole entry, separate from [SetTypeSpeed]'s real-time
        # adjustments. No function reading [+0x34] back has been found
        # yet to confirm the exact relationship between the two.
        #
        # BUG FIX (found via the new header_decoded field): every
        # single entry's header uses param=512 (0x0200) here -- NOT a
        # single byte. Same class of bug as [TabToColumn]/
        # [RepeatBlankTile] (see their comments) -- accept the full
        # 16-bit word, 2 hex digits when it fits in a byte, 4 otherwise.
        if lo == 0x03:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2:
                param = nxt[0] | (nxt[1] << 8)
                if param <= 0xFF:
                    out.append('[SetInitialTypeSpeed?][%02X]' % param)
                else:
                    out.append('[SetInitialTypeSpeed?][%04X]' % param)
                cur += 4
                count += 2
                continue
            out.append('[03]')
            cur += 2
            count += 1
            continue

            
        # CONFIRMED (same function as FUN_140056720, the reveal-pacing
        # timer already decompiled way back during the delay-bug
        # investigation): idx=5 in the jump table IS this function --
        # the raw control byte and the jump-table index are literally
        # the same value (LAB_140056260 writes it straight into [+0x24],
        # no translation). Always seen in the post-[DrawClosingMark] footer.
        # Doesn't write [+0x34] (the pacing rate) -- just reads whatever
        # is already there, so this isn't a deliberate "reset"; it's
        # more likely a side effect of dispatch reuse. Labeled
        # descriptively rather than by assumed purpose.
        if lo == 0x05:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2 and nxt[1] == 0:
                out.append('[PacingTick][%02X]' % nxt[0])
                cur += 4
                count += 2
                continue
            out.append('[05]')
            cur += 2
            count += 1
            continue
            
        # CONFIRMED (decompile of idx=10's handler): writes the SAME
        # value into both [+0x30] (current cursor position) and [+0x32]
        # (the fixed anchor that [NewLine]/idx=8 later reads to recompute
        # the cursor). Runs once per dialogue box, in the header --
        # sets where each line starts.
        #
        # BUG FIX (found via the header_decoded round-trip test):
        # entry 91's real header uses margin=390 (0x186), and entries
        # 160/162/163 use 643 (0x283) -- both exceed a single byte.
        # Same class of fix as [TabToColumn]/[RepeatBlankTile]/
        # [SetInitialTypeSpeed?] above -- accept the full 16-bit word.
        if lo == 0x0A:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2:
                param = nxt[0] | (nxt[1] << 8)
                if param <= 0xFF:
                    out.append('[SetLeftMargin][%02X]' % param)
                else:
                    out.append('[SetLeftMargin][%04X]' % param)
                cur += 4
                count += 2
                continue
            out.append('[0A]')
            cur += 2
            count += 1
            continue
            
        # CONFIRMED (decompile of idx=15's handler): sets [+0x3e]=1, no
        # parameter read -- unlike every other control code so far.
        # [+0x3e] is checked inside the [WaitFrames] wait-loop alongside
        # a button-input bitmask (DAT_1408fd8d8), and FUN_140056490 (the
        # game's "fast-forward, skip typing" mechanism) never checks it
        # at all -- suggesting this specifically blocks the player from
        # skipping a dramatic pause by holding the confirm button.
        if lo == 0x0F:
            out.append('[ForceWait]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=14's handler): clears [+0x3e] to
        # 0 -- the EXACT SAME field [ForceWait] (idx=15) sets to 1.
        # No parameter. The "undo"/cancel counterpart to [ForceWait].
        if lo == 0x0E:
            out.append('[ClearForceWait]')
            cur += 2
            count += 1
            continue

        # RESOLVED (not a conflict): dispatch-table indices 12 (0x0C)
        # and 13 (0x0D) point to decompiled functions (FUN_140056a90 /
        # FUN_140056ad0) that push the cursor up one line-step, draw a
        # small fixed tile (0xC9 for idx=12, 0xC7 for idx=13), then pop
        # the cursor back -- i.e. draw a small mark ABOVE the current
        # position without moving on. That's exactly what dakuten (゛)
        # and han-dakuten (゜) marks look like visually (small marks
        # above-right of the base kana). This isn't a second, competing
        # meaning for 0x0C/0x0D -- it's the actual glyph-level mechanism
        # BEHIND the dakuten/han-dakuten combiner already handled above:
        # 0x0C draws the raised ゛ mark, then the NEXT word (the base
        # kana's own byte) gets read and drawn normally on the next
        # loop iteration, landing at the same column. The
        # DAKUTEN_MAP/HANDAKUTEN_MAP precomposed-character handling
        # above already represents this correctly at a readable level,
        # so no separate raw tag is needed here.

        # CONFIRMED (decompile of idx=16's handler): reads a parameter
        # word (only the LOW BYTE is kept -- uses `char`, not `short`,
        # unlike every other parameter-reading handler so far) and
        # writes it into [+0x41]. This field is READ inside
        # FUN_140056360 (the character-reader/dispatcher), right after
        # EVERY glyph is drawn: `if ([+0x41] != 0) FUN_1400fba90([+0x41], 0)`.
        # HYPOTHESIS (not proven): a per-character sound-effect trigger
        # (the classic RPG "typewriter blip" while text reveals) --
        # fits the "runs after every character, only when set" pattern.
        # FUN_1400fba90 itself hasn't been decompiled/investigated.
        if lo == 0x10:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2:
                param = nxt[0]  # low byte only -- see note above
                out.append('[SetTypeSound?][%02X]' % param)
                cur += 4
                count += 2
                continue
            out.append('[10]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=19's handler, FUN_140056c70, AND
        # of FUN_1400092c0 itself, the function it calls): walks a
        # LINKED LIST of visual items (icons/sprites) attached to the
        # dialogue box object, and for each one flagged a specific way,
        # either destroys it or fully resets it to a default/hidden
        # state (moved off-screen to X=1920,Y=1080, scale/color
        # zeroed). Empties the whole list afterward. No parameter of
        # its own. Only ever seen as the VERY FIRST word of an entry,
        # and only in the weapon-description entries (36-50, 52, 56,
        # 57). HYPOTHESIS (not proven): clears/hides whatever icon was
        # left over from the PREVIOUS weapon description before this
        # one's own icon gets shown -- fits "always first word" (clean
        # slate before anything else runs) and the entries' shared
        # theme (each weapon has its own floating icon).
        #
        # idx=20 (0x14) is a CONFIRMED synonym -- a direct Ghidra
        # cross-reference on FUN_140056c70 shows BOTH the idx=19 and
        # idx=20 jump-table slots point at it (not a coincidence of
        # identical code, the literal same function address). Kept as
        # a separate raw byte value here (not folded into a single
        # check) purely to preserve exact round-trip fidelity if 0x14
        # ever shows up somewhere -- both decode to the same tag.
        if lo == 0x13 or lo == 0x14:
            out.append('[ClearIconList?]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=17's handler): reads a parameter
        # word and passes it DIRECTLY to FUN_1400fba90(value, 0) -- the
        # exact same confirmed sound-effect-manager function
        # [SetTypeSound?] (idx=16) uses. Unlike idx=16 (which sets a
        # PERSISTENT per-character sound that keeps firing after every
        # glyph), this plays the sound ONCE, immediately, on its own.
        if lo == 0x11:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2:
                param = nxt[0] | (nxt[1] << 8)
                if param <= 0xFF:
                    out.append('[PlaySound][%02X]' % param)
                else:
                    out.append('[PlaySound][%04X]' % param)
                cur += 4
                count += 2
                continue
            out.append('[11]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=18's handler): reads a parameter
        # word; if it's GREATER than 16 (0x10), calls FUN_1400fb840()
        # (not decompiled yet, no arguments). HYPOTHESIS (not proven):
        # some kind of conditional trigger/flag, gated by the parameter
        # value crossing a threshold. Labeled provisionally.
        if lo == 0x12:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2:
                param = nxt[0] | (nxt[1] << 8)
                if param <= 0xFF:
                    out.append('[ConditionalTrigger?][%02X]' % param)
                else:
                    out.append('[ConditionalTrigger?][%04X]' % param)
                cur += 4
                count += 2
                continue
            out.append('[12]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=21's handler, LAB_140056c90):
        # zeroes [+0x38] (the same undocumented field used in
        # [TabToColumn]'s formula -- a real clue towards its purpose,
        # possibly a line/paragraph counter that resets here) and
        # copies the saved margin anchor ([+0x32]) back into the live
        # cursor ([+0x30]) -- exactly half of what [NewLine] does
        # (return to the left margin), WITHOUT the "move down one line"
        # part [NewLine] also does. No parameter. Confirmed via a
        # direct Ghidra cross-reference: this jump-table slot (idx=21,
        # not idx=20 -- an earlier pass at this table briefly mixed the
        # two up, corrected here) is the ONLY one pointing at this
        # function.
        if lo == 0x15:
            out.append('[ResetToMarginSameLine]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=1's handler): reads a parameter
        # word, same shape as [SetLeftMargin] -- just writes it into
        # [+0x3c] instead. Previously thought to have no reader; found
        # one by decompiling FUN_140056cb0 (the glyph-drawing function),
        # which passes [+0x3c] as the THIRD argument to FUN_140057850
        # (the function that resolves which dialogue-box/balloon to
        # draw into, alongside the cursor position). HYPOTHESIS (not yet
        # proven): this selects which character the dialogue balloon's
        # tail points at -- fits every screenshot seen so far (balloon
        # tail aims at whichever of the 2-3 characters on screen is
        # "speaking"), but no test has isolated this parameter alone
        # yet. Labeled provisionally; rename if that hypothesis is
        # disproven.
        if lo == 0x01:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2 and nxt[1] == 0:
                out.append('[SetSpeaker?][%02X]' % nxt[0])
                cur += 4
                count += 2
                continue
            out.append('[01]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=11's handler): increments a GLOBAL
        # counter (DAT_1408fd954), completely unrelated to this entry's
        # own state (doesn't touch anything at param_1+...). No
        # parameter read. Always seen in the post-[DrawClosingMark] footer,
        # consistent with "count how many box-transitions have happened
        # this session/game" or similar telemetry -- doesn't affect
        # this box's layout or timing, so NOT a suspect for the
        # [DrawClosingMark]-without-"\n" margin bug.
        if lo == 0x0B:
            out.append('[IncrementCounter]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=4's handler): reads a parameter
        # word (N), then calls the glyph-drawing function N times with
        # tile index 0 (presumably blank/space in this font sheet),
        # advancing the cursor by 1 each time -- i.e. "print N blank
        # tiles". HYPOTHESIS: a spacing/indent command.
        #
        # IMPORTANT CORRECTION: this was previously unhandled, which
        # caused a MISREAD of the common mid-dialogue footer pattern --
        # the raw words "04 09 06" were being shown as standalone "[04]"
        # followed by a coincidental "[TabToColumn][06]" (since 09 just
        # happened to be [TabToColumn]'s own code, consuming the next
        # word as ITS parameter by pure chance). The correct reading is
        # "[04]"'s OWN parameter is that "09" -- there is no
        # [TabToColumn] in that footer at all. Whatever "06" turns out
        # to mean is still open (see chat history).
        if lo == 0x04:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2:
                param = nxt[0] | (nxt[1] << 8)
                if param <= 0xFF:
                    out.append('[RepeatBlankTile][%02X]' % param)
                else:
                    out.append('[RepeatBlankTile][%04X]' % param)
                cur += 4
                count += 2
                continue
            out.append('[04]')
            cur += 2
            count += 1
            continue

        # CONFIRMED (decompile of idx=9's handler): reads a parameter
        # word, same "consume next word" shape as [SetLeftMargin]/
        # [SetSpeaker?]. Computes:
        #   [+0x30] = ([+0x38] & 0xFFF8) * 4 + [+0x32] + param
        # -- i.e. an absolute cursor-X jump, offset by BOTH the saved
        # left-margin anchor ([+0x32]) AND some multiple of an
        # undocumented field [+0x38] (possibly a row/line counter).
        # HYPOTHESIS (not proven, but well-supported): a column/tab
        # positioning command. Found in entry 68 (a credits screen)
        # immediately before "PRODUCER" and "PROFESSOR F" -- two role
        # labels that would need independent horizontal alignment,
        # exactly the kind of thing this command's math would be used
        # for.
        #
        # CORRECTION: this was earlier (wrongly) believed to ALSO
        # appear in the common mid-dialogue [DrawClosingMark] footer pattern
        # ("[04][TabToColumn][06]"). That was a misread caused by idx=4
        # ALSO consuming a parameter word (see [RepeatBlankTile] below)
        # -- once that was fixed, the "09" in that footer turned out to
        # be idx=4's own parameter, not a real [TabToColumn] invocation
        # at all. [TabToColumn] is (so far) ONLY confirmed in the
        # entry-68 credits-screen usage.
        #
        # BUG FIX (see chat history): unlike every other parameterized
        # control code here, this one's parameter is NOT always a
        # single byte -- entry 68's real usage has param=1360 (0x0550,
        # hi=0x05), which the original "hi must be 0" check rejected,
        # silently falling through to the generic glyph fallback and
        # printing the low byte's char ('P') instead. Accept the FULL
        # 16-bit word: prints as 2 hex digits [XX] when it fits in a
        # byte, or 4 hex digits [XXXX] otherwise.
        # CONFIRMED (decompile of idx=6's handler): the "press to
        # continue" prompt. On first call, draws tile 0xFE (a small
        # triangle/arrow glyph, matching BYTE_TO_CHAR's 0x25 -> '\u25BC'
        # entry) at the cursor. Every 32 frames, toggles a sprite
        # visibility bit (blinking). Loops (returning a special
        # "suspend" value, 0x1000) until the confirm button is pressed
        # (checked against the same global input bitmask [WaitFrames]
        # uses), then hides the sprite and lets execution continue. No
        # parameter read.
        if lo == 0x06:
            out.append('[WaitForConfirm]')
            cur += 2
            count += 1
            continue

        if lo == 0x09:
            nxt = pe.read_bytes(cur + 2, 2)
            if len(nxt) == 2:
                param = nxt[0] | (nxt[1] << 8)
                if param <= 0xFF:
                    out.append('[TabToColumn][%02X]' % param)
                else:
                    out.append('[TabToColumn][%04X]' % param)
                cur += 4
                count += 2
                continue
            out.append('[09]')
            cur += 2
            count += 1
            continue

        out.append(BYTE_TO_CHAR.get(lo, '[%02X]' % lo))
        cur += 2
        count += 1
    return ''.join(out)


def find_header_and_text(pe, base_rva, entry_index, max_header_words=500, window=8, min_hits=3):
    """Returns (header_words, decoded_text) for the entry at base_rva.
    header_words is the full 16-bit word sequence before the real text
    starts (needed so the C++ patcher can reproduce it verbatim).

    BUG FIX (see chat history): this used to keep only the low byte of
    each header word (`b[0]`), silently discarding the high byte. That
    was fine for header words in the 0-255 range, but at least one
    confirmed real-world case (entry 18's header word at index 3, a
    reveal-pacing "set delay" parameter) has a true value of 0x0200
    (512) -- its high byte is NOT zero. Dropping it produced 0 instead
    of 512, and that single lost byte was the entire cause of a
    dialogue-box softlock in the patched game (see MM7Loc's
    DialogueState.cpp / FUN_140056630 investigation). Header words are
    NOT guaranteed to be single bytes; read and preserve the full
    little-endian 16-bit word.
    """
    for start_word in range(max_header_words):
        cur = base_rva + start_word * 2
        window_bytes = []
        for i in range(window + 1):  # +1: need one extra word of lookahead for the dakuten/aux-table checks below
            try:
                b = pe.read_bytes(cur + i * 2, 2)
            except ValueError:
                b = None
            window_bytes.append(b)

        # A confirmed aux-table jump (0x0100 resolving to real, non-empty
        # text) is strong enough evidence on its own -- unlike a single
        # kana byte or a single dakuten pair, it doesn't need 2 more
        # "hits" nearby to be trusted. Checked BEFORE the windowed
        # hits>=min_hits logic below, and short-circuits straight to
        # "boundary found here" if it matches -- an 8-word window mostly
        # full of unrelated control codes (typical for the "shared
        # credits setup" family, entries like 0/69/70/73-76) would
        # otherwise never reach min_hits from aux-table jumps alone,
        # since each jump is only 2 words.
        if is_aux_table_lead(pe, entry_index, window_bytes[0], window_bytes[1]):
            real_start_word = start_word
        else:
            def is_strong_or_dakuten_lead(i):
                b = window_bytes[i]
                if b is not None and is_strong_text_signal(b):
                    return True
                nxt = window_bytes[i + 1] if i + 1 < len(window_bytes) else None
                return is_dakuten_lead(b, nxt)

            hits = sum(1 for i in range(window) if is_strong_or_dakuten_lead(i))
            if hits < min_hits:
                continue
            first_valid_offset = None
            for i in range(window):
                if is_strong_or_dakuten_lead(i):
                    first_valid_offset = i
                    break
            real_start_word = start_word + first_valid_offset

        # (boundary found -- either via the aux-table short-circuit
        # above, or via the windowed hits>=min_hits scan; the rest of
        # this function's original logic, unchanged, continues below)
        if True:

            # BUG FIX (found via a full header_decoded/header_bytes
            # round-trip test -- see chat history): this heuristic only
            # checks "does this word LOOK like real kana" -- it has no
            # idea that certain opcodes (SetSpeaker?, SetTypeSpeed,
            # SetInitialTypeSpeed?, RepeatBlankTile, PacingTick,
            # WaitFrames, TabToColumn, SetLeftMargin, SetTypeSound?,
            # dakuten/han-dakuten, the aux-table jump) ALWAYS consume
            # exactly one more word as a parameter, unconditionally,
            # regardless of what that word's value looks like. If the
            # window-scan's candidate boundary happens to fall in the
            # MIDDLE of one of these two-word instructions (because the
            # parameter word itself accidentally looked like "real
            # text"), header_bytes ends up missing that final
            # parameter, and the translated "text" field's first word
            # gets silently swallowed by the game as that parameter
            # instead of being drawn. This affected the majority of
            # entries in this table.
            #
            # Fix: don't trust the window-scan's boundary directly --
            # instead walk word-by-word from the very start (rva 0,
            # true position 0 of this entry), respecting every
            # instruction's real word-count (2 for the opcodes above,
            # 1 otherwise), and snap the boundary forward to the first
            # instruction-aligned position at or after the window-scan's
            # candidate. This can never land mid-instruction, no matter
            # how the parameter's value happens to look.
            TWO_WORD_OPCODES = {0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x09, 0x0A, 0x10, 0x0C, 0x0D}
            aligned = 0
            while aligned < real_start_word:
                b = pe.read_bytes(base_rva + aligned * 2, 2)
                if len(b) < 2:
                    break
                lo, hi = b[0], b[1]
                if hi == 0 and lo == 0x00:
                    # only meaningful paired with hi!=0 (the 0x0100 aux
                    # marker checked below); a lone (0,0) would be the
                    # end-of-array terminator, shouldn't occur mid-scan
                    aligned += 1
                elif hi != 0:
                    # 0x0100 (aux-table jump): consumes one more word
                    # (the table index) unconditionally, same as the
                    # other two-word opcodes above.
                    aligned += 2
                elif lo in TWO_WORD_OPCODES:
                    aligned += 2
                else:
                    aligned += 1
            real_start_word = aligned

            header_words = []
            for w in range(real_start_word):
                try:
                    b = pe.read_bytes(base_rva + w * 2, 2)
                    # Full little-endian 16-bit word, NOT just the low
                    # byte -- see docstring above for why this matters.
                    word = b[0] | (b[1] << 8)
                    header_words.append(word)
                except ValueError:
                    break
            text_rva = base_rva + real_start_word * 2
            decoded = decode_word_array(pe, text_rva, entry_index)
            if not decoded:
                # The "window" that triggered here sits right at/after
                # the array's true terminator -- zero real characters
                # of text remain. This happens on small non-dialogue
                # entries (numeric/flag data, not text at all) where a
                # coincidental kana-range byte briefly looked like a
                # real signal. Keep scanning forward instead of
                # returning a useless empty entry -- if nothing better
                # turns up, we fall through to the "not real text"
                # return at the end of the function.
                continue
            return header_words, decoded
    return [], ""


def parse_pairs_file(path):
    triples = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split('\t')
            if len(parts) != 3:
                continue
            idx_s, dest_s, src_s = parts
            try:
                idx = int(idx_s)
                dest_rva = int(dest_s, 16)
                src_rva = int(src_s, 16)
            except ValueError:
                continue
            triples.append((idx, dest_rva, src_rva))
    return triples


def validate_newbox_newline(json_path):
    """Scans an already-built/translated GameTextJP.json for entries
    where "[DrawClosingMark]" isn't immediately followed by "\\n"/"[NewLine]" --
    the mistake that causes a translated box to render with a
    warped/shifted left margin (see the big comment on the 0xCA handler
    in decode_word_array() above for why). Returns True if everything
    looks OK, False if it found at least one suspicious entry (also
    prints details either way).

    Every control code that can appear between [DrawClosingMark] and the next
    real text has now been decoded (see [RepeatBlankTile], idx=4, and
    [WaitForConfirm], idx=6 -- the last two unknowns in the common
    mid-dialogue footer pattern), and NEITHER touches the margin
    anchor, so there's no longer an "unconfirmed, might be a legitimate
    reset mechanism" bucket -- every flagged [DrawClosingMark] here is a real
    candidate for the margin bug.
    """
    with open(json_path, encoding='utf-8') as f:
        data = json.load(f)

    problems = []
    for key, entry in data.items():
        text = entry.get('text', '')
        idx = 0
        while True:
            idx = text.find('[DrawClosingMark]', idx)
            if idx == -1:
                break
            after = idx + len('[DrawClosingMark]')
            rest = text[after:]
            # Strip every bracket tag (e.g. "[WaitFrames][1E]", "[08]",
            # "[PacingTick][04]") to see if anything besides control
            # tags remains. If nothing does, this [DrawClosingMark] is the
            # closing footer at the very end of the entry, not the
            # start of a new box of visible text -- not a problem
            # either way.
            rest_no_tags = re.sub(r'\[[^\]]*\]', '', rest)
            if rest_no_tags.strip() == '':
                pass
            elif rest.startswith('\n') or rest.startswith('[NewLine]'):
                pass
            else:
                snippet = rest[:24].replace('\n', '\\n')
                problems.append((key, idx, snippet))
            idx = after

    if problems:
        print("ATENCAO: %d [DrawClosingMark] sem reset de margem antes do texto novo:" % len(problems))
        for key, pos, snippet in problems:
            print('  entrada "%s" (posicao %d): "...[DrawClosingMark]%s..."' % (key, pos, snippet))
        return False
    print("OK: todo [DrawClosingMark] seguido de texto tem reset de margem antes.")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe', nargs='?', help='Caminho para MMLC2.exe')
    ap.add_argument('pairs_file', nargs='?', help='Saida do find_jp_text_sources.py (index/dest_RVA/source_RVA)')
    ap.add_argument('--only', help='Lista de indices separados por virgula para incluir (default: todos)')
    ap.add_argument('--out', default='GameTextJP.json', help='Arquivo de saida')
    ap.add_argument('--validate', metavar='JSON_FILE',
                     help='Nao gera nada -- so verifica um GameTextJP.json ja existente (ex: depois de '
                          'traduzir) em busca de [DrawClosingMark] sem "\\n" logo depois. Ignora exe/pairs_file.')
    args = ap.parse_args()

    if args.validate:
        ok = validate_newbox_newline(args.validate)
        sys.exit(0 if ok else 1)

    if not args.exe or not args.pairs_file:
        ap.error("exe e pairs_file sao obrigatorios (a menos que use --validate)")

    only = None
    if args.only:
        only = set(int(x) for x in args.only.split(','))

    pe = PEImage(args.exe)
    triples = parse_pairs_file(args.pairs_file)

    out = {}
    count = 0
    for idx, dest_rva, src_rva in triples:
        if only is not None and idx not in only:
            continue
        try:
            triple_bytes = pe.read_bytes(src_rva, 16)
        except ValueError as e:
            print("indice %d: erro lendo source_rva 0x%x -- %s" % (idx, src_rva, e))
            continue
        text_va = struct.unpack_from('<Q', triple_bytes, 0)[0]
        if text_va < pe.image_base:
            print("indice %d: ponteiro de texto invalido" % idx)
            continue
        text_rva = pe.va_to_rva(text_va)

        header_words, decoded_text = find_header_and_text(pe, text_rva, idx)

        # Human-readable version of header_bytes, decoded with the exact
        # same control-code logic used for "text" -- informational only,
        # NOT used for patching (JapaneseText.cpp still reads the raw
        # header_bytes array). Safe to ignore/delete; regenerated fresh
        # every extraction. Exists because the raw number list is hard
        # to read by eye (e.g. telling "SetSpeaker?(32)" from
        # "SetTypeSound?(32)" at a glance is not obvious from "1, 32"
        # vs "16, 32" alone).
        #
        # BUG FIX (found by manually tracing entry 160's supposedly
        # "unresolved" aux-table jump -- it actually resolves fine):
        # this used to run decode_word_array() against a WordListReader
        # adapter wrapping ONLY the header words, disconnected from the
        # real exe. That's fine for every control code except the aux-
        # table mechanism (0x0100), which needs to read the REAL
        # auxiliary pointer table at a REAL file address -- something a
        # fake in-memory reader can never do, so it always silently
        # "failed" and printed [AUX_TABLE_JUMP:UNRESOLVED] whenever a
        # header happened to use it, even though the real jump resolves
        # correctly. Fixed by decoding directly against the real `pe`
        # at the header's own real starting address (text_rva), just
        # capped to the header's word count via max_chars -- exactly
        # what happens for "text" itself, just a different slice.
        header_decoded = decode_word_array(
            pe, text_rva, idx, max_chars=len(header_words))

        # dest_rva here points at the FIRST field of the source triple's
        # own destination slot -- i.e. exactly what needs to be
        # overwritten at runtime (matches what JapaneseText.cpp expects).
        out[str(idx)] = {
            "dest_rva": hex(dest_rva),
            # JSON key kept as "header_bytes" for compatibility with
            # JapaneseText.cpp's existing field name, but values are now
            # full 16-bit words (0-65535), not single bytes -- see
            # find_header_and_text()'s docstring.
            "header_bytes": header_words,
            "header_decoded": header_decoded,  # informational only -- see comment above, don't edit/rely on for patching
            "text": decoded_text,  # starts identical to original -- EDIT THIS to translate
        }
        count += 1

    json_text = json.dumps(out, ensure_ascii=False, indent=2)

    # Collapse "header_bytes": [ ... ] onto a single line -- with the
    # default multi-line indent=2 formatting, a 30+ word header spans
    # 30+ separate lines, which is painful to scroll past when reading
    # the JSON by eye. This only affects "header_bytes" (matched by the
    # literal key name) -- "text" and "header_decoded" strings, and any
    # other formatting, are untouched. Purely cosmetic; doesn't change
    # what JapaneseText.cpp reads (still the same numbers, same order).
    def collapse_header_bytes(match):
        numbers = re.findall(r'-?\d+', match.group(1))
        return '"header_bytes": [%s]' % ', '.join(numbers)

    json_text = re.sub(r'"header_bytes": \[([\s\S]*?)\]', collapse_header_bytes, json_text)

    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(json_text)

    print("Escrito %s com %d entradas." % (args.out, count))
    print("Edite o campo 'text' de cada entrada para traduzir.")
    print("NAO edite 'header_bytes' -- precisam ser preservados exatamente como estao.")
    print("'header_decoded' e so pra leitura -- gerado de novo toda extracao, JapaneseText.cpp nao le esse campo.")


if __name__ == '__main__':
    main()
