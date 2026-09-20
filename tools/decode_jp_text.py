#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_jp_text.py
==================

Companion to find_jp_text_sources.py. Runs OUTSIDE Ghidra, directly against
the raw MMLC2.exe, so you don't need Ghidra open to decode a batch of
candidate Japanese-text source RVAs.

WHAT IT EXPECTS
---------------
Each "source RVA" points at a 16-byte triple confirmed by live debugging:

    offset +0x00: 8 bytes -- a virtual address (little-endian) pointing at
                              the actual text. IMPORTANT: the text does NOT
                              start at byte 0 of this pointer -- there's a
                              variable-length run of small "header" words
                              (indices/counts, not character codes) first.
                              This script scans forward for the first spot
                              where several consecutive words all decode as
                              plausible characters, and starts reading the
                              actual line from there.
                              Once past the header: a flat array of "words"
                              (2 bytes per character: low byte = the game's
                              1-byte font/codepage index, high byte = 0x00),
                              null-terminated (lo byte == 0).
    offset +0x08: 8 bytes -- a count field (role not 100% pinned down yet --
                              looked like it might be a char count, but
                              didn't match the visible string length in our
                              samples, so this script does NOT rely on it;
                              it decodes until a null low-byte or a
                              non-zero high-byte, same as the game's own
                              reader would).
    offset +0x10: 8 bytes -- always seen as 0 so far.

WHERE TO GET SOURCE RVAs
-------------------------
Run find_jp_text_sources.py inside Ghidra (Script Manager) with MMLC2.exe
loaded. It prints a table of (index, dest_RVA, source_RVA) triples found
inside UndefinedFunction_1400017b0.

USAGE
-----
    # quick text-mode check, a few RVAs by hand:
    python decode_jp_text.py MMLC2.exe 0x463a00 0x464468 0x4645e0

    # from a plain file, one hex RVA per line:
    python decode_jp_text.py MMLC2.exe --file rvas.txt

    # from the exact tab-separated output pasted from
    # find_jp_text_sources.py (index / dest_RVA / source_RVA per line;
    # header line and blank lines are skipped automatically), producing a
    # JSON translation table keyed by index:
    python decode_jp_text.py MMLC2.exe --pairs-file ghidra_output.txt --json textos_japones.json

