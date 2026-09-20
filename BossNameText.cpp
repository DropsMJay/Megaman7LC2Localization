// BossNameText.cpp
#include "pch.h"
#include "BossNameText.h"
#include "Logging.h"
#include "resource.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <MinHook.h>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <intrin.h>

using json = nlohmann::json;

extern HMODULE g_selfModule; // defined in dllmain.cpp

// -----------------------------------------------------------------------
// BIG FONT (boss-name banner). Fixed, static tile table -- confirmed
// identical across game sessions. Each letter is TWO stacked 8x8 tiles
// (top half at index N, bottom half at N+16).
// -----------------------------------------------------------------------
static const std::map<char, int32_t> kBigFontLetterToTop = {
    {'A',0},{'B',1},{'C',2},{'D',3},{'E',4},{'F',5},{'G',6},{'H',7},{'I',8},
    {'J',9},{'K',10},{'L',11},{'M',12},{'N',13},{'O',14},{'P',15},{'R',32},{'S',33},
    {'T',34},{'U',35},{'Z',36},{'Y',66},
};
static const std::map<int32_t, char> kBigFontTopToLetter = {
    {0,'A'},{1,'B'},{2,'C'},{3,'D'},{4,'E'},{5,'F'},{6,'G'},{7,'H'},{8,'I'},
    {9,'J'},{10,'K'},{11,'L'},{12,'M'},{13,'N'},{14,'O'},{15,'P'},{32,'R'},{33,'S'},
    {34,'T'},{35,'U'},{36,'Z'},{66,'Y'},
};

// -----------------------------------------------------------------------
// Digits and other special glyphs, decoded from a full capture of the
// intro's "IN THE YEAR 20XXAD" line. Unlike normal letters, these do
// NOT follow the "bottom = top + 16" rule:
//
// - Digits: bottom = top + 1 (not +16). Only 0 and 2 are confirmed (the
//   only digits that appear in any of the game's original big-font
//   text). Other digits are presumably elsewhere on the tile sheet but
//   we have no captured evidence for them -- treated as unsupported
//   (same as an unmapped letter) until someone captures a screen that
//   uses them.
// - 'X': a SINGLE 16x16 sprite (not two stacked 8x8 tiles) -- occupies
//   one array slot at the TOP row's Y only, no separate bottom entry.
//   Confirmed via capture: subCount (s) was 1 for X, vs 0 for normal
//   paired letters and digits.
// - Ellipsis ("..."): each dot is its own single 8x8 tile (100), at a
//   Y position 2px below that line's bottom-row Y (e.g. bottom=16 ->
//   dot Y=14), spaced 6px apart from each other. This is inferred from
//   exactly ONE captured example (the "..." after "THEIR MASTER" in the
//   intro) -- the Y-offset-from-bottom rule in particular hasn't been
//   cross-checked against a second example, so treat it as a
//   best-effort guess that may need adjusting once tested in-game.
// -----------------------------------------------------------------------
static const std::map<char, int32_t> kBigFontDigitToTop = { {'0',103}, {'2',105} };
constexpr int32_t kBigFontDefaultLetterWidth = 8; // moved up here -- needed by GetBigFontDigitWidth below
// Widths measured from the "IN THE YEAR 20XXAD" capture: '0' is
// notably narrower than the 8px default (confirmed: X.x - 0.x = 4).
// '2' matches the 8px default exactly (0.x - 2.x = 8), so it doesn't
// need its own entry.
static const std::map<char, int32_t> kBigFontDigitWidth = { {'0',4} };
static int32_t GetBigFontDigitWidth(char digit)
{
    auto it = kBigFontDigitWidth.find(digit);
    return it != kBigFontDigitWidth.end() ? it->second : kBigFontDefaultLetterWidth;
}
constexpr int32_t kBigFontXTile = 64;
constexpr int32_t kBigFontXWidth = 9; // measured: two adjacent X's in "20XXAD" were 9px apart
constexpr int32_t kBigFontXWidthBeforeLetter = 13; // measured: X followed by A in "20XXAD" was 13px apart
// NOTE: X's width, like the small font's T, is a kerning-pair-style
// value, not a fixed VWF one -- see kBigFontXWidthBeforeLetter usage
// in BuildReplacementStrObjBigFontIntro. Only X-then-X and X-then-letter
// are confirmed so far; other neighbor combinations are unmeasured.
constexpr int32_t kBigFontEllipsisTile = 100;
constexpr int32_t kBigFontEllipsisDotWidth = 6; // measured: dots were 6px apart from each other
constexpr int32_t kBigFontEllipsisYOffsetFromBottom = -2; // measured, single example only

// -----------------------------------------------------------------------
// JAPANESE kana/kanji (big font). Tile numbers originally came from
// reading positions directly off the raw BOSSNAME.bin texture
// (confirmed 4bpp linear, 16 tiles/row, 8x8 each), later corrected
// against real gameplay captures where they disagreed (see below --
// most kana turned out to be single-tile, not top/bottom paired like
// Latin letters, despite how the texture looks visually).
// -----------------------------------------------------------------------
static const std::map<std::string, int32_t> kKanaToTop = {
    { "\xE3\x81\x97", 67 }, // し (shi) -- the ONLY kana confirmed to actually pair (top=67/bottom=83)
};
// Kana/kanji + Japanese punctuation that render as a SINGLE tile, no
// top/bottom pairing. "。" (period) happens to be the exact same tile
// as the big font's "..." ellipsis dot (both are just a small dot
// glyph). か (ka) was initially assumed to pair like a Latin letter
// (its atlas position visually spans both rows), but TWO independent
// full-session captures of "しかし..." -- including one where the
// person deliberately waited several seconds for the text to fully
// settle before closing the game, to rule out a fade-in timing
// artifact -- consistently showed only tile 68 for か, never a paired
// 84. That turned out to be the norm, not the exception: a full
// stabilized capture of "そして、数ヶ月後・・・" (confirmed by a
// community member who reads Japanese to include the "、" comma) has
// EXACTLY 12 tiles, matching perfectly if そ, て, 月, and 後 are ALSO
// single-tile (only し pairs): dots(3) + 後(1) + 月(1) + ヶ(1) + 数(1)
// + 、(1) + て(1) + し(2, paired) + そ(1) = 12. So 数=74 and ヶ=102 are
// now confirmed too, moved here from "unmapped".
static const std::map<std::string, int32_t> kKanaPunctuationToTile = {
    { "\xE3\x80\x82", kBigFontEllipsisTile }, // 。 (period) -- same tile as an ellipsis dot
    { "\xE3\x80\x81", 101 },                  // 、 (comma)
    { "\xE3\x81\x8B", 68 },                   // か (ka)
    { "\xE3\x81\x9D", 70 },                   // そ (so)
    { "\xE3\x81\xA6", 72 },                   // て (te)
    { "\xE6\x95\xB0", 74 },                   // 数 (kazu/suu, "number")
    { "\xE3\x83\xB6", 102 },                  // ヶ (small ka)
    { "\xE6\x9C\x88", 76 },                   // 月 (tsuki/getsu, "month")
    { "\xE5\xBE\x8C", 78 },                   // 後 (go/ato, "after")
};
// No width measurements exist for any kana/punctuation yet (nothing
// has been tested on-screen) -- default to the same 8px every
// unmeasured Latin letter uses, until someone captures a real example.
constexpr int32_t kKanaDefaultWidth = 8;

