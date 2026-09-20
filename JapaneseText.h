// JapaneseText.h
// -----------------------------------------------------------------------
// Patches the Japanese "word-array" dialogue text (2 bytes/char: low byte
// = font/codepage index, high byte = 0x00), as opposed to EnglishText.h's
// simple char* system used for English.
//
// BACKGROUND (see README_investigacao_texto_japones.md for the full
// reverse-engineering trail):
//   - UndefinedFunction_1400017b0 (RVA 0x17b0) populates a "staging" table
//     (destinations like DAT_140e26bXX / DAT_140e27bXX) by copying 24-byte
//     {text_ptr, count, 0} triples from scattered static source locations
//     in .rdata.
//   - FUN_140056360 (the character-reader) uses [RCX+0x18] -- which,
//     after one level of dereference, resolves to that copied text_ptr --
//     to read the actual word-array, indexed by a running R8 counter.
//   - Critically, R8 does NOT start at 0: there's a variable-length
//     "header" of small numeric words BEFORE the real glyph data starts
//     (purpose still unconfirmed -- possibly per-line offsets, possibly
//     something else entirely). To avoid breaking whatever consumes that
//     header, this patcher PRESERVES the original entry's header bytes
//     verbatim and only replaces the text AFTER it.
//
// HOOK STRATEGY
//   Hook UndefinedFunction_1400017b0 itself via MinHook, same pattern as
//   RVA_INIT_STRINGS in EnglishText.cpp: call the original first (so the
//   game populates the staging table normally), then call
//   PatchJapaneseStrings() to overwrite specific destination slots.
// -----------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// ---------------------------------------------------------------------
// Character encoding: Unicode codepoint -> 1-byte game font/codepage
// index. This is the INVERSE of the table used to decode/display text
// (EnglishCharTable.h's g_charToByte, and the Python scripts' BYTE_TO_CHAR).
// Keep in sync with those if the font mapping ever changes.
//
// Includes ASCII, the accented Latin overrides (for PT-BR translations),
// AND the full kana table (needed so an UNTRANSLATED entry -- original
// Japanese text copied straight through -- round-trips correctly instead
// of every kana character falling back to '?'). Any character genuinely
// not found here still falls back to '?' (0x3F), with a debug log so
// it's easy to spot and fix.
// ---------------------------------------------------------------------
inline const std::unordered_map<char32_t, uint8_t>& GetCharToByteTable() {
    static const std::unordered_map<char32_t, uint8_t> table = {
        {U' ', 0x20}, {U'!', 0x21}, {U'\u201D', 0x22}, {U'#', 0x23}, {U'\u25BC', 0x25},
        {U'\'', 0x27}, {U'(', 0x28}, {U')', 0x29}, {U'*', 0x2A}, {U'\u25AE', 0x2B},
        {U',', 0x2C}, {U'-', 0x2D}, {U'.', 0x2E}, {U'/', 0x2F},
        {U'0', 0x30}, {U'1', 0x31}, {U'2', 0x32}, {U'3', 0x33}, {U'4', 0x34},
        {U'5', 0x35}, {U'6', 0x36}, {U'7', 0x37}, {U'8', 0x38}, {U'9', 0x39},
        {U':', 0x3A}, {U';', 0x3B}, {U'<', 0x3C}, {U'=', 0x3D}, {U'>', 0x3E}, {U'?', 0x3F},
        {U'\u00A9', 0x40},
        {U'A', 0x41}, {U'B', 0x42}, {U'C', 0x43}, {U'D', 0x44}, {U'E', 0x45},
        {U'F', 0x46}, {U'G', 0x47}, {U'H', 0x48}, {U'I', 0x49}, {U'J', 0x4A},
        {U'K', 0x4B}, {U'L', 0x4C}, {U'M', 0x4D}, {U'N', 0x4E}, {U'O', 0x4F},
        {U'P', 0x50}, {U'Q', 0x51}, {U'R', 0x52}, {U'S', 0x53}, {U'T', 0x54},
        {U'U', 0x55}, {U'V', 0x56}, {U'W', 0x57}, {U'X', 0x58}, {U'Y', 0x59}, {U'Z', 0x5A},
        {U'[', 0x5B}, {U'\u00A5', 0x5C}, {U']', 0x5D}, {U'^', 0x5E}, {U'_', 0x5F}, {U'`', 0x60},
        {U'a', 0x61}, {U'b', 0x62}, {U'c', 0x63}, {U'd', 0x64}, {U'e', 0x65},
        {U'f', 0x66}, {U'g', 0x67}, {U'h', 0x68}, {U'i', 0x69}, {U'j', 0x6A},
        {U'k', 0x6B}, {U'l', 0x6C}, {U'm', 0x6D}, {U'n', 0x6E}, {U'o', 0x6F},
        {U'p', 0x70}, {U'q', 0x71}, {U'r', 0x72}, {U's', 0x73}, {U't', 0x74},
        {U'u', 0x75}, {U'v', 0x76}, {U'w', 0x77}, {U'x', 0x78}, {U'y', 0x79}, {U'z', 0x7A},
        {U'\u30A9', 0x80}, {U'\u30A7', 0x81},
        // Original (unmodified-font) hiragana row for bytes 0x90-0x9B --
        // あいうえおかきくけこさし. IMPORTANT: these bytes were being
        // encoded as '?' (0x3F) before, because these 12 syllables share
        // their byte slots with the PT-BR accented-Latin overrides
        // listed further below, and only the accented-Latin keys were
        // mapped. That silently corrupted any [0C] (dakuten-combine)
        // control code sitting right before one of these syllables --
        // combining "?" isn't a valid operation for whatever glyph-merge
        // logic the game uses there, and produced exactly the symptom
        // observed live: a floating dakuten mark with no base character,
        // AND a hang once the game reached that specific text. Mapping
        // these to their correct ORIGINAL byte values fixes the
        // dakuten-combine logic even though the visual GLYPH will still
        // be wrong (the font texture itself was repurposed for accented
        // Latin) -- the combining logic operates on the byte value, not
        // on what glyph currently happens to be drawn at that texture
        // cell, so this is the right fix at the byte level regardless.
        {U'\u3042', 0x90}, {U'\u3044', 0x91}, {U'\u3046', 0x92}, {U'\u3048', 0x93},
        {U'\u304A', 0x94}, {U'\u304B', 0x95}, {U'\u304D', 0x96}, {U'\u304F', 0x97},
        {U'\u3051', 0x98}, {U'\u3053', 0x99}, {U'\u3055', 0x9A}, {U'\u3057', 0x9B},
        // Kana table (byte 0x9C-0xFD, plus 0x80/0x81/0xFF) -- inverse of
        // decode_jp_text.py's BYTE_TO_CHAR. Needed for round-tripping
        // original Japanese text.
        {U'\u3059', 0x9C}, {U'\u305B', 0x9D}, {U'\u305D', 0x9E}, {U'\u305F', 0x9F},
        {U'\u3061', 0xA0}, {U'\u3064', 0xA1}, {U'\u3066', 0xA2}, {U'\u3068', 0xA3},
        {U'\u306A', 0xA4}, {U'\u306B', 0xA5}, {U'\u306C', 0xA6}, {U'\u306D', 0xA7},
        {U'\u306E', 0xA8}, {U'\u306F', 0xA9}, {U'\u3072', 0xAA}, {U'\u3075', 0xAB},
        {U'\u3078', 0xAC}, {U'\u307B', 0xAD}, {U'\u307E', 0xAE}, {U'\u307F', 0xAF},
        {U'\u3080', 0xB0}, {U'\u3081', 0xB1}, {U'\u3082', 0xB2}, {U'\u3084', 0xB3},
        {U'\u3086', 0xB4}, {U'\u3088', 0xB5}, {U'\u3089', 0xB6}, {U'\u308A', 0xB7},
        {U'\u308B', 0xB8}, {U'\u308C', 0xB9}, {U'\u308D', 0xBA}, {U'\u308F', 0xBB},
        {U'\u3092', 0xBC}, {U'\u3093', 0xBD}, {U'\u3083', 0xBE}, {U'\u3085', 0xBF},
        {U'\u3087', 0xC0}, {U'\u3063', 0xC1},
        {U'\u300C', 0xC4}, {U'\u300D', 0xC5}, {U'\u30FB', 0xC6}, {U'\u309C', 0xC7},
        {U'\u309B', 0xC8},
        // CONFIRMED (cross-referenced against 5 known screenshots): the
        // game uses dedicated full-width punctuation codes here, distinct
        // from the ASCII-range 0x21/0x3F used by English text.
        {U'\uFF1F', 0xC2}, // '？' (full-width question mark)
        {U'\uFF01', 0xC3}, // '！' (full-width exclamation mark)
        {U'\u30E3', 0xCB}, {U'\u30E5', 0xCC}, {U'\u30E7', 0xCD}, {U'\u30C3', 0xCE},
        {U'\u30FC', 0xCF},
        {U'\u30A2', 0xD0}, {U'\u30A4', 0xD1}, {U'\u30A6', 0xD2}, {U'\u30A8', 0xD3},
        {U'\u30AA', 0xD4}, {U'\u30AB', 0xD5}, {U'\u30AD', 0xD6}, {U'\u30AF', 0xD7},
        {U'\u30B1', 0xD8}, {U'\u30B3', 0xD9}, {U'\u30B5', 0xDA}, {U'\u30B7', 0xDB},
        {U'\u30B9', 0xDC}, {U'\u30BB', 0xDD}, {U'\u30BD', 0xDE}, {U'\u30BF', 0xDF},
        {U'\u30C1', 0xE0}, {U'\u30C4', 0xE1}, {U'\u30C6', 0xE2}, {U'\u30C8', 0xE3},
        {U'\u30CA', 0xE4}, {U'\u30CB', 0xE5}, {U'\u30CC', 0xE6}, {U'\u30CD', 0xE7},
        {U'\u30CE', 0xE8}, {U'\u30CF', 0xE9}, {U'\u30D2', 0xEA}, {U'\u30D5', 0xEB},
        {U'\u30D8', 0xEC}, {U'\u30DB', 0xED}, {U'\u30DE', 0xEE}, {U'\u30DF', 0xEF},
        {U'\u30E0', 0xF0}, {U'\u30E1', 0xF1}, {U'\u30E2', 0xF2}, {U'\u30E4', 0xF3},
        {U'\u30E6', 0xF4}, {U'\u30E8', 0xF5}, {U'\u30E9', 0xF6}, {U'\u30EA', 0xF7},
        {U'\u30EB', 0xF8}, {U'\u30EC', 0xF9}, {U'\u30ED', 0xFA}, {U'\u30EF', 0xFB},
        {U'\u30F2', 0xFC}, {U'\u30F3', 0xFD},
        {U'\u30A3', 0xFF},
        // PT-BR accented Latin overrides (same slots MM7Loc's English
        // patch already repurposes from the original hiragana row --
        // reuse them here too so both patches share one consistent font).
        {U'\u00C1', 0x90}, {U'\u00C0', 0x91}, {U'\u00C2', 0x92}, {U'\u00C3', 0x93},
        {U'\u00C9', 0x94}, {U'\u00CA', 0x95}, {U'\u00CD', 0x96}, {U'\u00DA', 0x97},
        {U'\u00D5', 0x98}, {U'\u00D3', 0x99}, {U'\u00D4', 0x9A}, {U'\u00C7', 0x9B},
        // lowercase accented (not in the original override set -- added
        // here for convenience; verify visually in-game since these
        // specific byte slots were only confirmed for uppercase forms)
        {U'\u00E1', 0x90}, {U'\u00E0', 0x91}, {U'\u00E2', 0x92}, {U'\u00E3', 0x93},
        {U'\u00E9', 0x94}, {U'\u00EA', 0x95}, {U'\u00ED', 0x96}, {U'\u00FA', 0x97},
        {U'\u00F5', 0x98}, {U'\u00F3', 0x99}, {U'\u00F4', 0x9A}, {U'\u00E7', 0x9B},
    };
    return table;
}

