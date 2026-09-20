// MiscText.h
// -----------------------------------------------------------------------
// Patches the "system label" text system found behind FUN_1401003c0 /
// FUN_140100610 -- used for single-purpose UI text (the Sound Test
// screen, Versus Mode / Player Select, the "THANK YOU FOR PLAYING!"
// credits screen) AND the opening history/copyright screen
// ("1987  ROCKMAN" ... "...AND" / "CAPCOM CO., LTD.1995"), as opposed to
// the main 171-entry dialogue table (EnglishText.h) or the Japanese
// word-array dialogue system (JapaneseText.h).
//
// BACKGROUND (how the system works, as found by reverse engineering):
// full RE trail):
//   - Two sibling arrays of 8-byte pointers, TABLE_A and TABLE_B, live in
//     .rdata. FUN_1401003c0/FUN_140100610 pick between them at runtime
//     via `*DAT_140942e50 + 0x400 != 0`. NAMING IS PROVISIONAL -- we
//     know the two tables differ (confirmed: slot 15 holds the full
//     1987-1993 history screen in one table, and just the two copyright
//     lines in the other), but which table corresponds to which actual
//     release/region build hasn't been confirmed live yet. Don't assume
//     "A" = English or "B" = Japanese from the names alone.
//   - Each table slot is indexed by a small integer ("system label ID"),
//     e.g. 15 = history/copyright screen, 14 = Sound Test, 18 = Versus
//     Mode/Player Select, 19 = "THANK YOU FOR PLAYING!".
//   - Each slot points to a CHAIN of concatenated entries, each shaped:
//         [count][texSel][validCheck][x][y] + <count> ASCII bytes
//     repeated until a 0x00 count byte terminates the chain.
//       - count: number of characters in this line
//       - texSel: picks rm07_VRAM_COL0 (fill) vs rm07_VRAM_COL1 (outline)
//         for THIS line specifically -- (texSel & 0x1c) == 0 -> COL0,
//         otherwise COL1
//       - validCheck: entry is only drawn if (texSel & 0x1c) < 5 AND
//         validCheck < 9 -- exact purpose of the two thresholds still
//         unconfirmed, but preserving the original values is safe
//       - x, y: pixel position, each stored /8 (multiply by 8 to draw)
//
// PATCH STRATEGY
//   Unlike the main dialogue table, nothing "rebuilds" these pointers at
//   runtime -- they're static .rdata values read directly by the
//   renderer every frame. So there's no init function to hook: we read
//   the ORIGINAL chain once per translated slot (to capture
//   texSel/validCheck/x/y per line so we don't have to guess them), then
//   build a replacement chain with our translated text in a near-module
//   buffer (same rationale as EnglishText.cpp/JapaneseText.cpp -- see
//   those files' comments), and overwrite the table slot's pointer.
//   Call PatchMiscTable once at startup, after LoadMiscLocalization.
// -----------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

// One line within a system-label entry, as read from MiscText.json.
// x/y/texSel/validCheck are PRESERVED from the corresponding original
// line unless explicitly overridden in the JSON (needed if your
// translation adds more lines than the original had, or needs different
// positioning).
struct MiscLine {
    std::string text;                       // UTF-8; encoded via GetCharToByteTable()
    std::optional<uint8_t> texSelOverride;
    std::optional<uint8_t> validCheckOverride;
    std::optional<uint8_t> xOverride;       // pre-multiply value (real pixel x / 8)
    std::optional<uint8_t> yOverride;       // pre-multiply value (real pixel y / 8)
};

// moduleBase-relative addresses of the two sibling pointer tables.
// CONFIRM THESE if you re-run the RE steps on a different game build --
// they're specific to the MMLC2.exe build this was reverse-engineered
// against.
constexpr uintptr_t RVA_TABLE_A = 0x56d110;
constexpr uintptr_t RVA_TABLE_B = 0x56d020;
// NOTE: named MISC_ENTRY_STRIDE (not ENTRY_STRIDE) -- EnglishText.h already
// declares a global `ENTRY_STRIDE` for the dialogue StringEntry table
// (0x18 bytes), which is a different value for a different table. Reusing
// the plain name caused a C2086/C2374 redefinition error once both
// headers ended up in the same translation unit.
constexpr size_t MISC_ENTRY_STRIDE = 8;

// Decodes MiscText.json into memory. Call once, early -- before
// PatchMiscTable. Loose file (not embedded resource) for now, same
// reasoning as JapaneseStrings.json: allows edit-and-retest without
// recompiling while this system is still being fleshed out.
void LoadMiscLocalization();

// Reads each translated slot's ORIGINAL chain (to capture header bytes),
// builds replacement chains in a near-module buffer, and overwrites the
// pointer(s) in TABLE_A and/or TABLE_B. Call once at startup.
void PatchMiscTable(uintptr_t moduleBase);
