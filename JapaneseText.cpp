// JapaneseText.cpp
// -----------------------------------------------------------------------
// See JapaneseText.h for the overall design/rationale.
// -----------------------------------------------------------------------
#include "pch.h"
#include "JapaneseText.h"
#include "Logging.h"
#include "resource.h"
#include <codecvt>
#include <locale>
#include <cstdio>
#include <cstring>
#include <MinHook.h>
#include <intrin.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern HMODULE g_selfModule; // defined in dllmain.cpp

// Keep every allocated word-array buffer alive for the process lifetime
// (mirrors EnglishText.cpp's approach for the English char* arrays --
// the game keeps reading from these pointers for as long as it runs, so
// they must never be freed or reallocated out from under it).
//
// DIAGNOSTIC EXPERIMENT: previously these lived in generic process heap
// (via std::vector), at addresses far outside the game module's own
// address range (e.g. 0x1f96812d5c0 vs module base ~0x7ff6...). Despite
// content and the surrounding 24-byte triple both being verified correct
// all the way through to the hang, the game still hung -- so we're now
// testing whether the buffer's ADDRESS RANGE itself matters (maybe some
// code checks "is this pointer within our own module/near our static
// data" and takes a different, buggy path otherwise). g_nearModuleBlock
// holds one big VirtualAlloc'd region placed as close to the module base
// as the OS will allow, and every entry's buffer is sub-allocated from
// inside it instead of generic heap.
static uint8_t* g_nearModuleBlock = nullptr;
static size_t g_nearModuleBlockUsed = 0;
static size_t g_nearModuleBlockSize = 0;

static uint8_t* AllocateNearModule(uintptr_t moduleBase, size_t totalBytesNeeded) {
    // Try a handful of candidate addresses within +/-1GB of the module
    // base (Windows will refuse any that are already occupied -- that's
    // fine, we just try the next candidate). Falling back to nullptr
    // (let the OS pick anywhere) if none of them work, so this degrades
    // gracefully instead of failing outright.
    const intptr_t offsets[] = {
        0x10000000, -0x10000000, 0x20000000, -0x20000000,
        0x40000000, -0x40000000, 0x60000000, -0x60000000,
    };
    for (intptr_t off : offsets) {
        LPVOID hint = reinterpret_cast<LPVOID>(moduleBase + off);
        LPVOID mem = VirtualAlloc(hint, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mem != nullptr) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "[JapaneseText] Block allocated near module: 0x%llx (offset 0x%llx from moduleBase)\n",
                     (unsigned long long)mem, (unsigned long long)off);
            DebugOut(buf);
            return reinterpret_cast<uint8_t*>(mem);
        }
    }
    // Fallback: let the OS place it anywhere.
    LPVOID mem = VirtualAlloc(nullptr, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DebugOut("[JapaneseText] Error reaching module—using a generic address.\n");
    return reinterpret_cast<uint8_t*>(mem);
}