// One patched Japanese dialogue entry.
struct JapaneseEntry {
    uintptr_t destRva;              // RVA of the DAT_140e2XXXX staging slot
                                     // (first qword of the 24-byte triple)
                                     // -- this is what gets overwritten.
    std::vector<uint8_t> headerBytes; // Original header bytes (BEFORE the
                                       // real text), preserved verbatim.
    std::u32string newText;         // Replacement text (already decoded
                                     // from UTF-8 by the JSON loader).
};

// Encodes newText (+ preserved header) into a persistent word-array
// buffer (2 bytes/char, low=code, high=0x00), null-terminated. The
// buffer must outlive the patched pointer -- callers should keep it in
// static/process-lifetime storage (see JapaneseText.cpp).
std::vector<uint16_t> EncodeJapaneseWordArray(
    const std::vector<uint8_t>& headerBytes,
    const std::u32string& text);

// Reads a JSON translation file (see companion Python script for the
// expected format) and overwrites the corresponding DAT_140e2XXXX slots
// with newly-encoded text. Call this AFTER the original
// UndefinedFunction_1400017b0 has run (i.e. from the MinHook detour,
// after calling the original function pointer).
void PatchJapaneseStrings(uintptr_t moduleBase);

// DIAGNOSTIC: spawns a background thread that polls the 24-byte triple
// at moduleBase+watchRva every few milliseconds and logs (via
// DebugOut) whenever either the pointer (+0x00) or the second
// field (+0x08) changes value, with a timestamp. This is how we catch
// writes that happen too early/fast for a manually-armed Cheat Engine
// watchpoint to catch (by the time a person can attach and arm it, the
// write has often already happened during boot). Call once, after
// PatchJapaneseStrings, from the same hook.
void StartWatchingEntry(uintptr_t moduleBase, uintptr_t watchRva, int durationSeconds);

// DIAGNOSTIC: registers the address that FUN_140056360's hook should
// filter on -- only calls whose [RCX+0x18] field matches this exact
// value get logged (FUN_140056360 is called constantly for every bit
// of on-screen text -- HUD, menus, everything -- so logging
// unconditionally would be useless noise; this narrows it down to only
// the specific entry we're debugging). IMPORTANT: pass the entry's
// DESTINATION SLOT address (moduleBase + destRva), NOT the resolved
// text-buffer pointer -- [RCX+0x18] holds a pointer to the slot, which
// needs one more dereference to reach the actual buffer. Called from
// PatchJapaneseStrings for whichever entry IsWatchedEntry() flags.
void SetCharReaderWatchedBuffer(uintptr_t watchedAddr);

// DIAGNOSTIC: installs the MinHook detour on FUN_140056360 (the
// character-reader function). Call once, early (e.g. right after the
// English/Japanese string-table hooks are installed in dllmain.cpp).
bool InstallCharReaderHook(uintptr_t moduleBase);