NOTE ON PE PARSING
-------------------
This script parses the PE section table itself (no external deps like
`pefile` required) to map RVA <-> file offset, and reads ImageBase from the
optional header so the text pointer (a full virtual address, e.g.
0x14057A5C0) can be converted back into an RVA.
"""

import argparse
import json
import struct
import sys


# Same codepage as CharEncoding.h / mm7_custom_strings.py. 0x90-0x9B use the
# game's ORIGINAL (unmodified) hiragana mapping, not the MM7Loc PT-BR
# override -- appropriate here since we're reading the raw, unpatched .exe.
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
    # Original (unmodified) hiragana row -- confirmed via CharEncoding.h's
    # own comment, and via live decoding ("i" of "isoide" needed 0x91).
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
    """Minimal PE32+ parser: just enough to map RVA <-> file offset."""

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
        if magic == 0x20b:  # PE32+
            self.image_base = struct.unpack_from('<Q', data, opt_header_off + 24)[0]
        elif magic == 0x10b:  # PE32
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
                'name': name,
                'virt_addr': virt_addr,
                'virt_size': virt_size,
                'raw_ptr': raw_ptr,
                'raw_size': raw_size,
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


def decode_word_array(pe, rva, max_chars=300):
    out = []
    cur = rva
    for _ in range(max_chars):
        try:
            b = pe.read_bytes(cur, 2)
        except ValueError:
            out.append('(fora dos limites do arquivo)')
            break
        if len(b) < 2:
            break
        lo, hi = b[0], b[1]
        if hi != 0:
            break
        if lo == 0:
            break
        out.append(BYTE_TO_CHAR.get(lo, '[%02X]' % lo))
        cur += 2
    return ''.join(out)


def is_valid_char_word(b):
    """b is a 2-byte little-endian slice. Returns True if it looks like a
    valid 'lo=code, hi=0x00' character word (lo must be a KNOWN character,
    not just any value < 0x100 -- avoids false positives on stray bytes)."""
    if len(b) < 2:
        return False
    lo, hi = b[0], b[1]
    return hi == 0 and lo != 0 and lo in BYTE_TO_CHAR


def is_strong_text_signal(b):
    """Stricter check used only for LOCATING where real text starts.
    Header/index words in these tables are always small numbers (< 0x80
    in every sample seen so far), while real dialogue is essentially
    always kana (>= 0x80). Requiring the kana range avoids false
    triggers on header values that coincidentally equal a valid ASCII
    character code (0x20 space being the most common offender)."""
    if len(b) < 2:
        return False
    lo, hi = b[0], b[1]
    return hi == 0 and lo >= 0x80 and lo in BYTE_TO_CHAR


def is_ascii_letter_word(b):
    """Stricter check for the ASCII fallback pass: only counts as a hit if
    lo is an actual LETTER (A-Z/a-z), not any ASCII value. A single
    isolated ASCII value (like 0x20 space, or a digit) is too easy to
    hit by coincidence in header/index data; requiring several
    consecutive *letters* is a much stronger signal that real English
    text -- not a stray header number -- is starting here."""
    if len(b) < 2:
        return False
    lo, hi = b[0], b[1]
    if hi != 0:
        return False
    return (0x41 <= lo <= 0x5A) or (0x61 <= lo <= 0x7A)


def find_ascii_run_start(pe, base_rva, max_header_words=160, min_run=4, max_chars=400):
    """Fallback for entries whose text is plain ASCII/English (no kana at
    all) -- the main kana-based search in find_text_start_and_decode()
    would never find these, since it specifically requires kana bytes as
    its 'strong signal' to avoid false positives on header data. This
    looks for a run of at least `min_run` CONSECUTIVE ascii-letter words,
    which is a much rarer coincidence in header/index data than a single
    matching byte."""
    for start_word in range(max_header_words):
        cur = base_rva + start_word * 2
        ok = True
        for i in range(min_run):
            try:
                b = pe.read_bytes(cur + i * 2, 2)
            except ValueError:
                ok = False
                break
            if not is_ascii_letter_word(b):
                ok = False
                break
        if ok:
            decoded = decode_word_array(pe, cur, max_chars)
            return start_word, decoded
    return None, ""


def find_text_start_and_decode(pe, base_rva, max_header_words=160, window=8, min_hits=3, max_chars=400):
    """The array at base_rva usually starts with a run of small
    'header' words (indices/counts/etc, NOT character codes) before the
    actual text begins. Scan forward word-by-word; at each candidate
    start position, look at the next `window` words and count how many
    look like valid characters. If at least `min_hits` do, treat this as
    the real text start and decode fully from there.

    NOTE: this is a "mostly valid in a window" check, not "N consecutive
    valid words" -- real dialogue can have a control/formatting code
    (not in our character table) within the first few characters (e.g.
    a combining-dakuten byte right after the 3rd character), which would
    break a strict consecutive-run check too early. The window approach
    tolerates that.

    If this kana-based search finds nothing, falls back to
    find_ascii_run_start() for the (presumably rarer) case of an entry
    that's plain English/ASCII with no kana in it at all -- MM7's
    Japanese-flagged resource pool can still contain some English-only
    strings (e.g. borrowed terms, or shared entries), and those would
    otherwise never be detected by the kana-only signal.

    Returns (header_words_skipped, decoded_string). header_words_skipped
    is None if no plausible text start was found at all.
    """
    for start_word in range(max_header_words):
        cur = base_rva + start_word * 2
        window_bytes = []
        for i in range(window):
            try:
                b = pe.read_bytes(cur + i * 2, 2)
            except ValueError:
                b = None
            window_bytes.append(b)

        hits = sum(1 for b in window_bytes if b is not None and is_strong_text_signal(b))
        if hits >= min_hits:
            # Found a promising window -- but start decoding from the
            # FIRST valid word inside it, not from the window's start
            # (which may still be sitting on trailing header bytes).
            first_valid_offset = None
            for i, b in enumerate(window_bytes):
                if b is not None and is_strong_text_signal(b):
                    first_valid_offset = i
                    break
            real_start_word = start_word + first_valid_offset
            real_cur = base_rva + real_start_word * 2
            decoded = decode_word_array(pe, real_cur, max_chars)
            return real_start_word, decoded

    # Kana-based search found nothing -- try the ASCII fallback before
    # giving up entirely.
    return find_ascii_run_start(pe, base_rva, max_header_words, max_chars=max_chars)


def parse_pairs_file(path):
    """Parses the exact tab-separated output of find_jp_text_sources.py:
    lines like '3\t0xe26bc8\t0x464b10'. Skips the header line and blanks.
    Returns a list of (index, dest_rva, source_rva) tuples."""
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
                continue  # header line ("index dest_RVA source_RVA") etc.
            triples.append((idx, dest_rva, src_rva))
    return triples


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe', help='Caminho para MMLC2.exe')
    ap.add_argument('rvas', nargs='*', help='RVAs (hex, ex: 0x463a00) dos triples a decodificar')
    ap.add_argument('--file', help='Arquivo com um RVA hex por linha (alternativa a passar na linha de comando)')
    ap.add_argument('--pairs-file', help='Saida colada do find_jp_text_sources.py (index/dest_RVA/source_RVA por linha)')
    ap.add_argument('--json', help='Se fornecido, tambem escreve uma tabela em JSON nesse caminho')
    args = ap.parse_args()

    # entries: list of (index_or_None, dest_rva_or_None, source_rva)
    entries = []
    if args.pairs_file:
        for idx, dest_rva, src_rva in parse_pairs_file(args.pairs_file):
            entries.append((idx, dest_rva, src_rva))
    for r in args.rvas:
        entries.append((None, None, int(r, 16)))
    if args.file:
        with open(args.file) as f:
            content = f.read()
        # Accepts either one RVA per line, or several space-separated on
        # the same line (or a mix of both) -- just tokenize on whitespace.
        # Tolerates non-hex tokens (e.g. if someone points --file at a
        # --pairs-file-style file with a header line) by skipping them
        # with a warning instead of crashing.
        for tok in content.split():
            tok = tok.strip()
            if not tok or tok.startswith('#'):
                continue
            try:
                entries.append((None, None, int(tok, 16)))
            except ValueError:
                print("Aviso: ignorando token nao-hex '%s' em --file" % tok, file=sys.stderr)

    if not entries:
        print("Nenhuma RVA fornecida. Use argumentos, --file, ou --pairs-file.", file=sys.stderr)
        sys.exit(1)

    pe = PEImage(args.exe)
    print("ImageBase: 0x%x\n" % pe.image_base)

    json_out = {}

    for i, (idx, dest_rva, src_rva) in enumerate(entries):
        key = str(idx) if idx is not None else str(i)
        try:
            triple = pe.read_bytes(src_rva, 16)
        except ValueError as e:
            print("0x%x: ERRO -- %s" % (src_rva, e))
            json_out[key] = {"source_rva": "0x%x" % src_rva, "error": str(e)}
            continue
        text_va, count = struct.unpack_from('<QQ', triple, 0)
        if text_va < pe.image_base:
            print("0x%x: ponteiro de texto invalido (0x%x < image base)" % (src_rva, text_va))
            json_out[key] = {"source_rva": "0x%x" % src_rva, "error": "ponteiro invalido"}
            continue
        text_rva = pe.va_to_rva(text_va)
        try:
            header_words, decoded = find_text_start_and_decode(pe, text_rva)
        except Exception as e:
            header_words, decoded = None, None

        print("source_RVA=0x%x  text_ptr_RVA=0x%x  count_field=0x%x" % (src_rva, text_rva, count))
        if header_words is None:
            print("  -> nao achei nenhum trecho decodificavel como texto por perto")
        else:
            print("  -> (pulou %d words de cabecalho) %s" % (
                header_words, decoded if decoded else "(vazio)"))
        print("")

        entry_json = {
            "source_rva": "0x%x" % src_rva,
            "text_ptr_rva": "0x%x" % text_rva,
            "count_field": "0x%x" % count,
            "header_words_skipped": header_words,
            "text": decoded if decoded is not None else None,
        }
        if dest_rva is not None:
            entry_json["dest_rva"] = "0x%x" % dest_rva
        json_out[key] = entry_json

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(json_out, f, ensure_ascii=False, indent=2, sort_keys=False)
        print("Tabela JSON escrita em: %s" % args.json)


if __name__ == '__main__':
    main()