// Recognizes literal "[XX]" tokens (bracket, 2 hex digits, close bracket
// -- exactly the notation the Python extraction scripts use for
// unmapped/control bytes) inside the text and converts them back to the
// raw byte they represent, instead of trying to map '[', '0', 'C', ']'
// as four separate ordinary characters. This is what makes an
// UNTRANSLATED entry (text left exactly as extracted) round-trip
// byte-for-byte, preserving whatever line-break/dakuten/etc control
// codes were in the original -- critical, since silently mangling every
// control code into a literal '?' would corrupt in-game text formatting
// even for entries nobody touched.
// CONFIRMED (cross-referenced against 5 known screenshots, 9/9 matches
// on the line-break rule; dakuten confirmed via multiple examples;
// han-dakuten confirmed via [0D]ハワーアッ[0D]フ -> パワーアップ):
// reverse mapping for the dakuten/han-dakuten combine control codes, so
// translated text can use normal precomposed characters (が, ぱ, etc)
// and still round-trip to the [0x0C, base]/[0x0D, base] byte pairs the
// game actually expects. Keep in sync with build_japanese_strings_json.py's
// DAKUTEN_MAP/HANDAKUTEN_MAP (same rule, opposite direction).
static const std::unordered_map<char32_t, char32_t> g_dakutenToBase = {
    {U'\u304C', U'\u304B'}, {U'\u304E', U'\u304D'}, {U'\u3050', U'\u304F'},
    {U'\u3052', U'\u3051'}, {U'\u3054', U'\u3053'},
    {U'\u3056', U'\u3055'}, {U'\u3058', U'\u3057'}, {U'\u305A', U'\u3059'},
    {U'\u305C', U'\u305B'}, {U'\u305E', U'\u305D'},
    {U'\u3060', U'\u305F'}, {U'\u3062', U'\u3061'}, {U'\u3065', U'\u3064'},
    {U'\u3067', U'\u3066'}, {U'\u3069', U'\u3068'},
    {U'\u3070', U'\u306F'}, {U'\u3073', U'\u3072'}, {U'\u3076', U'\u3075'},
    {U'\u3079', U'\u3078'}, {U'\u307C', U'\u307B'},
    {U'\u30AC', U'\u30AB'}, {U'\u30AE', U'\u30AD'}, {U'\u30B0', U'\u30AF'},
    {U'\u30B2', U'\u30B1'}, {U'\u30B4', U'\u30B3'},
    {U'\u30B6', U'\u30B5'}, {U'\u30B8', U'\u30B7'}, {U'\u30BA', U'\u30B9'},
    {U'\u30BC', U'\u30BB'}, {U'\u30BE', U'\u30BD'},
    {U'\u30C0', U'\u30BF'}, {U'\u30C2', U'\u30C1'}, {U'\u30C5', U'\u30C4'},
    {U'\u30C7', U'\u30C6'}, {U'\u30C9', U'\u30C8'},
    {U'\u30D0', U'\u30CF'}, {U'\u30D3', U'\u30D2'}, {U'\u30D6', U'\u30D5'},
    {U'\u30D9', U'\u30D8'}, {U'\u30DC', U'\u30DB'},
};
static const std::unordered_map<char32_t, char32_t> g_handakutenToBase = {
    {U'\u3071', U'\u306F'}, {U'\u3074', U'\u3072'}, {U'\u3077', U'\u3075'},
    {U'\u307A', U'\u3078'}, {U'\u307D', U'\u307B'},
    {U'\u30D1', U'\u30CF'}, {U'\u30D4', U'\u30D2'}, {U'\u30D7', U'\u30D5'},
    {U'\u30DA', U'\u30D8'}, {U'\u30DD', U'\u30DB'},
};
std::vector<uint16_t> EncodeJapaneseWordArray(
    const std::vector<uint16_t>& headerWords,
    const std::u32string& text)
{
    std::vector<uint16_t> out;
    out.reserve(headerWords.size() + text.size() + 1);

    // Preserve the original header verbatim, one 16-bit word per slot.
    //
    // BUG FIX: this used to truncate every header word to its low byte
    // via `std::vector<uint8_t>`. Entry 18's header word at index 3 (the
    // parameter of the typing-speed control, [SetInitialTypeSpeed?]) is
    // 0x0200 (512) in the original game, which doesn't fit in a byte.
    // Truncating it to 0 permanently stalled the text-reveal timer and
    // softlocked the dialogue box. Header words must be preserved as
    // full 16-bit values.
    for (uint16_t w : headerWords) {
        out.push_back(w);
    }

    auto isHexDigit = [](char32_t c) {
        return (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'F') || (c >= U'a' && c <= U'f');
    };
    auto hexVal = [](char32_t c) -> int {
        if (c >= U'0' && c <= U'9') return c - U'0';
        if (c >= U'A' && c <= U'F') return c - U'A' + 10;
        if (c >= U'a' && c <= U'f') return c - U'a' + 10;
        return 0;
    };

    const auto& charToByte = GetCharToByteTable();
    size_t i = 0;
    while (i < text.size()) {
        // Try the 4-hex-digit "[LLHH]" token first (a full word with a
        // nonzero high byte -- some control codes aren't just a control
        // BYTE, they're a full word that doesn't fit the simpler [XX]
        // notation; an earlier version of the extraction pipeline
        // stopped reading entirely the first time one of these showed
        // up mid-entry, silently truncating a lot of real content in
        // many entries -- fixed on the Python side, and this format
        // needs to be recognized here too).
        if (text[i] == U'[' && i + 5 < text.size() &&
            isHexDigit(text[i + 1]) && isHexDigit(text[i + 2]) &&
            isHexDigit(text[i + 3]) && isHexDigit(text[i + 4]) && text[i + 5] == U']') {
            uint8_t lo = static_cast<uint8_t>((hexVal(text[i + 1]) << 4) | hexVal(text[i + 2]));
            uint8_t hi = static_cast<uint8_t>((hexVal(text[i + 3]) << 4) | hexVal(text[i + 4]));
            uint16_t word = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
            out.push_back(word);
            i += 6;
            continue;
        }
        // Try to match a "[XX]" control-byte token (high byte implicitly 0).
        if (text[i] == U'[' && i + 3 < text.size() &&
            isHexDigit(text[i + 1]) && isHexDigit(text[i + 2]) && text[i + 3] == U']') {
            uint8_t rawByte = static_cast<uint8_t>((hexVal(text[i + 1]) << 4) | hexVal(text[i + 2]));
            out.push_back(static_cast<uint16_t>(rawByte));
            i += 4;
            continue;
        }

        char32_t ch = text[i];

        // Local helper: compares an ASCII literal (as a u32string) against
        // the current position in the text, without allocating anything.
        auto matchesLiteral = [&](const char32_t* lit) -> bool {
            size_t len = 0;
            while (lit[len] != 0) len++;
            if (i + len > text.size()) return false;
            for (size_t k = 0; k < len; k++) {
                if (text[i + k] != lit[k]) return false;
            }
            return true;
            };

        // CONFIRMED RULE: [DrawClosingMark] is a readable alias for the
        // raw marker 0xCA -- draws the closing full-width period "。".
        // RENAMED from the old, misleading [NEWBOX]: confirmed via
        // dynamic debugging (see MM7Loc_INVESTIGATION_SUMMARY.md, 3.10)
        // that this does NOT create or destroy any box object -- the
        // box is created exactly once per entry (FUN_140057a10), never
        // per this marker. It's purely a glyph draw. Always pair it
        // with [NewLine] right after if the following text needs to
        // start at the left margin -- nothing does that automatically.
        if (matchesLiteral(U"[DrawClosingMark]")) {
            out.push_back(0x00CA);
            i += 17; // length of "[DrawClosingMark]"
            continue;
        }

        // CONFIRMED RULE: [WaitFrames][XX] is a readable alias for the
        // raw pair 0x07 XX ("one-off pause of XX frames").
        if (matchesLiteral(U"[WaitFrames][")) {
            size_t hexStart = i + 13; // length of "[WaitFrames]["
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0007);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED RULE (live instrumentation, exact value match):
        // [SetTypeSpeed][XX] is a readable alias for the raw pair
        // 0x02 XX ("sets the typing pace PERSISTENTLY -- stays in
        // effect until the next [SetTypeSpeed]").
        if (matchesLiteral(U"[SetTypeSpeed][")) {
            size_t hexStart = i + 15; // length of "[SetTypeSpeed]["
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0002);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED (decompile of idx=3's handler): reads a parameter
        // word into [+0x34] -- distinct from [SetTypeSpeed] (idx=2),
        // which writes [+0x3f]/[+0x40] instead. HYPOTHESIS (not
        // proven): a BASE/initial typing speed for the whole entry.
        //
        // BUG FIX: every entry's header uses param=512 (0x0200), not a
        // single byte -- same class of fix as [TabToColumn]/
        // [RepeatBlankTile] above. Accept either a 2-digit [XX] or a
        // 4-digit [XXXX] form.
        if (matchesLiteral(U"[SetInitialTypeSpeed?][")) {
            size_t hexStart = i + 23; // length of "[SetInitialTypeSpeed?]["
            if (hexStart + 4 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                isHexDigit(text[hexStart + 2]) && isHexDigit(text[hexStart + 3]) &&
                text[hexStart + 4] == U']') {
                uint16_t param = static_cast<uint16_t>(
                    (hexVal(text[hexStart]) << 12) | (hexVal(text[hexStart + 1]) << 8) |
                    (hexVal(text[hexStart + 2]) << 4) | hexVal(text[hexStart + 3]));
                out.push_back(0x0003);
                out.push_back(param);
                i = hexStart + 5;
                continue;
            }
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0003);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED: [PacingTick][XX] is a readable alias for the raw
        // pair 0x05 XX -- the same function (FUN_140056720) that
        // already processes normal typing pace, just called from the
        // post-[DrawClosingMark] footer. Doesn't write the rate, only reads it --
        // exact purpose still uncertain, but the technical behavior is
        // confirmed.
        if (matchesLiteral(U"[PacingTick][")) {
            size_t hexStart = i + 13; // length of "[PacingTick]["
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0005);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED RULE: a literal '\n' (typed by you, the translator)
        // always means "a real visible line break" -- becomes the PAIR
        // 08 08 (which always meant line break across the 9 confirmed
        // screenshots).
        if (ch == U'\n') {
            out.push_back(0x0008);
            out.push_back(0x0008);
            i++;
            continue;
        }

        // CONFIRMED: [NewLine] represents the pair 08 08 (visible line
        // break, confirmed across 9 screenshots).
        if (matchesLiteral(U"[NewLine]")) {
            out.push_back(0x0008);
            out.push_back(0x0008);
            i += 9; // length of "[NewLine]"
            continue;
        }

        // CONFIRMED (same dispatch-table slot as [NewLine]/idx=8): a
        // LONE 0x08 calls the exact same handler as [NewLine], just
        // once instead of twice -- "return to left margin + advance by
        // one step" rather than a full line break. Only seen in the
        // post-[DrawClosingMark] footer so far.
        if (matchesLiteral(U"[AdvanceCursorLine]")) {
            out.push_back(0x0008);
            i += 19; // length of "[AdvanceCursorLine]"
            continue;
        }

        // CONFIRMED: [SetLeftMargin][XX] is a readable alias for the raw
        // pair 0x0A XX -- sets the line-start margin that [NewLine]
        // (0x08) later uses. Runs once per box, in the header.
        //
        // BUG FIX: some entries use a margin value >255 (e.g. 390,
        // 643) -- same class of fix as [TabToColumn]/[RepeatBlankTile]/
        // [SetInitialTypeSpeed?] above. Accept either a 2-digit [XX] or
        // a 4-digit [XXXX] form.
        if (matchesLiteral(U"[SetLeftMargin][")) {
            size_t hexStart = i + 16; // length of "[SetLeftMargin]["
            if (hexStart + 4 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                isHexDigit(text[hexStart + 2]) && isHexDigit(text[hexStart + 3]) &&
                text[hexStart + 4] == U']') {
                uint16_t param = static_cast<uint16_t>(
                    (hexVal(text[hexStart]) << 12) | (hexVal(text[hexStart + 1]) << 8) |
                    (hexVal(text[hexStart + 2]) << 4) | hexVal(text[hexStart + 3]));
                out.push_back(0x000A);
                out.push_back(param);
                i = hexStart + 5;
                continue;
            }
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x000A);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED: [ForceWait] is the readable alias for the raw byte
        // 0x0F, on its own -- no parameter (unlike the other controls).
        // Sets [+0x3e], which [WaitFrames] uses to decide whether to
        // accept skipping the pause with the confirm button.
        if (matchesLiteral(U"[ForceWait]")) {
            out.push_back(0x000F);
            i += 11; // length of "[ForceWait]"
            continue;
        }

        // CONFIRMED (decompile of idx=14's handler): clears [+0x3e] to
        // 0 -- the exact same field [ForceWait] (idx=15) sets to 1. No
        // parameter. The "undo"/cancel counterpart to [ForceWait].
        //
        // RESOLVED (not a conflict): indices 12 (0x0C) and 13 (0x0D) in
        // the Japanese dispatch table also have decompiled handlers
        // that draw a small raised tile (0xC9 / 0xC7) above the
        // current cursor position without moving it -- that's the
        // actual glyph-level mechanism behind the dakuten/han-dakuten
        // combiner above, not a competing meaning. No separate tag
        // needed here; the precomposed-character handling above already
        // represents it at a readable level.
        if (matchesLiteral(U"[ClearForceWait]")) {
            out.push_back(0x000E);
            i += 16; // length of "[ClearForceWait]"
            continue;
        }

        // CONFIRMED (decompile of idx=16's handler, AND of
        // FUN_1400fba90 itself, the function it calls): a genuine
        // sound-effect manager -- remaps/collision-checks sound IDs,
        // stops any conflicting sound before playing a new one via a
        // vtable call, and accepts an optional loop/volume-style
        // parameter. Strongly confirms the "typewriter blip" hypothesis
        // even without an in-game test pinning down which exact sound.
        // Reads a parameter word (only the low byte is kept, per the
        // decompile using `char` not `short`) into [+0x41], checked
        // after every glyph draw in the character-reader.
        if (matchesLiteral(U"[SetTypeSound?][")) {
            size_t hexStart = i + 16; // length of "[SetTypeSound?]["
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0010);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED (decompile of idx=19's handler, FUN_140056c70, AND
        // of FUN_1400092c0 itself): walks a linked list of visual
        // items (icons/sprites) attached to the dialogue box, and for
        // each one flagged a specific way, either destroys it or fully
        // resets it to a hidden/default state (moved off-screen,
        // scale/color zeroed). Empties the list afterward. No
        // parameter. Only ever seen as the very first word of
        // weapon-description entries. HYPOTHESIS (not proven):
        // clears/hides whatever icon was left over from the previous
        // weapon description before this one's own icon shows.
        if (matchesLiteral(U"[ClearIconList?]")) {
            out.push_back(0x0013);
            i += 16; // length of "[ClearIconList?]"
            continue;
        }

        // CONFIRMED (decompile of idx=17's handler): reads a parameter
        // word and passes it DIRECTLY to FUN_1400fba90(value, 0) -- the
        // exact same confirmed sound-effect-manager function
        // [SetTypeSound?] (idx=16) uses. Unlike idx=16 (persistent,
        // per-character), this plays the sound ONCE, immediately.
        if (matchesLiteral(U"[PlaySound][")) {
            size_t hexStart = i + 12; // length of "[PlaySound]["
            if (hexStart + 4 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                isHexDigit(text[hexStart + 2]) && isHexDigit(text[hexStart + 3]) &&
                text[hexStart + 4] == U']') {
                uint16_t param = static_cast<uint16_t>(
                    (hexVal(text[hexStart]) << 12) | (hexVal(text[hexStart + 1]) << 8) |
                    (hexVal(text[hexStart + 2]) << 4) | hexVal(text[hexStart + 3]));
                out.push_back(0x0011);
                out.push_back(param);
                i = hexStart + 5;
                continue;
            }
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0011);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED (decompile of idx=18's handler): reads a parameter
        // word; if it's GREATER than 16 (0x10), calls FUN_1400fb840()
        // (not decompiled, no arguments). HYPOTHESIS (not proven): some
        // kind of conditional trigger/flag gated by a value threshold.
        if (matchesLiteral(U"[ConditionalTrigger?][")) {
            size_t hexStart = i + 22; // length of "[ConditionalTrigger?]["
            if (hexStart + 4 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                isHexDigit(text[hexStart + 2]) && isHexDigit(text[hexStart + 3]) &&
                text[hexStart + 4] == U']') {
                uint16_t param = static_cast<uint16_t>(
                    (hexVal(text[hexStart]) << 12) | (hexVal(text[hexStart + 1]) << 8) |
                    (hexVal(text[hexStart + 2]) << 4) | hexVal(text[hexStart + 3]));
                out.push_back(0x0012);
                out.push_back(param);
                i = hexStart + 5;
                continue;
            }
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0012);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED (decompile of idx=21's handler, LAB_140056c90):
        // zeroes [+0x38] (the same undocumented field used in
        // [TabToColumn]'s formula) and copies the saved margin anchor
        // ([+0x32]) back into the live cursor ([+0x30]) -- half of what
        // [NewLine] does (return to left margin), without moving down a
        // line. No parameter.
        if (matchesLiteral(U"[ResetToMarginSameLine]")) {
            out.push_back(0x0015);
            i += 23; // length of "[ResetToMarginSameLine]"
            continue;
        }

        // CONFIRMED (decompile of idx=1's handler): reads a parameter
        // word into [+0x3c], later passed as the third argument to the
        // glyph-drawing function's box/balloon resolver. HYPOTHESIS
        // (not proven): selects which character the dialogue balloon's
        // tail points at. Labeled provisionally with a "?" -- rename if
        // disproven.
        if (matchesLiteral(U"[SetSpeaker?][")) {
            size_t hexStart = i + 14; // length of "[SetSpeaker?]["
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0001);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED (decompile of idx=11's handler): increments a
        // GLOBAL counter (DAT_1408fd954) unrelated to this entry's own
        // state -- no parameter, no effect on this box's layout/timing.
        // Always seen in the post-[DrawClosingMark] footer.
        if (matchesLiteral(U"[IncrementCounter]")) {
            out.push_back(0x000B);
            i += 18; // length of "[IncrementCounter]"
            continue;
        }

        // CONFIRMED (decompile of idx=4's handler): reads a parameter
        // word N, then draws tile 0 (blank/space) N times, advancing
        // the cursor each time -- "print N blank tiles". HYPOTHESIS: a
        // spacing/indent command. Accepts either 2 or 4 hex digits,
        // same reasoning as [TabToColumn] (see its comment above).
        if (matchesLiteral(U"[RepeatBlankTile][")) {
            size_t hexStart = i + 18; // length of "[RepeatBlankTile]["
            if (hexStart + 4 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                isHexDigit(text[hexStart + 2]) && isHexDigit(text[hexStart + 3]) &&
                text[hexStart + 4] == U']') {
                uint16_t param = static_cast<uint16_t>(
                    (hexVal(text[hexStart]) << 12) | (hexVal(text[hexStart + 1]) << 8) |
                    (hexVal(text[hexStart + 2]) << 4) | hexVal(text[hexStart + 3]));
                out.push_back(0x0004);
                out.push_back(param);
                i = hexStart + 5;
                continue;
            }
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0004);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED (decompile of idx=9's handler): reads a parameter
        // word, same shape as [SetLeftMargin]/[SetSpeaker?]. Computes
        // an absolute cursor-X jump: [+0x30] = ([+0x38]&0xFFF8)*4 +
        // [+0x32] + param. HYPOTHESIS (well-supported, not proven): a
        // column/tab positioning command -- its only confirmed use is on
        // the credits screen (entry 68), right before "PRODUCER" /
        // "PROFESSOR F".
        //
        // BUG FIX: unlike every other parameterized control code here,
        // this one's parameter isn't always a single byte -- entry 68's
        // real value is 1360 (0x0550). Accept EITHER a 2-digit [XX]
        // (0-255, the common small-param case) or a 4-digit [XXXX] (full
        // 16-bit) form. The decoder (build_japanese_strings_json.py)
        // picks whichever is shortest for round-trip, but both are
        // accepted here regardless of which one produced the text.
        if (matchesLiteral(U"[WaitForConfirm]")) {
            out.push_back(0x0006);
            i += 16; // length of "[WaitForConfirm]"
            continue;
        }

        if (matchesLiteral(U"[TabToColumn][")) {
            size_t hexStart = i + 14; // length of "[TabToColumn]["
            // Try the 4-digit form first (longer match takes priority).
            if (hexStart + 4 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                isHexDigit(text[hexStart + 2]) && isHexDigit(text[hexStart + 3]) &&
                text[hexStart + 4] == U']') {
                uint16_t param = static_cast<uint16_t>(
                    (hexVal(text[hexStart]) << 12) | (hexVal(text[hexStart + 1]) << 8) |
                    (hexVal(text[hexStart + 2]) << 4) | hexVal(text[hexStart + 3]));
                out.push_back(0x0009);
                out.push_back(param);
                i = hexStart + 5;
                continue;
            }
            if (hexStart + 2 < text.size() &&
                isHexDigit(text[hexStart]) && isHexDigit(text[hexStart + 1]) &&
                text[hexStart + 2] == U']') {
                uint8_t param = static_cast<uint8_t>(
                    (hexVal(text[hexStart]) << 4) | hexVal(text[hexStart + 1]));
                out.push_back(0x0009);
                out.push_back(static_cast<uint16_t>(param));
                i = hexStart + 3;
                continue;
            }
        }

        // CONFIRMED RULE: precomposed dakuten/han-dakuten characters
        // (が, ぱ, etc) re-encode as [0x0C or 0x0D, base-character-byte]
        // -- the game combines these two words back into the voiced
        // glyph at display time.
        {
            auto dIt = g_dakutenToBase.find(ch);
            auto hIt = g_handakutenToBase.find(ch);
            if (dIt != g_dakutenToBase.end() || hIt != g_handakutenToBase.end()) {
                bool isDakuten = (dIt != g_dakutenToBase.end());
                char32_t baseCh = isDakuten ? dIt->second : hIt->second;
                auto baseIt = charToByte.find(baseCh);
                if (baseIt != charToByte.end()) {
                    out.push_back(isDakuten ? 0x000C : 0x000D);
                    out.push_back(static_cast<uint16_t>(baseIt->second));
                    i++;
                    continue;
                }
                // base character somehow not in the table -- fall through
                // to the normal lookup below (will '?'-fallback and log).
            }
        }

        auto it = charToByte.find(ch);
        uint8_t code;
        if (it != charToByte.end()) {
            code = it->second;
        } else {
            code = 0x3F; // '?' fallback for genuinely unmapped characters
            char logbuf[128];
            snprintf(logbuf, sizeof(logbuf),
                     "[JapaneseText] Warning: character U+%04X has no mapping, using '?'\n",
                     static_cast<unsigned>(ch));
            DebugOut(logbuf);
        }
        out.push_back(static_cast<uint16_t>(code));
        i++;
    }

    out.push_back(0x0000); // null terminator (lo=0, hi=0)
    return out;
}

// Minimal UTF-8 -> UTF-32 decoder (avoids relying on the deprecated
// std::wstring_convert where possible, but that's the simplest portable
// option available without pulling in a new dependency -- fine for a
// one-shot startup load of a small JSON file).
static std::u32string Utf8ToU32(const std::string& utf8) {
    std::u32string result;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = utf8[i];
        char32_t cp = 0;
        int extra = 0;
        if ((c & 0x80) == 0x00) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { i++; continue; } // invalid leading byte, skip
        i++;
        bool valid = true;
        for (int k = 0; k < extra; k++) {
            if (i >= utf8.size() || (utf8[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (utf8[i] & 0x3F);
            i++;
        }
        if (valid) result.push_back(cp);
    }
    return result;
}

// Cache of parsed+encoded entries, built once on first call. If
// UndefinedFunction_1400017b0 turns out to run more than once (unlike
// RVA_INIT_STRINGS, which is confirmed one-shot -- we don't have that
// same confirmation for this function), the pointer WRITES below still
// need to happen on every call in case a later call re-populates
// (and so overwrites) the slots we already patched. The expensive part
// -- JSON parsing and word-array encoding -- only happens once.
struct PendingPatch {
    uintptr_t destRva;
    uintptr_t bufferPtr;  // pointer into g_nearModuleBlock's stable storage
    uint64_t wordCount;   // total words written (header + text + terminator) --
                           // candidate replacement value for the count field
                           // at destAddr+0x08 (see PatchJapaneseStrings).
};
static std::vector<PendingPatch> g_pendingPatches;
static bool g_loaded = false;

// DIAGNOSTIC: which entries to log/watch in detail. Started with just
// index 18 (RVA 0xe26cd0); add more RVAs to the list below as new
// problem entries are identified.
//
// Currently returns true for EVERY entry, which floods the log. That was
// a temporary measure used to locate the in-game box showing ~12 dots
// (found: entry 25's header embeds "............" via the auxiliary
// table -- see MM7Loc_INVESTIGATION_SUMMARY.md, 2.10). Narrow it back to
// an explicit list (the commented-out version below) before release.
static bool IsWatchedEntry(uintptr_t destRva) {
    (void)destRva;
    return IsDebugModeOn;
}
/*
static bool IsWatchedEntry(uintptr_t destRva) {
    switch (destRva) {
        case 0xe26cd0: // index 18
            return true;
        case 0xe26d78: // index 25 -- header/text boundary investigation:
                       // checking whether the "." glyph drawn is really
                       // the missing parameter of [SetLeftMargin] in the
                       // header, rather than real text ("わあ きれい！")
            return true;
        case 0xe277e0: // index 136 -- all-dots entry, also matches
                       // the boundary-bug header pattern
            return true;
        case 0xe279d8: // index 157 -- same, all-dots + pattern match
            return true;
        default:
            return false;
    }
}
*/

static void LoadAndBuildOnce(uintptr_t moduleBase) {
    if (g_loaded) return;
    g_loaded = true;

    // Reads the JSON as a resource embedded in the DLL itself -- the
    // same mechanism EnglishText.cpp already uses for English
    // (GameTextUS.json). No longer depends on the process's working
    // directory or the folder the DLL is installed in.
    HRSRC hRes = FindResource(g_selfModule, MAKEINTRESOURCE(IDR_GAMETEXT_JP), RT_RCDATA);
    if (!hRes) {
        DebugOut("[JapaneseText] FindResource failed for GameTextJP.json\n");
        return;
    }

    HGLOBAL hData = LoadResource(g_selfModule, hRes);
    DWORD size = SizeofResource(g_selfModule, hRes);
    const char* rawData = static_cast<const char*>(LockResource(hData));

    if (!hData || !rawData || size == 0) {
        DebugOut("[JapaneseText] LoadResource/LockResource failed for GameTextJP.json\n");
        return;
    }

    json j;
    try {
        j = json::parse(rawData, rawData + size);
    }
    catch (const std::exception& e) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[JapaneseText] Erro parseando GameTextJP.json: %s\n", e.what());
        DebugOut(buf);
        return;
    }

    // Pass 1: decode/encode every entry into a temporary heap buffer, and
    // remember its destRva alongside it -- we don't know the final
    // near-module addresses yet, so these are just staging.
    struct StagedEntry {
        uintptr_t destRva;
        std::vector<uint16_t> words;
    };
    std::vector<StagedEntry> staged;
    staged.reserve(j.size());

    size_t totalWords = 0;
    for (auto& [key, entryJson] : j.items()) {
        try {
            uintptr_t destRva = std::stoull(entryJson.at("dest_rva").get<std::string>(), nullptr, 16);

            std::vector<uint16_t> headerWords;
            if (entryJson.contains("header_bytes")) {
                for (auto& b : entryJson.at("header_bytes")) {
                    // Field name kept as "header_bytes" for JSON
                    // compatibility, but values can legitimately exceed
                    // 255 -- parse as a full int, not a byte.
                    int v = b.get<int>();
                    headerWords.push_back(static_cast<uint16_t>(v));
                }
            }

            std::string textUtf8 = entryJson.at("text").get<std::string>();
            std::u32string text = Utf8ToU32(textUtf8);

            std::vector<uint16_t> words = EncodeJapaneseWordArray(headerWords, text);
            totalWords += words.size();
            staged.push_back({destRva, std::move(words)});
        } catch (const std::exception& e) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[JapaneseText] Error in entry '%s': %s\n", key.c_str(), e.what());
            DebugOut(buf);
        }
    }

    // Pass 2: allocate one big block as close to the module as the OS
    // will allow, and copy every staged buffer into it back-to-back.
    size_t totalBytes = totalWords * sizeof(uint16_t);
    g_nearModuleBlock = AllocateNearModule(moduleBase, totalBytes);
    g_nearModuleBlockSize = totalBytes;
    g_nearModuleBlockUsed = 0;

    if (g_nearModuleBlock == nullptr) {
        DebugOut("[JapaneseText] VirtualAlloc failed completely -- Japanese patch aborted.\n");
        return;
    }

    for (auto& entry : staged) {
        size_t bytes = entry.words.size() * sizeof(uint16_t);
        uint8_t* dst = g_nearModuleBlock + g_nearModuleBlockUsed;
        memcpy(dst, entry.words.data(), bytes);
        uintptr_t bufferPtr = reinterpret_cast<uintptr_t>(dst);
        uint64_t wordCount = static_cast<uint64_t>(entry.words.size());
        g_nearModuleBlockUsed += bytes;

        g_pendingPatches.push_back({entry.destRva, bufferPtr, wordCount});

        // Register this entry's DESTINATION SLOT address (moduleBase +
        // destRva) -- NOT bufferPtr -- as what the character-reader hook
        // should filter on. [RCX+0x18] holds a pointer to the staging
        // slot (e.g. the DAT_140e26cd0-style address), which only after
        // ONE MORE dereference resolves to the actual text buffer
        // (bufferPtr).
        //
        // NOTE: this per-entry call was removed -- watching is now set
        // up unconditionally in PatchJapaneseStrings (see that function)
        // so it works whether or not this specific entry is present in
        // GameTextJP.json this run, letting us compare original vs
        // patched behavior for the same slot.
    }

    // DIAGNOSTIC: the original text always lived in read-only .rdata.
    // Our replacement has been read-write so far. If some game code
    // tries to WRITE to this buffer (e.g. marking characters as "already
    // shown"), that write would have hard-crashed on the original
    // (read-only) memory -- maybe the game has handling around that
    // specific failure mode that never triggers against our writable
    // memory, causing a hang instead of the crash/recovery the original
    // code path expects. Locking the whole block down to read-only here,
    // AFTER all the content is copied in, tests that theory directly.
    DWORD oldProtect;
    BOOL protectOk = VirtualProtect(g_nearModuleBlock, g_nearModuleBlockSize, PAGE_READONLY, &oldProtect);
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "[JapaneseText] VirtualProtect(PAGE_READONLY) -> %s\n",
                 protectOk ? "sucess" : "FAILED");
        DebugOut(buf);
    }

    char summary[128];
    snprintf(summary, sizeof(summary), "[JapaneseText] %zu entries loaded from GameTextJP.json.\n",
              g_pendingPatches.size());
    DebugOut(summary);
}

// DIAGNOSTIC TOGGLE: confirmed FALSE via live logging -- this field gets
// overwritten by the GAME ITSELF shortly after our hook runs (observed:
// our write of a small int, e.g. 0x38, later replaced by something that
// looks like ANOTHER VALID POINTER, e.g. 0x7FF60CFB8138 / RVA 0x578138 --
// squarely inside the same address range that holds other real text
// data). So this is NOT a static "count" we should be computing/keeping
// in sync -- it's dynamically managed by other game logic closer to
// when this specific entry is actually displayed, and whatever we write
// here gets clobbered regardless. Leaving this OFF; touching this field
// was never the fix.
static constexpr bool PATCH_COUNT_FIELD = false;

void PatchJapaneseStrings(uintptr_t moduleBase) {
    LoadAndBuildOnce(moduleBase);

    for (const auto& p : g_pendingPatches) {
        uintptr_t destAddr = moduleBase + p.destRva;
        bool watched = IsWatchedEntry(p.destRva);

        // DIAGNOSTIC: log exactly what we're about to write for entries
        // we're actively debugging, so there's zero ambiguity about
        // whether this code path ran, and with what values, versus what
        // Cheat Engine later reads from memory.
        if (watched) {
            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[JapaneseText][DEBUG] rva=0x%llx destAddr=0x%llx bufferPtr=0x%llx wordCount=%llu (0x%llx)\n",
                     (unsigned long long)p.destRva, (unsigned long long)destAddr, (unsigned long long)p.bufferPtr,
                     (unsigned long long)p.wordCount, (unsigned long long)p.wordCount);
            DebugOut(dbg);
        }

        // The destination slot holds a VA pointer (first qword of the
        // 24-byte triple). Overwrite that pointer always.
        *reinterpret_cast<uintptr_t*>(destAddr) = p.bufferPtr;

        if (PATCH_COUNT_FIELD) {
            // +0x08: candidate "count" field. Trying wordCount (total
            // words written, including the header we preserved and the
            // null terminator) as the replacement value -- adjust/try
            // variants (e.g. excluding header, excluding terminator) if
            // this specific formula doesn't resolve the hang.
            *reinterpret_cast<uint64_t*>(destAddr + 0x08) = p.wordCount;
        }
        // +0x10 (third field): still always seen as 0 in every sample
        // captured so far -- left untouched.

        // DIAGNOSTIC: read back immediately after writing, to rule out
        // anything (another thread, another hook, a compiler reordering
        // surprise) touching this memory between our write and now.
        if (watched) {
            uintptr_t readBackPtr = *reinterpret_cast<uintptr_t*>(destAddr);
            uint64_t readBackCount = *reinterpret_cast<uint64_t*>(destAddr + 0x08);
            char dbg2[256];
            snprintf(dbg2, sizeof(dbg2),
                     "[JapaneseText][DEBUG] rva=0x%llx right after write -- ptr=0x%llx count=0x%llx\n",
                     (unsigned long long)p.destRva,
                     (unsigned long long)readBackPtr, (unsigned long long)readBackCount);
            DebugOut(dbg2);

            // Watch this specific entry for the next 60 seconds so we
            // can see EXACTLY when (and to what) the game changes these
            // fields, instead of only comparing two disconnected
            // snapshots (right-after-boot vs. whenever we happen to
            // check in Cheat Engine later).
            StartWatchingEntry(moduleBase, p.destRva, 60);
        }
    }
}

// --- StartWatchingEntry implementation ---

struct WatchThreadArgs {
    uintptr_t moduleBase;
    uintptr_t watchRva;
    int durationSeconds;
};

static DWORD WINAPI WatchThreadProc(LPVOID lpParam) {
    WatchThreadArgs* args = reinterpret_cast<WatchThreadArgs*>(lpParam);
    uintptr_t addr = args->moduleBase + args->watchRva;
    uintptr_t rva = args->watchRva;

    uintptr_t lastPtr = *reinterpret_cast<volatile uintptr_t*>(addr);
    uint64_t lastCount = *reinterpret_cast<volatile uint64_t*>(addr + 0x08);
    uint64_t lastThird = *reinterpret_cast<volatile uint64_t*>(addr + 0x10);

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[JapaneseText][WATCH] rva=0x%llx start -- ptr=0x%llx field2=0x%llx field3=0x%llx\n",
                 (unsigned long long)rva,
                 (unsigned long long)lastPtr, (unsigned long long)lastCount, (unsigned long long)lastThird);
        DebugOut(buf);
    }

    DWORD startTick = GetTickCount();
    DWORD durationMs = static_cast<DWORD>(args->durationSeconds) * 1000;

    while (GetTickCount() - startTick < durationMs) {
        Sleep(50); // poll ~20x/sec -- frequent enough to catch boot-time
                   // writes, cheap enough not to matter for a few seconds

        uintptr_t curPtr = *reinterpret_cast<volatile uintptr_t*>(addr);
        uint64_t curCount = *reinterpret_cast<volatile uint64_t*>(addr + 0x08);
        uint64_t curThird = *reinterpret_cast<volatile uint64_t*>(addr + 0x10);

        if (curPtr != lastPtr || curCount != lastCount || curThird != lastThird) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "[JapaneseText][WATCH] rva=0x%llx CHANGED in t=%lums -- ptr=0x%llx field2=0x%llx field3=0x%llx\n",
                     (unsigned long long)rva, (unsigned long)(GetTickCount() - startTick),
                     (unsigned long long)curPtr, (unsigned long long)curCount, (unsigned long long)curThird);
            DebugOut(buf);
            lastPtr = curPtr;
            lastCount = curCount;
            lastThird = curThird;
        }
    }

    DebugOut("[JapaneseText][WATCH] watch ended.\n");
    delete args;
    return 0;
}