// Returns the byte length (1-4) of the UTF-8 sequence starting at
// `leadByte`, per the standard leading-byte bit patterns. Falls back to
// 1 for anything malformed (never advances by 0, avoiding an infinite
// loop on bad input).
static int Utf8SequenceLength(unsigned char leadByte)
{
    if ((leadByte & 0x80) == 0x00) return 1;
    if ((leadByte & 0xE0) == 0xC0) return 2;
    if ((leadByte & 0xF0) == 0xE0) return 3;
    if ((leadByte & 0xF8) == 0xF0) return 4;
    return 1;
}

// NOTE: 'A' was previously listed here at 9px, but a full capture of
// "IN THE YEAR 20XXAD" (which has two independent A's -- one in
// "YEAR", one in "XXAD") measured BOTH at exactly 8px (D.x - A.x = 8,
// and R.x - A.x = 8). 8 is also the default, so 'A' no longer needs a
// special-case entry at all -- removed rather than left at a wrong
// value.
static const std::map<char, int32_t> kBigFontLetterWidth = { {'O',7} };
constexpr int32_t kBigFontSpaceWidth = 7;
static int32_t GetBigFontLetterWidth(char letter)
{
    auto it = kBigFontLetterWidth.find(letter);
    return it != kBigFontLetterWidth.end() ? it->second : kBigFontDefaultLetterWidth;
}

// -----------------------------------------------------------------------
// SMALL FONT (flavor-text line, JP-only in the unmodified game).
//
// UPDATE: previously assumed this font's tile numbering changed every
// game session (based on early captures made DURING the line's fade-in
// animation, before it settles -- those captures were unreliable, not
// evidence of real per-session randomness). A teammate built a
// complete letter table by hand from the original SNES ROM (running it
// multiple times to confirm it never changes there), and found TWO
// copies of the font in ROM exactly 256 tiles apart -- the exact same
// "+256" relationship already confirmed for the big font (intro vs.
// boss-name banner). Cross-checking against every stable value we'd
// captured from the PORT across many separate game sessions in this
// project, 21 of 22 matched the "ROM table minus 256" copy exactly
// (the lone mismatch, L, is presumed to be a transcription slip
// somewhere in this project's notes and needs reconfirming). This is
// strong enough evidence that the port's small font is ALSO static, not
// per-session -- so we now hardcode it directly instead of learning it
// live. K, Q, V, X, Z do not exist in this font at all (confirmed:
// none of the 8 original flavor-text lines need them, and they're not
// present on the ROM tile sheet either).
// -----------------------------------------------------------------------
static const std::map<char, int32_t> kSmallFontLetterToTile = {
    {'A',37},{'B',38},{'C',39},{'D',40},{'E',41},{'F',42},{'G',43},
    {'H',44},{'I',45},{'J',46},{'L',48},{'M',54},{'N',55},{'O',56},
    {'P',57},{'R',59},{'S',60},{'T',61},{'U',62},{'W',96},{'!',97},
    {'Y',98},{'\'',99},
};

// Widths (in pixels) -- measured from stable "DON'T SLIP!" /
// "WATCH YOUR STEP" captures. Letters not in this table default to
// 8px. Only single-line text is supported for now.
//
// NOTE: T was originally measured as 4px (captured right next to an
// apostrophe, in "DON'T"), but every glyph tile is 8x8 -- any width
// below 8 causes visible overlap with the next character, which is
// probably an artifact of that specific T+apostrophe pairing (similar
// to other pairing anomalies found elsewhere in this font) rather than
// T's real standalone width. Letting it default to 8 instead.
static const std::map<char, int32_t> kSmallFontLetterWidth = {
    {'!',6}, {'P',6}, {'I',6}, {'L',8}, {'\'',8}, {'N',8}, {'O',8},
};
constexpr int32_t kSmallFontDefaultLetterWidth = 8;
constexpr int32_t kSmallFontSpaceWidth = 7;
static int32_t GetSmallFontLetterWidth(char letter)
{
    auto it = kSmallFontLetterWidth.find(letter);
    return it != kSmallFontLetterWidth.end() ? it->second : kSmallFontDefaultLetterWidth;
}

static int64_t g_flavorTextPatchedCache[8] = {}; // 0 = not built yet

// -----------------------------------------------------------------------
// The 16 ORIGINAL strings (8 boss names + 8 flavor-text lines).
// -----------------------------------------------------------------------
struct OriginalEntry {
    const char* original; // spaces included
    bool isBigFont;
};

static const OriginalEntry kOriginalBossNames[8] = {
    {"FREEZE MAN", true}, {"JUNK MAN", true}, {"BURST MAN", true}, {"CLOUD MAN", true},
    {"SPRING MAN", true}, {"SLASH MAN", true}, {"SHADE MAN", true}, {"TURBO MAN", true},
};
static const OriginalEntry kOriginalFlavorText[8] = {
    {"DON'T SLIP!", false}, {"FORGOTTEN FACTORY", false}, {"BOMB BOMBER BOMBEST", false},
    {"WATCH YOUR STEP", false}, {"BOYOYON PARADISE", false}, {"JURASSIC JUNGLE", false},
    {"MYSTERY! THE HORROR", false}, {"CHAMP OF THE ROADS", false},
};

// -----------------------------------------------------------------------
// Intro cutscene text (big font, same font as the boss-name banner --
// NOT the small flavor-text font). 3 screens, decoded from full
// captures: "IN THE YEAR 20XXAD" -> "BUT..." -> "BEGIN SEARCHING FOR /
// THEIR MASTER..." (native 2-line, '\n' marks the break). '.' and 'X'
// are literal characters here, handled specially by the intro
// tokenizer/builder (see BuildReplacementStrObjBigFontIntro) since they
// don't follow the normal 2-tiles-per-letter pattern.
//
// JAPANESE: NOT implemented yet. The JP build reuses the English "IN
// THE YEAR 20XXAD" verbatim (no translation needed there), but the
// other two screens ("BUT..." and "BEGIN SEARCHING..." equivalents,
// shown in kana/kanji in the JP build) use a font region we have NO
// tile mapping for -- unlike the small flavor-text font, nobody has
// captured/decoded the kana glyphs on this font sheet yet. Patching
// those requires the same process used for the English lines: capture
// CHARDUMP data for those 2 JP screens (diagnostic mode,
// GlyphDrawDebug.cpp), and build a kana->tile table from it. Until then
// g_introReplacementJP[1] and [2] are simply inert (empty = "leave
// untouched" like every other slot in this file), and there's no
// identification logic below for them either -- the identify step
// would need matching kana tiles, same chicken-and-egg problem.
// -----------------------------------------------------------------------
// firstGlyphOrderSwapped: true only for the ONE line where a full
// capture confirmed the array-order anomaly (the "BEGIN
// SEARCHING.../THEIR MASTER..." 2-line screen -- see the comment on
// ComputeExpectedBigFontTileSequence). Do NOT assume this applies to
// every line -- it doesn't; lines 0 and 1 were captured and confirmed
// working WITHOUT this swap. This is a per-case quirk, same as the
// small font's "WATCH" anomaly: no general rule, only apply where
// there's direct evidence.
struct OriginalIntroLine { const char* original; bool firstGlyphOrderSwapped; };
static const OriginalIntroLine kOriginalIntroLinesEN[3] = {
    { "IN THE YEAR 20XXAD", false },
    { "BUT...", false },
    { "BEGIN SEARCHING FOR\nTHEIR MASTER...", true },
};
static std::string g_introReplacementEN[3];

