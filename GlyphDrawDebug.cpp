#include "pch.h"
#include "GlyphDrawDebug.h"
#include "Logging.h"
#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <intrin.h>
#include <map>
#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>

constexpr uintptr_t RVA_DRAW_CHARS = 0x5dd80; // FUN_14005dd80

typedef void (*DrawChars_t)(int64_t param_1, uint64_t param_2, uint64_t* param_3,
    uint64_t param_4, int param_5, int param_6, int param_7);
static DrawChars_t Real_DrawChars = nullptr;

template<typename T>
static bool SafeRead(uintptr_t addr, T* out)
{
    __try { *out = *reinterpret_cast<T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// -----------------------------------------------------------------------
// Safe, truncation-only buffer append. NEVER aborts the process, unlike
// plain sprintf_s: when sprintf_s can't fit the full formatted output in
// the remaining space, MSVC's default invalid-parameter handler calls
// abort() -- a real crash, not a graceful failure. This hook fires every
// frame for MANY different UI elements (not just boss-name/flavor-text --
// confirmed by seeing plenty of unrelated short tile clusters in capture
// logs), and `count` can be as high as 63, so a long enough string WILL
// eventually overflow a fixed 2048-byte line buffer. _vsnprintf_s with
// _TRUNCATE truncates safely instead of aborting. Returns false once the
// buffer is full (caller should stop appending more entries).
// -----------------------------------------------------------------------
static bool AppendSafe(char* buf, size_t bufSize, int& pos, const char* fmt, ...)
{
    if (pos < 0 || (size_t)pos >= bufSize) return false;
    va_list args;
    va_start(args, fmt);
    int written = _vsnprintf_s(buf + pos, bufSize - (size_t)pos, _TRUNCATE, fmt, args);
    va_end(args);
    if (written < 0) { pos = (int)bufSize; return false; } // ran out of room -- buffer is full
    pos += written;
    return true;
}

// -----------------------------------------------------------------------
// Best-effort decode tables, copied from the FIXED tables confirmed in
// BossNameText.cpp (see HANDOFF). Kept as a separate copy here on
// purpose -- GlyphDrawDebug.cpp and BossNameText.cpp are mutually
// exclusive in the build (production vs diagnostic mode), so this file
// can't depend on the other TU. These are for READABILITY of the log
// only (best-effort '?' fallback for unknown tiles) -- not used for any
// patching logic.
//
// BIG FONT: two stacked 8x8 tiles per letter (top = N, bottom = N+16).
// SMALL FONT: one tile per letter, no stacking.
// -----------------------------------------------------------------------
static const std::map<int32_t, char> kBigFontTopToLetter = {
    {0,'A'},{1,'B'},{2,'C'},{3,'D'},{4,'E'},{5,'F'},{7,'H'},{8,'I'},
    {9,'J'},{10,'K'},{11,'L'},{12,'M'},{13,'N'},{14,'O'},{32,'R'},
    {34,'T'},{35,'U'},{36,'Z'},{66,'Y'},
};
static const std::map<int32_t, char> kSmallFontTileToLetter = {
    {37,'A'},{38,'B'},{39,'C'},{40,'D'},{41,'E'},{42,'F'},{43,'G'},
    {44,'H'},{45,'I'},{46,'J'},{48,'L'},{54,'M'},{55,'N'},{56,'O'},
    {57,'P'},{59,'R'},{60,'S'},{61,'T'},{62,'U'},{96,'W'},{97,'!'},
    {98,'Y'},{99,'\''},
};

// Best-effort per-entry decode: tries small font first (single tile),
// falls back to big font top-tile lookup (works fine even without
// pairing information -- if it's actually a big-font BOTTOM tile, this
// will just print '?' for that entry, which is fine for a diagnostic
// dump; the point is readability, not perfect accuracy).
static char DecodeTileBestEffort(int32_t tile)
{
    auto itSmall = kSmallFontTileToLetter.find(tile);
    if (itSmall != kSmallFontTileToLetter.end()) return itSmall->second;
    auto itBig = kBigFontTopToLetter.find(tile);
    if (itBig != kBigFontTopToLetter.end()) return itBig->second;
    return '?';
}

void Detour_DrawChars(int64_t param_1, uint64_t param_2, uint64_t* param_3,
    uint64_t param_4, int param_5, int param_6, int param_7)
{
    // NOTE: was a fixed char[256][2048] array with a linear strcmp scan
    // and a hard cap of 256 unique lines. That cap was actually getting
    // hit in practice -- a persistent animated background/watermark
    // element (unrelated to real text, drawn via this same generic tile
    // function) produces a new unique dump on every frame while it
    // animates, silently eating the whole 256-slot budget before the
    // game ever reaches the text we actually care about (e.g. intro
    // lines past the first one). Switched to an unordered_set of
    // std::string with no practical cap and O(1) average lookup instead
    // of a linear scan.
    static std::unordered_set<std::string> s_seenLines;

    int64_t strObj = 0;
    if (SafeRead((uintptr_t)param_1 + 0x40, &strObj) && strObj != 0) {
        int32_t count = 0;
        int64_t arrPtr = 0;
        if (SafeRead((uintptr_t)strObj + 0x40, &count) &&
            SafeRead((uintptr_t)strObj + 0x48, &arrPtr) &&
            count > 0 && count < 64) {

            struct RawEntry { int64_t entry; int32_t tile, flip, subCount, x, y; };
            std::vector<RawEntry> raw;
            raw.reserve(count);

            char line[6144] = {};
            int pos = 0;
            AppendSafe(line, sizeof(line), pos,
                "CHARDUMP strObj=%p arrPtr=%p count=%d base=(%d,%d) entries=[",
                (void*)strObj, (void*)arrPtr, count, param_5, param_6);

            constexpr size_t kCloseMargin = 96;
            bool truncated = false;
            int i = 0;
            for (; i < count; i++) {
                int64_t entry = 0;
                int32_t tile = -1, flip = -1, subCount = -1, offX = -9999, offY = -9999;
                if (SafeRead((uintptr_t)arrPtr + i * 8, &entry) && entry != 0) {
                    SafeRead((uintptr_t)entry + 0x30, &tile);
                    SafeRead((uintptr_t)entry + 0x34, &flip);
                    SafeRead((uintptr_t)entry + 0x38, &subCount);
                    SafeRead((uintptr_t)entry + 0x3c, &offX);
                    SafeRead((uintptr_t)entry + 0x40, &offY);
                }
                raw.push_back({ entry, tile, flip, subCount, offX, offY });
                if ((size_t)pos >= sizeof(line) - kCloseMargin) {
                    // Not enough room left for another full entry AND
                    // the closing marker -- stop here. `raw` already
                    // has every entry collected regardless (used below
                    // for the always-complete CHARDUMP-LINES summary).
                    truncated = true;
                    break;
                }
                // NOTE: includes the raw entry POINTER too (entry=%p) --
                // this is the address to target in Cheat Engine. Also
                // includes a best-effort decoded letter next to the tile
                // number, e.g. t=41('E'), so the dump is readable without
                // cross-referencing the tile tables by hand. '?' means
                // "not in either known table" -- could be a genuinely
                // unknown glyph (punctuation, big-font BOTTOM half, JP
                // kana/kanji, etc.), not necessarily an error.
                char decoded = DecodeTileBestEffort(tile);
                AppendSafe(line, sizeof(line), pos,
                    "{entry=%p t=%d('%c') f=%d s=%d x=%d y=%d} ",
                    (void*)entry, tile, decoded, flip, subCount, offX, offY);
            }
            if (truncated) {
                AppendSafe(line, sizeof(line), pos, "...(truncated, %d/%d shown)] resource=%p\n",
                    i, count, (void*)param_3);
            } else {
                AppendSafe(line, sizeof(line), pos, "] resource=%p\n", (void*)param_3);
            }

            // ---- Grouped-by-line summary: helps spot multi-line layout
            // (native 2-line strings, per-line X anchor, Y offset
            // between lines) at a glance -- relevant for the intro
            // cutscene text and for the flavor-text 2-line indentation
            // bug. The array is read in REVERSE order (last entry is
            // usually the FIRST character) -- but per known exceptions
            // (see BossNameText.cpp's kFlavorTextOrderExceptions) this
            // isn't a universal guarantee, so treat this summary as a
            // best-effort READING aid, not ground truth for patching.
            char summary[4096] = {};
            int spos = 0;
            {
                // Group consecutive-in-array entries sharing the same Y,
                // in the order they appear in the array (index 0..count-1),
                // then reverse each group's characters for display since
                // the array is normally right-to-left.
                std::vector<std::vector<char>> linesChars;
                std::vector<int32_t> lineY;
                std::vector<int32_t> lineMinX, lineMaxX;
                for (auto& e : raw) {
                    char c = DecodeTileBestEffort(e.tile);
                    if (!linesChars.empty() && lineY.back() == e.y) {
                        linesChars.back().push_back(c);
                        lineMinX.back() = (std::min)(lineMinX.back(), e.x);
                        lineMaxX.back() = (std::max)(lineMaxX.back(), e.x);
                    } else {
                        linesChars.push_back({ c });
                        lineY.push_back(e.y);
                        lineMinX.push_back(e.x);
                        lineMaxX.push_back(e.x);
                    }
                }
                AppendSafe(summary, sizeof(summary), spos,
                    "CHARDUMP-LINES strObj=%p %d line-group(s): ", (void*)strObj, (int)linesChars.size());
                constexpr size_t kSummaryCloseMargin = 150;
                for (size_t li = 0; li < linesChars.size(); li++) {
                    if ((size_t)spos >= sizeof(summary) - kSummaryCloseMargin) {
                        AppendSafe(summary, sizeof(summary), spos, "...(truncated)");
                        break; // buffer nearly full -- stop, what we have is still useful
                    }
                    std::string rev(linesChars[li].begin(), linesChars[li].end());
                    std::reverse(rev.begin(), rev.end()); // array is usually right-to-left
                    AppendSafe(summary, sizeof(summary), spos,
                        "[y=%d xRange=%d..%d \"%s\"] ", lineY[li], lineMinX[li], lineMaxX[li], rev.c_str());
                }
                AppendSafe(summary, sizeof(summary), spos, "\n");
            }

            bool alreadySeen = !s_seenLines.insert(line).second;
            if (!alreadySeen) {
                LogLine("%s", line);
                LogLine("%s", summary);
            }
        }
    }

    Real_DrawChars(param_1, param_2, param_3, param_4, param_5, param_6, param_7);
}

bool InstallGlyphDrawHook(uintptr_t moduleBase)
{
    uintptr_t addr = moduleBase + RVA_DRAW_CHARS;

    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
        reinterpret_cast<LPVOID>(&Detour_DrawChars),
        reinterpret_cast<LPVOID*>(&Real_DrawChars));
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));

    LogLine("InstallGlyphDrawHook(DrawChars) @ RVA 0x%llx (addr %p) -> create=%d enable=%d\n",
        (unsigned long long)RVA_DRAW_CHARS, (void*)addr, (int)s1, (int)s2);

    return s1 == MH_OK && s2 == MH_OK;
}