void StartWatchingEntry(uintptr_t moduleBase, uintptr_t watchRva, int durationSeconds) {
    WatchThreadArgs* args = new WatchThreadArgs{ moduleBase, watchRva, durationSeconds };
    HANDLE h = CreateThread(nullptr, 0, WatchThreadProc, args, 0, nullptr);
    if (h != nullptr) {
        CloseHandle(h); // don't need the handle -- thread runs detached
    } else {
        DebugOut("[JapaneseText][WATCH] failed to create watch thread.\n");
        delete args;
    }
}


// --- FUN_140056360 (character reader) diagnostic hook ---
//
// This function is called constantly for EVERY bit of on-screen text
// (HUD, menus, all dialogue -- not just Japanese), using RCX as a
// pointer to a per-text render-context struct. We already know two of
// its fields from the earlier reverse-engineering: [RCX+0x18] is the
// word-array text pointer (Japanese path) and [RCX+0x20] is a running
// character index. We don't know the FULL calling convention for
// certain (how many real parameters it takes beyond RCX), so the
// detour is declared with all four integer/pointer argument registers
// (RCX/RDX/R8/R9) as generic 64-bit values and forwards every one of
// them to the original unchanged -- this preserves the x64 calling
// convention regardless of how many the real function actually uses,
// without us needing to know the exact signature.