// Japanese build. Screen 0 reuses the English "IN THE YEAR 20XXAD"
// verbatim (confirmed -- no translation exists for it). Screens 1 and
// 2 are read off actual gameplay screenshots + the raw BOSSNAME.bin
// texture (see kKanaToTop above for how the tile numbers were found).
//
// Screen 2: "そして、数ヶ月後・・・" -- CONFIRMED to have a "、" comma
// after て (a community member who can read Japanese confirmed this
// explicitly after some back-and-forth; an earlier guess in this file
// briefly assumed no comma at all, which was wrong -- reverted).
// "数" and "ヶ" were the last unmapped glyphs; a full stabilized
// capture of this exact line (count=12) matched perfectly once そ, て,
// 月, and 後 were corrected to single-tile too (see
// kKanaPunctuationToTile) -- 数=74 and ヶ=102 are now confirmed by that
// match, not just a texture-position guess.
//
// Screen 1 ("しかし・・・") only needs し/か, both confirmed, so it
// should work today.
//
// firstGlyphOrderSwapped is set false for both JP lines -- neither has
// been captured/confirmed to have (or not have) the anomaly yet, so
// false (no swap) is the safe default until there's real evidence
// either way.
static const OriginalIntroLine kOriginalIntroLinesJP[3] = {
    { "IN THE YEAR 20XXAD", false },
    { "\xE3\x81\x97\xE3\x81\x8B\xE3\x81\x97...", false }, // しかし...
    { "\xE3\x81\x9D\xE3\x81\x97\xE3\x81\xA6\xE3\x80\x81\xE6\x95\xB0\xE3\x83\xB6\xE6\x9C\x88\xE5\xBE\x8C...", false }, // そして、数ヶ月後...
};
static std::string g_introReplacementJP[3];

static std::string g_bossNameReplacement[8];
static std::string g_flavorTextReplacement[8];
static int g_lastKnownBossIndex = -1;

static std::string StripSpaces(const std::string& s)
{
    std::string out;
    for (char c : s) if (c != ' ') out += c;
    return out;
}

// -----------------------------------------------------------------------
// Near-module allocator (same pattern as EnglishText.cpp).
// -----------------------------------------------------------------------
static uint8_t* g_nearModuleBlock = nullptr;
static size_t g_nearModuleBlockUsed = 0;
static size_t g_nearModuleBlockSize = 0;

