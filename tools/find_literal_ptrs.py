#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
find_literal_ptrs.py
=====================

Standalone (no Ghidra needed) complement to find_jp_text_sources.py /
decode_jp_text.py. Scans the raw bytes of UndefinedFunction_1400017b0
(RVA 0x17b0-0x2bba, confirmed by the Ghidra script's own "body size"
report) for embedded 8-byte pointer immediates that point at a plain,
printable, null-terminated ASCII string somewhere else in the .exe --
catching literal assignments like

    _DAT_140e26b30 = &PTR_s_AND_CAPCOM_140577b28;

which don't fit the {text_ptr, count, 0} triple-copy pattern the other
two scripts look for, and so were invisible to them.

This works directly against the file, byte by byte (not just at 8-byte
aligned offsets), since an immediate pointer can start anywhere within a
variable-length x86 instruction.

USAGE
-----
    python find_literal_ptrs.py MMLC2.exe
    python find_literal_ptrs.py MMLC2.exe --start 0x17b0 --end 0x2bba
"""

import argparse
import struct
import sys


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


def try_read_ascii_string(pe, rva, min_len=4, max_len=200):
    """If there's a printable, null-terminated ASCII string starting
    exactly at rva, return it. Otherwise return None."""
    try:
        chunk = pe.read_bytes(rva, max_len)
    except ValueError:
        return None
    out = []
    for b in chunk:
        if b == 0:
            break
        # printable ASCII range, plus common punctuation already seen in
        # these credits/dialogue strings (comma, period, apostrophe, etc)
        if 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            return None  # non-printable byte before any null -- not a clean string
    else:
        return None  # never hit a null within max_len -- probably not a real string
    if len(out) < min_len:
        return None
    return ''.join(out)


def scan_for_literal_pointers(pe, func_start_rva, func_end_rva):
    """Two complementary scans:

    (a) Raw 8-byte absolute VA immediates -- works for data-to-data
        copies where a literal pointer sits directly in .rdata (this is
        what already found our {text_ptr, count, 0} dialogue triples in
        the other scripts).

    (b) x64 RIP-relative LEA instructions (`lea reg, [rip+disp32]`) --
        this is how compiled code usually references a global's address
        from within actual CODE (not data), e.g. to compute a pointer
        it's about to store somewhere. The address never appears as a
        raw 8-byte immediate in this case -- only a 4-byte signed
        displacement, resolved at RUNTIME relative to the address right
        after the instruction. This is the much more likely shape for a
        single literal pointer assignment like
        `_DAT_140e26b30 = &PTR_s_AND_CAPCOM_140577b28;`.
    """
    results = []

    # --- (a) raw 8-byte absolute VA scan ---
    for offset in range(func_start_rva, func_end_rva - 7):
        try:
            chunk = pe.read_bytes(offset, 8)
        except ValueError:
            continue
        if len(chunk) < 8:
            continue
        va = struct.unpack('<Q', chunk)[0]
        if va < pe.image_base:
            continue
        target_rva = pe.va_to_rva(va)
        s = try_read_ascii_string(pe, target_rva)
        if s is not None:
            results.append((offset, target_rva, s, 'abs64'))

    # --- (b) RIP-relative LEA scan ---
    # Opcode shape: [REX prefix 0x40-0x4F] 0x8D [ModRM with mod=00,rm=101]
    # ModRM byte matches this pattern when (byte & 0xC7) == 0x05 --
    # the reg field (bits 3-5) varies with which register is the
    # destination, but mod/rm bits identify RIP-relative addressing.
    try:
        func_bytes = pe.read_bytes(func_start_rva, func_end_rva - func_start_rva)
    except ValueError:
        func_bytes = b''
    n = len(func_bytes)
    for i in range(n - 6):
        b0 = func_bytes[i]
        if not (0x40 <= b0 <= 0x4F):
            continue
        if func_bytes[i + 1] != 0x8D:
            continue
        modrm = func_bytes[i + 2]
        if (modrm & 0xC7) != 0x05:
            continue
        disp32 = struct.unpack_from('<i', func_bytes, i + 3)[0]
        instr_rva = func_start_rva + i
        instr_len = 7  # REX + opcode + modrm + disp32
        next_instr_rva = instr_rva + instr_len
        target_va = pe.image_base + next_instr_rva + disp32
        if target_va < pe.image_base:
            continue
        target_rva = pe.va_to_rva(target_va)
        s = try_read_ascii_string(pe, target_rva)
        if s is not None:
            results.append((instr_rva, target_rva, s, 'lea_rip'))

    # de-duplicate (both scans can legitimately catch the same hit)
    seen = set()
    dedup = []
    for offset, target_rva, s, kind in results:
        key = (target_rva, s)
        if key in seen:
            continue
        seen.add(key)
        dedup.append((offset, target_rva, s, kind))
    return dedup


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe', help='Caminho para MMLC2.exe')
    ap.add_argument('--start', default='0x17b0', help='RVA inicial da funcao (default: 0x17b0)')
    ap.add_argument('--end', default='0x2bba', help='RVA final da funcao (default: 0x2bba, do proprio Ghidra)')
    args = ap.parse_args()

    func_start_rva = int(args.start, 16)
    func_end_rva = int(args.end, 16)

    pe = PEImage(args.exe)
    print("ImageBase: 0x%x" % pe.image_base)
    print("Escaneando RVA 0x%x - 0x%x (%d bytes)\n" % (
        func_start_rva, func_end_rva, func_end_rva - func_start_rva))

    results = scan_for_literal_pointers(pe, func_start_rva, func_end_rva)

    print("instr_offset_RVA\ttipo\tstring_RVA\tvalor")
    for offset, target_rva, s, kind in results:
        print("0x%x\t%s\t0x%x\t%s" % (offset, kind, target_rva, s))

    print("\nTotal encontrado: %d" % len(results))


if __name__ == '__main__':
    main()