constexpr uintptr_t RVA_CHAR_READER = 0x56360; // FUN_140056360

typedef uint64_t(*CharReader_t)(uint64_t, uint64_t, uint64_t, uint64_t);
static CharReader_t Real_CharReader = nullptr;

static uintptr_t g_charReaderWatchedBuffer = 0;
static uintptr_t g_charReaderModuleBase = 0; // needed to turn _ReturnAddress() into a readable RVA
static uint64_t g_charReaderCallCount = 0;
static uint64_t g_charReaderLoggedCount = 0;

void SetCharReaderWatchedBuffer(uintptr_t watchedAddr) {
    g_charReaderWatchedBuffer = watchedAddr;
    char buf[128];
    snprintf(buf, sizeof(buf), "[CharReader] Watching calls with slot_addr=0x%llx\n",
             (unsigned long long)watchedAddr);
    DebugOut(buf);
}

static uint64_t Detour_CharReader(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9) {
    // Capture this FIRST -- _ReturnAddress() reports whoever called
    // Detour_CharReader itself (fixed at function entry, in this
    // frame's return-address slot). This is exactly the "orchestrator"
    // function we're hunting for: FUN_140056360 stopping at the [CA]
    // token turned out to be NORMAL (confirmed identical in unpatched
    // gameplay), so the real bug must be in whichever caller decides
    // "is this dialogue done, can we advance" -- and that caller is
    // whoever shows up here.
    void* retAddr = _ReturnAddress();

    g_charReaderCallCount++;

    // DIAGNOSTIC: unconditional heartbeat, completely independent of the
    // buffer filter below. If this NEVER appears in the log, the hook
    // isn't actually being reached at all (RVA might have drifted after
    // a game update, or the detour isn't wired up correctly) -- that's
    // a very different problem than "the filter never matches", and
    // this is how we tell the two apart.
    if (g_charReaderCallCount == 1 || (g_charReaderCallCount % 500) == 0) {
        char hb[128];
        snprintf(hb, sizeof(hb), "[CharReader][HEARTBEAT] total calls so far: %llu\n",
                 (unsigned long long)g_charReaderCallCount);
        DebugOut(hb);
    }

    // Peek at the fields we understand, defensively -- RCX might not
    // always point at a struct shaped like the one we expect (this
    // function is shared by multiple text subsystems), so guard every
    // read with SEH rather than risk crashing the game over a
    // diagnostic log.
    uint64_t field18 = 0, field20 = 0, field28 = 0;
    bool readOk = true;
    __try {
        field18 = *reinterpret_cast<uint64_t*>(rcx + 0x18);
        field20 = *reinterpret_cast<uint64_t*>(rcx + 0x20);
        field28 = *reinterpret_cast<uint64_t*>(rcx + 0x28);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }

    uint64_t result = Real_CharReader(rcx, rdx, r8, r9);

    if (readOk && g_charReaderWatchedBuffer != 0 && field18 == g_charReaderWatchedBuffer) {
        __try {
            uint16_t rawCode = *reinterpret_cast<uint16_t*>(field18 + (uint64_t)field20 * 2);
            char f3e = *reinterpret_cast<char*>(rcx + 0x3e);
            char f3f = *reinterpret_cast<char*>(rcx + 0x3f);
            char f40 = *reinterpret_cast<char*>(rcx + 0x40);
            char f41 = *reinterpret_cast<char*>(rcx + 0x41);
            uint16_t f34 = *reinterpret_cast<uint16_t*>(rcx + 0x34);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "[CharReader][FIELDS] idx=%u code=0x%04x [+0x3e]=%d [+0x3f]=%d [+0x40]=%d [+0x41]=%d [+0x34]=0x%04x\n",
                (unsigned)field20, (unsigned)rawCode,
                (int)f3e, (int)f3f, (int)f40, (int)f41, (unsigned)f34);
            DebugOut(buf);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Only log calls that are actually about the entry we're watching
    // (field18 matching our known buffer pointer) -- otherwise this
    // fires thousands of times a second for completely unrelated UI
    // text and drowns out anything useful.
    if (readOk && g_charReaderWatchedBuffer != 0 && field18 == g_charReaderWatchedBuffer) {
        g_charReaderLoggedCount++;
        uintptr_t retRva = (g_charReaderModuleBase != 0)
            ? (reinterpret_cast<uintptr_t>(retAddr) - g_charReaderModuleBase)
            : 0;
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "[CharReader] #%llu (total=%llu) rcx=0x%llx f18=0x%llx f20(idx)=0x%llx f28=0x%llx "
                 "result=0x%llx CALLER_RVA=0x%llx (abs=0x%llx)\n",
                 (unsigned long long)g_charReaderLoggedCount, (unsigned long long)g_charReaderCallCount,
                 (unsigned long long)rcx, (unsigned long long)field18, (unsigned long long)field20,
                 (unsigned long long)field28, (unsigned long long)result,
                 (unsigned long long)retRva, (unsigned long long)reinterpret_cast<uintptr_t>(retAddr));
        DebugOut(buf);
    }

    return result;
}

bool InstallCharReaderHook(uintptr_t moduleBase) {
	if (!IsDebugModeOn) return true; // diagnostic-only hook
    g_charReaderModuleBase = moduleBase;
    uintptr_t addr = moduleBase + RVA_CHAR_READER;
    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
                                  reinterpret_cast<LPVOID>(&Detour_CharReader),
                                  reinterpret_cast<LPVOID*>(&Real_CharReader));
    if (s1 != MH_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[CharReader] MH_CreateHook failed: status %d\n", (int)s1);
        DebugOut(buf);
        return false;
    }
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    char buf[128];
    snprintf(buf, sizeof(buf), "[CharReader] Hook installed (rva=0x%llx) -- MH_EnableHook status %d\n",
             (unsigned long long)RVA_CHAR_READER, (int)s2);
    DebugOut(buf);
    return s2 == MH_OK;
}