static uint8_t* AllocateNearModule(uintptr_t moduleBase, size_t totalBytesNeeded)
{
    const intptr_t offsets[] = {
        0x10000000, -0x10000000, 0x20000000, -0x20000000,
        0x40000000, -0x40000000, 0x60000000, -0x60000000,
    };
    for (intptr_t off : offsets) {
        LPVOID hint = reinterpret_cast<LPVOID>(moduleBase + off);
        LPVOID mem = VirtualAlloc(hint, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mem != nullptr) {
            LogLine("[BossNameText] Allocated block near module base: 0x%llx (offset 0x%llx)\n",
                (unsigned long long)mem, (unsigned long long)off);
            return reinterpret_cast<uint8_t*>(mem);
        }
    }
    LPVOID mem = VirtualAlloc(nullptr, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LogLine("[BossNameText] Could not allocate near the module base -- using a generic address.\n");
    return reinterpret_cast<uint8_t*>(mem);
}
static uint8_t* BumpAlloc(size_t bytes, size_t alignment = 8)
{
    size_t aligned = (g_nearModuleBlockUsed + (alignment - 1)) & ~(alignment - 1);
    if (aligned + bytes > g_nearModuleBlockSize) return nullptr;
    uint8_t* result = g_nearModuleBlock + aligned;
    g_nearModuleBlockUsed = aligned + bytes;
    return result;
}

// -----------------------------------------------------------------------
// JSON loading
// -----------------------------------------------------------------------
void LoadBossNameLocalization()
{
    HRSRC hRes = FindResource(g_selfModule, MAKEINTRESOURCE(IDR_BOSSNAMETEXT), RT_RCDATA);
    if (!hRes) { LogLine("[BossNameText] FindResource failed for BossNameText.json\n"); return; }

    HGLOBAL hData = LoadResource(g_selfModule, hRes);
    DWORD size = SizeofResource(g_selfModule, hRes);
    const char* rawData = static_cast<const char*>(LockResource(hData));
    if (!hData || !rawData || size == 0) {
        LogLine("[BossNameText] LoadResource/LockResource failed for BossNameText.json\n");
        return;
    }

    json data;
    try { data = json::parse(rawData, rawData + size); }
    catch (const json::parse_error& e) {
        LogLine("[BossNameText] JSON parse error: %s\n", e.what());
        return;
    }

    if (data.contains("boss_names")) {
        for (auto& [key, value] : data["boss_names"].items()) {
            size_t idx = std::stoul(key);
            if (idx < 8) g_bossNameReplacement[idx] = value.get<std::string>();
        }
    }
    if (data.contains("flavor_text")) {
        for (auto& [key, value] : data["flavor_text"].items()) {
            size_t idx = std::stoul(key);
            if (idx < 8) g_flavorTextReplacement[idx] = value.get<std::string>();
        }
    }
    if (data.contains("intro_en")) {
        for (auto& [key, value] : data["intro_en"].items()) {
            size_t idx = std::stoul(key);
            if (idx < 3) g_introReplacementEN[idx] = value.get<std::string>();
        }
    }
    if (data.contains("intro_jp")) {
        for (auto& [key, value] : data["intro_jp"].items()) {
            size_t idx = std::stoul(key);
            if (idx < 3) g_introReplacementJP[idx] = value.get<std::string>();
        }
    }

    LogLine("[BossNameText] BossNameText.json loaded.\n");

    size_t totalBytesNeeded = 16 * (48 * (0x48 + 8) + 0x50);
    g_nearModuleBlockSize = totalBytesNeeded;

    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    g_nearModuleBlock = AllocateNearModule(moduleBase, totalBytesNeeded);
    g_nearModuleBlockUsed = 0;
}

// -----------------------------------------------------------------------
// Safe memory access helpers
// -----------------------------------------------------------------------
template<typename T>
static bool SafeRead(uintptr_t addr, T* out)
{
    __try { *out = *reinterpret_cast<T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeCopyFrom(uintptr_t src, void* dst, size_t size)
{
    __try { memcpy(dst, reinterpret_cast<void*>(src), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeWriteInt64(uintptr_t addr, int64_t value)
{
    __try { *reinterpret_cast<int64_t*>(addr) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// -----------------------------------------------------------------------
// Decode (big font only -- used to identify which boss name is on
// screen, so we know which boss index is "active" for matching the
// flavor-text line that follows).
// -----------------------------------------------------------------------
static std::string DecodeBigFont(int64_t arrPtr, int32_t count)
{
    std::string decoded;
    for (int i = 0; i < count; i += 2) {
        int64_t entryTop = 0;
        int32_t tileTop = -1;
        if (SafeRead((uintptr_t)arrPtr + (i + 1) * 8, &entryTop))
            SafeRead((uintptr_t)entryTop + 0x30, &tileTop);
        auto it = kBigFontTopToLetter.find(tileTop);
        decoded += (it != kBigFontTopToLetter.end()) ? it->second : '?';
    }
    std::reverse(decoded.begin(), decoded.end());
    return decoded;
}

// -----------------------------------------------------------------------
// Build replacement (big font -- variable length supported).
// -----------------------------------------------------------------------
static int64_t BuildReplacementStrObjBigFont(int64_t origStrObj, int64_t origArrPtr, int32_t origCount,
    const std::string& newTextWithSpaces)
{
    int64_t templateEntry = 0;
    if (!SafeRead((uintptr_t)origArrPtr, &templateEntry) || templateEntry == 0) return 0;
    unsigned char templateBuf[0x48] = {};
    if (!SafeCopyFrom(templateEntry, templateBuf, sizeof(templateBuf))) return 0;

    int32_t yBottom = 0, yTop = 0;
    SafeRead((uintptr_t)templateEntry + 0x40, &yBottom);
    int64_t secondEntry = 0;
    int32_t yTop2 = 0;
    if (SafeRead((uintptr_t)origArrPtr + 8, &secondEntry) && secondEntry != 0)
        SafeRead((uintptr_t)secondEntry + 0x40, &yTop2);
    yTop = yTop2;

    int32_t startX = 0;
    SafeRead((uintptr_t)templateEntry + 0x3c, &startX);

    struct CharPos { char letter; int32_t x; };
    std::vector<CharPos> positions;
    int32_t cursorX = startX;
    for (int i = (int)newTextWithSpaces.size() - 1; i >= 0; i--) {
        char c = newTextWithSpaces[i];
        if (c == ' ') { cursorX -= kBigFontSpaceWidth; continue; }
        positions.push_back({ c, cursorX });
        cursorX -= GetBigFontLetterWidth(c);
    }

    int32_t newCount = (int32_t)positions.size() * 2;
    uint8_t* newArrPtr = BumpAlloc(newCount * sizeof(int64_t), alignof(int64_t));
    if (!newArrPtr) return 0;

    for (size_t p = 0; p < positions.size(); p++) {
        char letter = positions[p].letter;
        int32_t x = positions[p].x;
        auto it = kBigFontLetterToTop.find(letter);
        if (it == kBigFontLetterToTop.end()) return 0;
        int32_t topTile = it->second;

        for (int half = 0; half < 2; half++) {
            uint8_t* newEntry = BumpAlloc(0x48, 8);
            if (!newEntry) return 0;
            memcpy(newEntry, templateBuf, sizeof(templateBuf));
            *reinterpret_cast<int32_t*>(newEntry + 0x30) = topTile + (half == 0 ? 16 : 0);
            *reinterpret_cast<int32_t*>(newEntry + 0x3c) = x;
            *reinterpret_cast<int32_t*>(newEntry + 0x40) = (half == 0) ? yBottom : yTop;
            reinterpret_cast<int64_t*>(newArrPtr)[p * 2 + half] = (int64_t)newEntry;
        }
    }

    uint8_t* newStrObj = BumpAlloc(0x50, 8);
    if (!newStrObj) return 0;
    unsigned char strBuf[0x50] = {};
    SafeCopyFrom(origStrObj, strBuf, sizeof(strBuf));
    memcpy(newStrObj, strBuf, sizeof(strBuf));
    *reinterpret_cast<int32_t*>(newStrObj + 0x40) = newCount;
    *reinterpret_cast<int64_t*>(newStrObj + 0x48) = (int64_t)newArrPtr;

    return (int64_t)newStrObj;
}

// -----------------------------------------------------------------------
// INTRO TEXT (big font). Unlike boss names, intro lines can contain
// digits, 'X', and "..." (ellipsis) -- glyphs that don't fit the simple
// "every character = 2 stacked tiles" model DecodeBigFont/
// BuildReplacementStrObjBigFont above assume. A tokenizer + explicit
// per-glyph-kind handling covers this.
// -----------------------------------------------------------------------
enum class BigGlyphKind { Space, Letter, Digit, XGlyph, EllipsisDot, Kana, KanaPunct };
struct BigGlyphToken { BigGlyphKind kind; std::string text; }; // text: 1 byte for ASCII glyphs, multi-byte UTF-8 for kana

// "..." (exactly three literal dots) is treated as a single 3-dot
// ellipsis run; a lone '.' or ".." outside of a full run of 3 is NOT
// currently supported (no captured data for that case) and falls
// through as three-separate-dots only when it's a full "...".
//
// Any byte >= 0x80 is assumed to start a UTF-8 kana/kanji/punctuation
// sequence (2-4 bytes) -- the ORIGINAL intro-line strings and any
// replacement text from the JSON are expected to be valid UTF-8 (as
// nlohmann::json already requires for string values).
static std::vector<BigGlyphToken> TokenizeBigFontText(const std::string& text)
{
    std::vector<BigGlyphToken> tokens;
    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80) {
            int len = Utf8SequenceLength(c);
            std::string glyph = text.substr(i, (size_t)len);
            BigGlyphKind kind = kKanaPunctuationToTile.count(glyph) ? BigGlyphKind::KanaPunct : BigGlyphKind::Kana;
            tokens.push_back({ kind, glyph });
            i += (size_t)len;
            continue;
        }
        if (c == ' ') { tokens.push_back({ BigGlyphKind::Space, " " }); i++; continue; }
        if (c == '.' && i + 2 < text.size() && text[i + 1] == '.' && text[i + 2] == '.') {
            tokens.push_back({ BigGlyphKind::EllipsisDot, "." });
            tokens.push_back({ BigGlyphKind::EllipsisDot, "." });
            tokens.push_back({ BigGlyphKind::EllipsisDot, "." });
            i += 3; continue;
        }
        if (c >= '0' && c <= '9') { tokens.push_back({ BigGlyphKind::Digit, std::string(1, (char)c) }); i++; continue; }
        if (c == 'X') { tokens.push_back({ BigGlyphKind::XGlyph, "X" }); i++; continue; }
        tokens.push_back({ BigGlyphKind::Letter, std::string(1, (char)c) });
        i++;
    }
    return tokens;
}

// The raw tile-VALUE sequence (in array order -- i.e. right-to-left /
// last-character-first, matching how the game stores it) that a given
// string would produce. Used only to IDENTIFY which known original
// intro line is currently on screen, by comparing tile values directly
// against a live capture -- much simpler than writing a fully general
// reverse-decoder that would need to disambiguate 1-slot glyphs (X,
// ellipsis dots) from 2-slot glyphs (letters, digits) positionally.
// Returns false if the text uses a letter/digit we have no tile for.
static bool ComputeExpectedBigFontTileSequence(const std::string& textWithSpaces, bool firstGlyphOrderSwapped,
    std::vector<int32_t>* outTiles)
{
    auto tokens = TokenizeBigFontText(textWithSpaces);
    std::vector<std::vector<int32_t>> blocks; // one block per glyph, in NORMAL (left-to-right) order
    for (auto& t : tokens) {
        switch (t.kind) {
        case BigGlyphKind::Space:
            break; // spaces produce no array entry
        case BigGlyphKind::Letter: {
            auto it = kBigFontLetterToTop.find(t.text[0]);
            if (it == kBigFontLetterToTop.end()) return false;
            blocks.push_back({ it->second + 16, it->second }); // bottom, then top (matches capture order)
            break;
        }
        case BigGlyphKind::Digit: {
            auto it = kBigFontDigitToTop.find(t.text[0]);
            if (it == kBigFontDigitToTop.end()) return false;
            blocks.push_back({ it->second + 1, it->second }); // bottom = top+1 for digits, not +16
            break;
        }
        case BigGlyphKind::XGlyph:
            blocks.push_back({ kBigFontXTile });
            break;
        case BigGlyphKind::EllipsisDot:
            blocks.push_back({ kBigFontEllipsisTile });
            break;
        case BigGlyphKind::Kana: {
            auto it = kKanaToTop.find(t.text);
            if (it == kKanaToTop.end()) return false;
            blocks.push_back({ it->second + 16, it->second }); // same pairing rule as Latin letters
            break;
        }
        case BigGlyphKind::KanaPunct: {
            auto it = kKanaPunctuationToTile.find(t.text);
            if (it == kKanaPunctuationToTile.end()) return false;
            blocks.push_back({ it->second }); // single tile, no pairing
            break;
        }
        }
    }
    outTiles->clear();
    // Array order is right-to-left OVERALL (whole string reversed), but
    // WITHIN each glyph's block the bottom/top order is already correct
    // as captured -- so reverse the order of BLOCKS, not their contents.
    //
    // EXCEPTION: a full capture of "BEGIN SEARCHING FOR\nTHEIR
    // MASTER..." showed the very LAST array entries (= the FIRST
    // character of the whole text, 'B') in (top, bottom) order instead
    // of the normal (bottom, top) every other paired glyph uses. This
    // is the same kind of per-case array-order anomaly already known
    // from the small font's "WATCH" investigation (no general rule --
    // just needs checking case by case). Only confirmed for THIS ONE
    // line so far -- gated behind firstGlyphOrderSwapped, which the
    // caller sets per-line, NOT applied universally (applying it to
    // every line broke the other 2, which were already confirmed
    // working WITHOUT this swap).
    if (firstGlyphOrderSwapped && !blocks.empty() && blocks.front().size() == 2)
        std::swap(blocks.front()[0], blocks.front()[1]);
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
        for (int32_t tile : *it) outTiles->push_back(tile);
    return true;
}

// Reads the tile value of every entry in a live array, in array order,
// for comparison against ComputeExpectedBigFontTileSequence's output.
static std::vector<int32_t> ReadCapturedTileSequence(int64_t arrPtr, int32_t count)
{
    std::vector<int32_t> tiles;
    tiles.reserve(count);
    for (int i = 0; i < count; i++) {
        int64_t entry = 0;
        int32_t tile = -1;
        if (SafeRead((uintptr_t)arrPtr + i * 8, &entry) && entry != 0)
            SafeRead((uintptr_t)entry + 0x30, &tile);
        tiles.push_back(tile);
    }
    return tiles;
}

// Build replacement for INTRO text (big font). Supports an OPTIONAL
// manual line break ('\n'): everything before it becomes line 1 (top),
// everything after becomes line 2 (bottom). ALWAYS left-anchored and
// built forward (left to right) -- confirmed by a full capture of the
// game's own native 2-line intro text ("BEGIN SEARCHING FOR" / "THEIR
// MASTER..."), where both lines shared the exact same leftmost X.
//
// This is a SEPARATE function from BuildReplacementStrObjBigFont
// (boss names) on purpose: boss names are always single-line, plain
// A-Z text, and that path is already tested and proven working under
// its own (right-anchor/backward-build) scheme -- left untouched here
// to avoid any regression risk. Intro text needs the extra glyph kinds
// (digit/X/ellipsis) and the left-anchor 2-line layout, which boss
// names never need.
static int64_t BuildReplacementStrObjBigFontIntro(int64_t origStrObj, int64_t origArrPtr, int32_t origCount,
    bool firstGlyphOrderSwapped, const std::string& newTextWithNewlineAndSpaces)
{
    std::string lines[2];
    size_t newlinePos = newTextWithNewlineAndSpaces.find('\n');
    int numLines = 1;
    if (newlinePos != std::string::npos) {
        lines[0] = newTextWithNewlineAndSpaces.substr(0, newlinePos);
        lines[1] = newTextWithNewlineAndSpaces.substr(newlinePos + 1);
        numLines = 2;
    }
    else {
        lines[0] = newTextWithNewlineAndSpaces;
    }

    // The array's LAST TWO entries are the pair for the FIRST
    // (leftmost) character of the ORIGINAL text -- per the array's
    // right-to-left convention, matching the LEFT anchor we want (see
    // comment above). NOTE: this assumes the leftmost original glyph is
    // a normal 2-slot letter/digit, which holds for all 3 currently
    // known intro lines (none start with 'X' or "...").
    //
    // ORDER: normally (bottom, top). ONE confirmed exception (see
    // firstGlyphOrderSwapped on OriginalIntroLine and the matching
    // comment in ComputeExpectedBigFontTileSequence): the "BEGIN
    // SEARCHING.../THEIR MASTER..." line has this specific pair (first
    // character of the whole text) as (top, bottom) instead. Only
    // swap when the caller tells us this line is the confirmed
    // exception -- assuming it universally broke the other 2 lines,
    // which were already confirmed working with the normal order.
    int64_t entryAtN2 = 0, entryAtN1 = 0;
    if (!SafeRead((uintptr_t)origArrPtr + (uintptr_t)(origCount - 2) * 8, &entryAtN2) || entryAtN2 == 0) return 0;
    if (!SafeRead((uintptr_t)origArrPtr + (uintptr_t)(origCount - 1) * 8, &entryAtN1) || entryAtN1 == 0) return 0;
    int64_t templateBottomEntry = firstGlyphOrderSwapped ? entryAtN1 : entryAtN2;
    unsigned char templateBuf[0x48] = {};
    if (!SafeCopyFrom(templateBottomEntry, templateBuf, sizeof(templateBuf))) return 0;

    int32_t leftX = 0, bottomY0 = 0;
    SafeRead((uintptr_t)templateBottomEntry + 0x3c, &leftX);
    SafeRead((uintptr_t)templateBottomEntry + 0x40, &bottomY0); // line 0's bottom-row Y

    struct Glyph { int32_t tile; int32_t x; int32_t y; int32_t subCount; };
    std::vector<Glyph> glyphs;

    for (int lineIdx = 0; lineIdx < numLines; lineIdx++) {
        // Confirmed via capture: line 1's top/bottom Y were each
        // exactly 16 more than line 0's (letters are 16px tall, so
        // lines don't need the 8px-per-line small-font trick).
        int32_t bottomY = bottomY0 + lineIdx * 16;
        int32_t topY = bottomY - 8;
        int32_t cursorX = leftX;

        auto lineTokens = TokenizeBigFontText(lines[lineIdx]);
        for (size_t ti = 0; ti < lineTokens.size(); ti++) {
            auto& t = lineTokens[ti];
            switch (t.kind) {
            case BigGlyphKind::Space:
                cursorX += kBigFontSpaceWidth;
                break;
            case BigGlyphKind::Letter: {
                auto it = kBigFontLetterToTop.find(t.text[0]);
                if (it == kBigFontLetterToTop.end()) {
                    LogLine("[BossNameText] Cannot patch intro text -- letter '%c' doesn't exist in this font.\n", t.text[0]);
                    return 0;
                }
                glyphs.push_back({ it->second, cursorX, topY, 0 });
                glyphs.push_back({ it->second + 16, cursorX, bottomY, 0 });
                cursorX += GetBigFontLetterWidth(t.text[0]);
                break;
            }
            case BigGlyphKind::Digit: {
                auto it = kBigFontDigitToTop.find(t.text[0]);
                if (it == kBigFontDigitToTop.end()) {
                    LogLine("[BossNameText] Cannot patch intro text -- digit '%c' not confirmed in this font yet (only 0 and 2 are).\n", t.text[0]);
                    return 0;
                }
                glyphs.push_back({ it->second, cursorX, topY, 0 });
                glyphs.push_back({ it->second + 1, cursorX, bottomY, 0 }); // digits: bottom = top+1
                cursorX += GetBigFontDigitWidth(t.text[0]);
                break;
            }
            case BigGlyphKind::XGlyph: {
                // Single 16x16 sprite -- one entry, top row only, subCount=1 (matches capture).
                glyphs.push_back({ kBigFontXTile, cursorX, topY, 1 });
                // X's width isn't fixed -- a full "IN THE YEAR 20XXAD"
                // capture measured X-then-X at 9px but X-then-A at
                // 13px (kerning-pair style, like the small font's T).
                // Only these two neighbor cases are confirmed; anything
                // else (X followed by a digit, space, kana, etc.) falls
                // back to the X-X value, which may still be off until
                // measured.
                bool nextIsX = (ti + 1 < lineTokens.size() && lineTokens[ti + 1].kind == BigGlyphKind::XGlyph);
                cursorX += nextIsX ? kBigFontXWidth : kBigFontXWidthBeforeLetter;
                break;
            }
            case BigGlyphKind::EllipsisDot:
                glyphs.push_back({ kBigFontEllipsisTile, cursorX, bottomY + kBigFontEllipsisYOffsetFromBottom, 0 });
                cursorX += kBigFontEllipsisDotWidth;
                break;
            case BigGlyphKind::Kana: {
                auto it = kKanaToTop.find(t.text);
                if (it == kKanaToTop.end()) {
                    LogLine("[BossNameText] Cannot patch intro text -- kana/kanji \"%s\" not confirmed in this font yet.\n", t.text.c_str());
                    return 0;
                }
                glyphs.push_back({ it->second, cursorX, topY, 0 });
                glyphs.push_back({ it->second + 16, cursorX, bottomY, 0 }); // same pairing as Latin letters
                cursorX += kKanaDefaultWidth; // no measured kana width yet
                break;
            }
            case BigGlyphKind::KanaPunct: {
                auto it = kKanaPunctuationToTile.find(t.text);
                if (it == kKanaPunctuationToTile.end()) {
                    LogLine("[BossNameText] Cannot patch intro text -- punctuation \"%s\" not confirmed in this font yet.\n", t.text.c_str());
                    return 0;
                }
                glyphs.push_back({ it->second, cursorX, topY, 0 }); // single tile, top row Y (matches ellipsis dot placement)
                cursorX += kKanaDefaultWidth; // no measured width yet
                break;
            }
            }
        }
    }

    int32_t newCount = (int32_t)glyphs.size();
    uint8_t* newArrPtr = BumpAlloc(newCount * sizeof(int64_t), alignof(int64_t));
    if (!newArrPtr) return 0;

    for (size_t p = 0; p < glyphs.size(); p++) {
        uint8_t* newEntry = BumpAlloc(0x48, 8);
        if (!newEntry) return 0;
        memcpy(newEntry, templateBuf, sizeof(templateBuf));
        *reinterpret_cast<int32_t*>(newEntry + 0x30) = glyphs[p].tile;
        *reinterpret_cast<int32_t*>(newEntry + 0x34) = 0; // flip/orientation -- matches template default
        *reinterpret_cast<int32_t*>(newEntry + 0x38) = glyphs[p].subCount;
        *reinterpret_cast<int32_t*>(newEntry + 0x3c) = glyphs[p].x;
        *reinterpret_cast<int32_t*>(newEntry + 0x40) = glyphs[p].y;
        reinterpret_cast<int64_t*>(newArrPtr)[p] = (int64_t)newEntry;
    }

    uint8_t* newStrObj = BumpAlloc(0x50, 8);
    if (!newStrObj) return 0;
    unsigned char strBuf[0x50] = {};
    SafeCopyFrom(origStrObj, strBuf, sizeof(strBuf));
    memcpy(newStrObj, strBuf, sizeof(strBuf));
    *reinterpret_cast<int32_t*>(newStrObj + 0x40) = newCount;
    *reinterpret_cast<int64_t*>(newStrObj + 0x48) = (int64_t)newArrPtr;

    return (int64_t)newStrObj;
}

// -----------------------------------------------------------------------
// Shared by both small-font build paths below.
// -----------------------------------------------------------------------
struct SmallFontCharPos { char letter; int32_t x; int32_t y; };

static int64_t BuildFromPositionsSmallFont(int64_t origStrObj, const unsigned char templateBuf[0x48],
    const std::vector<SmallFontCharPos>& positions)
{
    int32_t newCount = (int32_t)positions.size();
    uint8_t* newArrPtr = BumpAlloc(newCount * sizeof(int64_t), alignof(int64_t));
    if (!newArrPtr) return 0;

    for (size_t p = 0; p < positions.size(); p++) {
        char letter = positions[p].letter;
        int32_t x = positions[p].x;
        int32_t y = positions[p].y;
        auto it = kSmallFontLetterToTile.find(letter);
        if (it == kSmallFontLetterToTile.end()) {
            LogLine("[BossNameText] Cannot patch flavor text -- letter '%c' doesn't exist in this font.\n", letter);
            return 0;
        }

        uint8_t* newEntry = BumpAlloc(0x48, 8);
        if (!newEntry) return 0;
        memcpy(newEntry, templateBuf, 0x48);
        *reinterpret_cast<int32_t*>(newEntry + 0x30) = it->second;
        *reinterpret_cast<int32_t*>(newEntry + 0x3c) = x;
        *reinterpret_cast<int32_t*>(newEntry + 0x40) = y;
        reinterpret_cast<int64_t*>(newArrPtr)[p] = (int64_t)newEntry;
    }

    uint8_t* newStrObj = BumpAlloc(0x50, 8);
    if (!newStrObj) return 0;
    unsigned char strBuf[0x50] = {};
    SafeCopyFrom(origStrObj, strBuf, sizeof(strBuf));
    memcpy(newStrObj, strBuf, sizeof(strBuf));
    *reinterpret_cast<int32_t*>(newStrObj + 0x40) = newCount;
    *reinterpret_cast<int64_t*>(newStrObj + 0x48) = (int64_t)newArrPtr;

    return (int64_t)newStrObj;
}

// -----------------------------------------------------------------------
// Build replacement (small font -- VARIABLE length). Uses the FIXED
// letter->tile table. Supports an OPTIONAL manual line break: if
// newTextWithSpaces contains a '\n', everything before it becomes line
// 1 (top) and everything after becomes line 2 (bottom).
//
// ANCHORING: confirmed via a full native capture that MULTI-LINE text
// ("BEGIN SEARCHING FOR" / "THEIR MASTER...", from the intro) is
// LEFT-aligned -- both lines share the exact same leftmost X, while
// their rightmost X differs by line length. But we only have that
// evidence for the 2-line case; the single-line path (e.g. "DON'T
// SLIP!" -> "WATCH OUT!!") was already tested and confirmed working
// under the OLD right-anchor/backward-build scheme, so that path is
// left untouched here -- only an actual line break switches to the new
// left-anchor/forward-build logic. This avoids risking a regression on
// the already-proven single-line case based on evidence that only
// covers the 2-line case.
// -----------------------------------------------------------------------
static int64_t BuildReplacementStrObjSmallFont(int64_t origStrObj, int64_t origArrPtr, int32_t origCount,
    const std::string& newTextWithNewlineAndSpaces)
{
    size_t newlinePos = newTextWithNewlineAndSpaces.find('\n');
    bool isTwoLine = newlinePos != std::string::npos;

    std::vector<SmallFontCharPos> positions;

    if (isTwoLine) {
        std::string line0 = newTextWithNewlineAndSpaces.substr(0, newlinePos);
        std::string line1 = newTextWithNewlineAndSpaces.substr(newlinePos + 1);
        std::string lines[2] = { line0, line1 };

        // LAST array entry = FIRST (leftmost) character, per the
        // array's normal right-to-left convention -- deliberate, we
        // want the LEFT edge here (see comment above).
        int64_t templateEntry = 0;
        if (!SafeRead((uintptr_t)origArrPtr + (uintptr_t)(origCount - 1) * 8, &templateEntry) || templateEntry == 0) return 0;
        unsigned char templateBuf[0x48] = {};
        if (!SafeCopyFrom(templateEntry, templateBuf, sizeof(templateBuf))) return 0;

        int32_t leftX = 0, baseY = 0;
        SafeRead((uintptr_t)templateEntry + 0x3c, &leftX);
        SafeRead((uintptr_t)templateEntry + 0x40, &baseY);

        // Both lines share the same left anchor (leftX), built forward
        // (left to right) -- matches the game's own left-aligned
        // multi-line layout. Write order in the array doesn't matter to
        // the renderer (each entry carries its own explicit tile/X/Y),
        // only READING existing text relies on the right-to-left
        // convention.
        for (int lineIdx = 0; lineIdx < 2; lineIdx++) {
            const std::string& line = lines[lineIdx];
            int32_t lineY = baseY + lineIdx * 8; // 8px per line, matches "WATCH"/"YOUR STEP" spacing
            int32_t cursorX = leftX;
            for (size_t i = 0; i < line.size(); i++) {
                char c = line[i];
                if (c == ' ') { cursorX += kSmallFontSpaceWidth; continue; }
                positions.push_back({ c, cursorX, lineY });
                cursorX += GetSmallFontLetterWidth(c);
            }
        }
        return BuildFromPositionsSmallFont(origStrObj, templateBuf, positions);
    }

    // ---- Single line: original, proven-working right-anchor path ----
    int64_t templateEntry = 0;
    if (!SafeRead((uintptr_t)origArrPtr, &templateEntry) || templateEntry == 0) return 0;
    unsigned char templateBuf[0x48] = {};
    if (!SafeCopyFrom(templateEntry, templateBuf, sizeof(templateBuf))) return 0;

    int32_t startX = 0, baseY = 0;
    SafeRead((uintptr_t)templateEntry + 0x3c, &startX);
    SafeRead((uintptr_t)templateEntry + 0x40, &baseY);

    int32_t cursorX = startX;
    for (int i = (int)newTextWithNewlineAndSpaces.size() - 1; i >= 0; i--) {
        char c = newTextWithNewlineAndSpaces[i];
        if (c == ' ') { cursorX -= kSmallFontSpaceWidth; continue; }
        positions.push_back({ c, cursorX, baseY });
        cursorX -= GetSmallFontLetterWidth(c);
    }
    return BuildFromPositionsSmallFont(origStrObj, templateBuf, positions);
}

// -----------------------------------------------------------------------
// The hook
// -----------------------------------------------------------------------
typedef void (*DrawChars_t)(int64_t param_1, uint64_t param_2, uint64_t* param_3,
    uint64_t param_4, int param_5, int param_6, int param_7);
static DrawChars_t Real_DrawChars = nullptr;
constexpr uintptr_t RVA_DRAW_CHARS = 0x5dd80; // FUN_14005dd80

void Detour_DrawChars(int64_t param_1, uint64_t param_2, uint64_t* param_3,
    uint64_t param_4, int param_5, int param_6, int param_7)
{
    int64_t strObj = 0;
    if (SafeRead((uintptr_t)param_1 + 0x40, &strObj) && strObj != 0) {
        int32_t count = 0;
        int64_t arrPtr = 0;
        if (SafeRead((uintptr_t)strObj + 0x40, &count) &&
            SafeRead((uintptr_t)strObj + 0x48, &arrPtr) &&
            count > 0 && count < 64) {

            // ---- Big font: identify by exact decode (static table). ----
            if (count % 2 == 0) {
                std::string decodedBig = DecodeBigFont(arrPtr, count);
                for (int i = 0; i < 8; i++) {
                    std::string origStripped = StripSpaces(kOriginalBossNames[i].original);
                    if (decodedBig == origStripped) {
                        g_lastKnownBossIndex = i;

                        if (!g_bossNameReplacement[i].empty()
                            && g_bossNameReplacement[i] != kOriginalBossNames[i].original) {
                            int64_t newStrObj = BuildReplacementStrObjBigFont(strObj, arrPtr, count,
                                g_bossNameReplacement[i]);
                            if (newStrObj != 0) {
                                SafeWriteInt64((uintptr_t)param_1 + 0x40, newStrObj);
                                LogLine("[BossNameText] Boss name patched: \"%s\" -> \"%s\"\n",
                                    decodedBig.c_str(), g_bossNameReplacement[i].c_str());
                            }
                        }
                        goto done;
                    }
                }
            }

            // ---- Intro text (big font, separate from boss names) ----
            // Identified by raw tile-VALUE sequence match (handles
            // digits/X/ellipsis/kana, which DecodeBigFont's simpler
            // letter-pair-only model above can't represent) against the
            // known EN and JP intro lines. Independent of the count%2
            // check above -- intro lines can have an ODD total entry
            // count (each "..." dot, 'X', or kana-punctuation only
            // takes 1 array slot instead of 2).
            {
                std::vector<int32_t> captured = ReadCapturedTileSequence(arrPtr, count);
                bool matched = false;
                for (int lang = 0; lang < 2 && !matched; lang++) {
                    const OriginalIntroLine* origLines = (lang == 0) ? kOriginalIntroLinesEN : kOriginalIntroLinesJP;
                    std::string* replacement = (lang == 0) ? g_introReplacementEN : g_introReplacementJP;
                    for (int i = 0; i < 3; i++) {
                        std::string introLine(origLines[i].original);
                        std::string origForMatch;
                        for (char c : introLine)
                            if (c != '\n') origForMatch += c; // '\n' doesn't affect tile identity, only Y
                        std::vector<int32_t> expected;
                        if (!ComputeExpectedBigFontTileSequence(origForMatch, origLines[i].firstGlyphOrderSwapped, &expected)) continue;
                        if (expected == captured) {
                            matched = true;
                            if (!replacement[i].empty() && replacement[i] != origLines[i].original) {
                                int64_t newStrObj = BuildReplacementStrObjBigFontIntro(strObj, arrPtr, count,
                                    origLines[i].firstGlyphOrderSwapped, replacement[i]);
                                if (newStrObj != 0) {
                                    SafeWriteInt64((uintptr_t)param_1 + 0x40, newStrObj);
                                    LogLine("[BossNameText] Intro line patched (%s #%d) -> \"%s\"\n",
                                        lang == 0 ? "EN" : "JP", i, replacement[i].c_str());
                                }
                            }
                            break;
                        }
                    }
                }
                if (matched) goto done;

                // DIAGNOSTIC: a big-font-shaped capture this large (30+
                // entries) that didn't match ANY known EN/JP intro line
                // is almost certainly a real intro screen we don't have
                // mapped (or a mapped one whose expected sequence is
                // wrong somehow) -- but the code above stays silent in
                // that case (it only logs on an actual match). That
                // silence made this hard to debug without switching to
                // GlyphDrawDebug.cpp. Log the raw captured sequence
                // here instead, deduped by content so it only appears
                // once per distinct screen, so this stays usable in
                // normal production-mode play without flooding the log.
                // Lowered from 30 to 5 -- "しかし..." (3 paired kana +
                // 3 ellipsis dots) only has 9 entries total, well under
                // the original threshold, which silently hid it from
                // this diagnostic exactly like the bug this was meant
                // to catch. 5 still excludes the smallest single/double
                // -glyph UI elements seen in earlier captures (counts
                // of 1-4).
                if (count >= 5) {
                    static std::unordered_set<std::string> seenUnmatched;
                    std::string key;
                    for (int32_t t : captured) key += std::to_string(t) + ",";
                    if (seenUnmatched.insert(key).second) {
                        std::string dump = "[BossNameText] Unmatched big-font capture, count=" + std::to_string(count) + ", tiles=[" + key + "]\n";
                        LogLine("%s", dump.c_str());
                    }
                }
            }

            // ---- Small font ----
            // No more live learning or fade-stability wait needed --
            // the letter->tile table is fixed. We still need to know
            // WHICH of the 8 flavor lines this is, which we infer from
            // the currently-known boss (set above) plus a matching
            // character count.
            {
                int matchedIndex = -1;
                if (g_lastKnownBossIndex >= 0) {
                    std::string candidate = StripSpaces(kOriginalFlavorText[g_lastKnownBossIndex].original);
                    if ((int)candidate.size() == count) {
                        matchedIndex = g_lastKnownBossIndex;
                    }
                }

                if (matchedIndex >= 0) {
                    if (g_flavorTextPatchedCache[matchedIndex] != 0) {
                        SafeWriteInt64((uintptr_t)param_1 + 0x40, g_flavorTextPatchedCache[matchedIndex]);
                        goto done;
                    }

                    if (!g_flavorTextReplacement[matchedIndex].empty()
                        && g_flavorTextReplacement[matchedIndex] != kOriginalFlavorText[matchedIndex].original) {

                        int64_t newStrObj = BuildReplacementStrObjSmallFont(strObj, arrPtr, count,
                            g_flavorTextReplacement[matchedIndex]);
                        if (newStrObj != 0) {
                            g_flavorTextPatchedCache[matchedIndex] = newStrObj;
                            SafeWriteInt64((uintptr_t)param_1 + 0x40, newStrObj);
                            LogLine("[BossNameText] Flavor text patched (boss #%d): -> \"%s\"\n",
                                matchedIndex, g_flavorTextReplacement[matchedIndex].c_str());
                        }
                    }
                }
            }
        }
    }
done:
    Real_DrawChars(param_1, param_2, param_3, param_4, param_5, param_6, param_7);
}

bool InstallBossNameHook(uintptr_t moduleBase)
{
    uintptr_t addr = moduleBase + RVA_DRAW_CHARS;
    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
        reinterpret_cast<LPVOID>(&Detour_DrawChars),
        reinterpret_cast<LPVOID*>(&Real_DrawChars));
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    LogLine("[BossNameText] InstallBossNameHook @ RVA 0x%llx (addr %p) -> create=%d enable=%d\n",
        (unsigned long long)RVA_DRAW_CHARS, (void*)addr, (int)s1, (int)s2);
    return s1 == MH_OK && s2 == MH_OK;
}