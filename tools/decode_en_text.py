#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_en_text_v2.py
======================

English dialogue extractor, take 2. The first version (decode_en_text.py)
assumed the StringEntry table at RVA_TABLE_START=0xE27B50 exists as static
data in the .exe -- it doesn't; that table is only populated once the game
actually runs (RVA_INIT_STRINGS copies into it at startup). Reading the raw
file at that RVA just finds zeroed/unmapped space, which is why v1 crashed.

This version fixes that by working with what's ACTUALLY static: the `lines`
arrays themselves (array of char* pointers, one per dialogue entry) live in
.rdata as real, permanent data -- RVA_INIT_STRINGS just copies references TO
them into the runtime table, it doesn't invent them. A prior investigation
session already collected the RVAs of every known `lines` array start into
known_array_starts.h (123 addresses + 1 manually-added boundary marking the
start of an unrelated credits block) specifically to solve the "where does
this entry's line list actually end" problem -- which otherwise causes text
to visibly leak from one dialogue entry into the next whenever a naive
"does the next 8 bytes look like a valid pointer" heuristic guesses wrong
(documented at length in that session's README, section 2.2).

THE KEY TRICK (from that README, worth repeating here since it's easy to
get backwards): when deciding whether the current line-pointer SLOT is
actually the start of the NEXT entry's array, compare the slot's own
ADDRESS against the known boundary list -- not the VALUE stored in that
slot. Two entries can sit back-to-back with zero gap, so slot i of entry A
and slot 0 of entry B occupy the same address; but the VALUE there is a
pointer to entry B's first LINE OF TEXT, not to entry B's array -- so
comparing values never matches, while comparing addresses reliably does.

USAGE
-----
    python decode_en_text_v2.py MMLC2.exe known_array_starts.h --json textos_ingles.json
"""

import argparse
import json
import re
import struct
import sys


# Same shared codepage used throughout this project.
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
}


class PEImage(object):
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        self._parse()

    def _parse(self):
        data = self.data
        if data[0:2] != b'MZ':
            raise ValueError("Nao parece ser um PE valido (assinatura MZ ausente).")
        e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
        if data[e_lfanew:e_lfanew + 4] != b'PE\x00\x00':
            raise ValueError("Assinatura PE nao encontrada no offset esperado.")
        coff_off = e_lfanew + 4
        machine, num_sections = struct.unpack_from('<HH', data, coff_off)
        size_opt_header = struct.unpack_from('<H', data, coff_off + 16)[0]
        opt_header_off = coff_off + 20
        magic = struct.unpack_from('<H', data, opt_header_off)[0]
        if magic == 0x20b:
            self.image_base = struct.unpack_from('<Q', data, opt_header_off + 24)[0]
        elif magic == 0x10b:
            self.image_base = struct.unpack_from('<I', data, opt_header_off + 28)[0]
        else:
            raise ValueError("Magic de optional header desconhecido: 0x%x" % magic)
        section_table_off = opt_header_off + size_opt_header
        self.sections = []
        for i in range(num_sections):
            off = section_table_off + i * 40
            name = data[off:off + 8].rstrip(b'\x00').decode('ascii', 'replace')
            virt_size, virt_addr, raw_size, raw_ptr = struct.unpack_from('<IIII', data, off + 8)
            self.sections.append({
                'name': name, 'virt_addr': virt_addr, 'virt_size': virt_size,
                'raw_ptr': raw_ptr, 'raw_size': raw_size,
            })

    def rva_to_offset(self, rva):
        for s in self.sections:
            if s['virt_addr'] <= rva < s['virt_addr'] + max(s['virt_size'], s['raw_size']):
                return s['raw_ptr'] + (rva - s['virt_addr'])
        raise ValueError("RVA 0x%x nao cai em nenhuma secao mapeada." % rva)

    def read_bytes(self, rva, n):
        off = self.rva_to_offset(rva)
        return self.data[off:off + n]

    def va_to_rva(self, va):
        return va - self.image_base


def parse_known_array_starts(path):
    """Extracts every 0x...u hex literal from the header file, IN THE
    ORDER THEY APPEAR in the file, WITHOUT deduplicating.

    IMPORTANT: deduplication used to happen here, but that's actively
    WRONG for the current known_array_starts_ordered.h /
    known_ctx_starts_ordered.h pair -- both are generated with one
    entry per real table block (171 total), in the SAME block order,
    and are meant to be read positionally (index i in one file
    corresponds to index i in the other, via main()'s enumerate()).
    Deduplicating each file independently (which happens naturally
    since many entries legitimately share the same text or the same
    boilerplate ctx header -- e.g. "THE WILD COIL LAUNCHES..." appears
    for 4 different in-game moments) removes a DIFFERENT number of
    entries from each file, breaking that positional correspondence.
    Kept duplicates and all."""
    content = open(path, encoding='utf-8', errors='replace').read()
    vals = re.findall(r'0x([0-9A-Fa-f]+)u', content)
    return [int(v, 16) for v in vals]


# CONFIRMED (decompile of LAB_140056260, the shared word/byte dispatcher
# both languages' dialogue boxes are initialized with -- see
# MM7Loc_INVESTIGATION_SUMMARY.md section 4.0/investigation chat for the
# full finding): the same function that reads the Japanese 16-bit word
# array ALSO reads English's plain char* text, one byte at a time
# (`uVar4 = (ushort)*pcVar3`), and applies the EXACT SAME `> 0x15 ==
# control code` rule to both. Plain ASCII letters are always >0x15, so
# this was invisible until now -- but a byte in this range inside an
# English string should fire the identical control codes already
# decoded for Japanese in build_japanese_strings_json.py.
#
# KEY STRUCTURAL DIFFERENCE FROM JAPANESE, NOT YET CONFIRMED LIVE: the
# Japanese array is 16-bit WORDS (every value, including control
# opcodes and their parameters, is 2 bytes with a zero high byte for
# small values). English's char* is 1 BYTE per unit. It's unconfirmed
# whether opcode PARAMETERS (e.g. [WaitFrames]'s frame count) are also
# read as a single byte here, or whether the individual opcode handler
# functions (FUN_140056xxx) fall back to reading a full 16-bit word
# regardless of source -- decoded here assuming single-byte parameters,
# the simpler/more consistent assumption, but this needs an in-game
# test (insert a control code into an English line, observe behavior)
# to confirm before relying on it for a real translation.
EN_CONTROL_PARAMS = {
    0x01: '[SetSpeaker?]', 0x02: '[SetTypeSpeed]', 0x03: '[SetInitialTypeSpeed?]',
    0x04: '[RepeatBlankTile]', 0x05: '[PacingTick]', 0x07: '[WaitFrames]',
    0x09: '[TabToColumn]', 0x0A: '[SetLeftMargin]', 0x10: '[SetTypeSound?]',
    0x11: '[PlaySound]', 0x12: '[ConditionalTrigger?]',
}
EN_CONTROL_NOPARAM = {
    0x06: '[WaitForConfirm]', 0x0B: '[IncrementCounter]', 0x0E: '[ClearForceWait]',
    0x0F: '[ForceWait]', 0x13: '[ClearIconList?]', 0x14: '[ClearIconList?]',
    0x15: '[ResetToMarginSameLine]',
}
# 0x08 needs lookahead (paired = [NewLine], lone = [AdvanceCursorLine]),
# same as the Japanese decoder -- handled inline in decode_cstring below
# rather than in these two flat dicts.
# 0x0C/0x0D (dakuten/han-dakuten) are NOT included here: that mechanism
# is bound to the Japanese glyph table specifically (combines with a
# following KANA byte) and has no clear equivalent for a Latin string --
# left as a raw [0C]/[0D] fallback token if ever encountered, rather
# than guessing.


def decode_cstring(pe, rva, max_chars=500):
    """Raises ValueError if the read goes out of any mapped section --
    the caller treats that as 'definitely not real text, stop here',
    same as a null/out-of-range pointer. (Earlier version of this
    function silently embedded a placeholder string on failure instead
    of raising, which let garbage slip past the plausibility filter --
    fixed here.)

    UPDATED: now also recognizes bytes <=0x15 as the SAME shared
    control codes already decoded for Japanese (see EN_CONTROL_PARAMS/
    EN_CONTROL_NOPARAM above and their big comment for the full
    reasoning and the open question about parameter width).
    """
    out = []
    cur = rva
    i = 0
    while i < max_chars:
        b = pe.read_bytes(cur, 1)[0]  # let ValueError propagate
        if b == 0:
            break
        if b == 0x08:
            # Lookahead for the pair (mirrors the Japanese decoder's
            # [NewLine] vs [AdvanceCursorLine] distinction) -- assumes
            # a single lookahead BYTE here, same open caveat as the
            # param-width question above.
            nxt = pe.read_bytes(cur + 1, 1)[0]
            if nxt == 0x08:
                out.append('[NewLine]')
                cur += 2
                i += 2
                continue
            out.append('[AdvanceCursorLine]')
            cur += 1
            i += 1
            continue
        if b in EN_CONTROL_PARAMS:
            param = pe.read_bytes(cur + 1, 1)[0]
            out.append('%s[%02X]' % (EN_CONTROL_PARAMS[b], param))
            cur += 2
            i += 2
            continue
        if b in EN_CONTROL_NOPARAM:
            out.append(EN_CONTROL_NOPARAM[b])
            cur += 1
            i += 1
            continue
        if b <= 0x15:
            # Recognized as "this is a control code" by the shared
            # dispatcher (b <= 0x15) but not one of the specific
            # opcodes mapped above -- raw fallback, same convention as
            # the Japanese decoder's unrecognized-code tokens.
            out.append('[%02X]' % b)
            cur += 1
            i += 1
            continue
        out.append(BYTE_TO_CHAR.get(b, '[%02X]' % b))
        cur += 1
        i += 1
    return ''.join(out)


def looks_like_garbage(decoded_line):
    """Rough plausibility check for a decoded line -- if it's mostly
    bracketed [XX] control tokens (or empty/huge), it's very unlikely to
    be real dialogue text and more likely we've wandered into unrelated
    static data (another table, padding) that happened to contain a
    byte sequence resolving to a technically-valid-looking pointer.
    This is heuristic #3 from the README, used here as a SECOND check
    alongside the address-boundary one -- the boundary list alone isn't
    enough, since gaps between known entries can contain unmapped
    (not-yet-catalogued) structures the boundary list doesn't know
    about.

    NOTE: now that recognized control tags (e.g. "[NewLine]",
    "[WaitFrames][XX]") are expected, legitimate real-world lines with
    a control code or two will have MORE bracket characters than
    before. This heuristic's 0.4 ratio threshold was tuned before that
    was true; a line with several real control codes plus normal text
    could conceivably trip it. Not adjusted here since no real example
    has been observed yet -- revisit if extraction starts dropping
    lines that turn out to have real control codes in them.
    """
    if not decoded_line:
        return True
    bracket_chars = decoded_line.count('[')
    approx_tokens = bracket_chars  # each "[XX]" token contributes one '['
    total_len = len(decoded_line)
    if total_len == 0:
        return True
    if (approx_tokens * 4) / float(total_len) > 0.4:
        return True
    if total_len > 120:
        return True
    return False


def read_entry_text(pe, array_rva, other_starts=None):
    """FINAL CORRECTED MODEL (confirmed live against the real .exe,
    after two wrong guesses -- see git history / chat transcript for
    the full trail): array_rva IS the start of an array of char*
    pointers after all -- the ORIGINAL model this script started with.
    What was wrong was WHICH address to feed it: not the StringEntry's
    first field (that's "ctx", a control header -- see the
    investigation notes), and not a further-resolved single string
    either (that only gets you line 1). It's the entry's THIRD field's
    OWN address, used directly as the array start, with NO extra
    resolve step first. Confirmed live: entry 0's third field, read as
    an array starting at its own address, gives "AND CAPCOM", then
    "ALL STAFF", then "...", then an invalid/garbage pointer -- three
    real lines matching the already-known Japanese-side credits content
    exactly, then a natural array-end.

    BUG FIX (see investigation summary 8.4): unlike Japanese, these
    pointer arrays have NO terminator of their own in memory -- reading
    stops only on an invalid pointer or implausible content
    (looks_like_garbage), which fails whenever the NEXT entry's own
    array happens to sit immediately adjacent with equally plausible
    content (confirmed for >60 entries via a systematic scan -- see
    chat history). `other_starts`, if given, is the set of every OTHER
    entry's own array_rva (excluding this one) -- if the read is about
    to consume a pointer sitting AT one of those addresses, stop before
    reading it, since that word is known to belong to a different
    entry's own array. This only catches bleeds that land EXACTLY on
    another entry's registered start (confirmed to catch ~35 cases);
    it does NOT catch bleeding into unregistered/uncatalogued memory
    (e.g. entry 3's "ROCK MAN 7" case, which isn't any known entry's
    own start) -- that class of bleed is not fixed by this check.
    """
    lines = []
    slot_rva = array_rva
    for _ in range(50):
        if other_starts is not None and slot_rva in other_starts:
            break
        try:
            ptr_bytes = pe.read_bytes(slot_rva, 8)
        except ValueError:
            break
        if len(ptr_bytes) < 8:
            break
        line_va = struct.unpack('<Q', ptr_bytes)[0]
        if line_va < pe.image_base:
            break
        try:
            line_rva = pe.va_to_rva(line_va)
            decoded = decode_cstring(pe, line_rva)
        except ValueError:
            break
        if looks_like_garbage(decoded):
            break
        lines.append(decoded)
        slot_rva += 8
    return '[NewLine]'.join(lines)


def read_lines_until_boundary(pe, array_rva, next_boundary_rva, max_lines=200):
    """Reads char* pointers sequentially starting at array_rva, stopping
    at the FIRST of:
      (a) the slot's own ADDRESS reaching next_boundary_rva (the
          address-comparison trick from the README -- comparing
          addresses, not pointer values, since zero-gap adjacent arrays
          make value-comparison useless), or
      (b) a null/out-of-range pointer, or
      (c) content that doesn't look like plausible text (heuristic #3;
          needed because gaps between known boundaries can contain
          unmapped/uncatalogued structures the boundary list alone
          can't detect -- see README section 2.2's own caveat that its
          "working mitigation" is NOT a full fix).
    """
    lines = []
    slot_rva = array_rva
    for _ in range(max_lines):
        if next_boundary_rva is not None and slot_rva >= next_boundary_rva:
            break
        try:
            ptr_bytes = pe.read_bytes(slot_rva, 8)
        except ValueError:
            break
        if len(ptr_bytes) < 8:
            break
        line_va = struct.unpack('<Q', ptr_bytes)[0]
        if line_va == 0:
            break
        if line_va < pe.image_base:
            break
        try:
            line_rva = pe.va_to_rva(line_va)
            decoded = decode_cstring(pe, line_rva)
        except ValueError:
            break
        if looks_like_garbage(decoded):
            break
        lines.append(decoded)
        slot_rva += 8
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe', help='Caminho para MMLC2.exe')
    ap.add_argument('known_starts', help='Caminho para known_array_starts_ordered.h (campo 2, texto real)')
    ap.add_argument('--ctx-starts', help='Caminho para known_ctx_starts_ordered.h (campo 0, mini-header de controle) -- '
                                          'mesma ordem de indice que known_starts. Se fornecido, adiciona "header_decoded" '
                                          'a cada entrada da saida, igual ao GameTextJP.json.')
    ap.add_argument('--json', help='Se fornecido, escreve uma tabela em JSON nesse caminho')
    args = ap.parse_args()

    pe = PEImage(args.exe)
    boundaries = parse_known_array_starts(args.known_starts)
    print("ImageBase: 0x%x" % pe.image_base)
    print("%d enderecos carregados de %s (ordem original do arquivo)\n"
          % (len(boundaries), args.known_starts))

    ctx_boundaries = None
    if args.ctx_starts:
        ctx_boundaries = parse_known_array_starts(args.ctx_starts)
        if len(ctx_boundaries) != len(boundaries):
            print("AVISO: known_starts tem %d enderecos, mas --ctx-starts tem %d "
                  "-- os indices podem nao bater certinho." % (len(boundaries), len(ctx_boundaries)))

    # BUG FIX (see investigation summary 8.4 and read_entry_text's own
    # docstring): the set of every known array-start address, used to
    # stop a read early if it's about to cross into another entry's
    # own registered array. Only catches bleeds landing exactly on a
    # KNOWN start (confirmed ~35 cases) -- bleeding into unregistered/
    # uncatalogued memory (e.g. entry 3's "ROCK MAN 7") is a separate,
    # still-open class of the same problem, not fixed by this.
    all_starts = set(b for b in boundaries if b != 0)

    # Output format matches GameTextJP.json's per-entry shape (see
    # build_japanese_strings_json.py): sequential numeric keys ("0",
    # "1", "2"...), one "text" string per entry, plus (if --ctx-starts
    # was given) "header_decoded" -- the entry's own "ctx" mini-header
    # (field 0 of the StringEntry-style block, confirmed to hold the
    # exact same shared control codes documented for Japanese, e.g.
    # "[SetTypeSpeed][00][SetTypeSound?][00][SetInitialTypeSpeed?][00]").
    # This is READ DIRECTLY via decode_cstring (single string, no
    # array/multi-line indirection -- ctx is always short, unlike the
    # real dialogue text in field 2). "array_rva" is kept per entry
    # (the English-side equivalent of "dest_rva").
    json_out = {}
    for i, start_rva in enumerate(boundaries):
        other_starts = all_starts - {start_rva}
        text = read_entry_text(pe, start_rva, other_starts)
        entry = {
            "array_rva": hex(start_rva),
        }
        if ctx_boundaries is not None and i < len(ctx_boundaries):
            ctx_rva = ctx_boundaries[i]
            try:
                header_decoded = decode_cstring(pe, ctx_rva)
            except ValueError:
                header_decoded = ''
            entry["header_decoded"] = header_decoded
        entry["text"] = text

        print("[Entrada @ 0x%x] (indice %d)" % (start_rva, i))
        if "header_decoded" in entry:
            print("  header: %s" % entry["header_decoded"])
        print("  text:   %s" % text)
        print("")

        json_out[str(i)] = entry

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(json_out, f, ensure_ascii=False, indent=2, sort_keys=False)
        print("Tabela JSON escrita em: %s" % args.json)


if __name__ == '__main__':
    main()
