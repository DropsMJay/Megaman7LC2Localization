// EnglishText.h
// Owns the game's dialogue text table: loading the translation from the
// embedded GameTextUS.json resource, and patching the game's in-memory
// string table so it points at our translated lines.
#pragma once
#include <cstdint>

// Layout of one entry in the game's string table (found via Ghidra).
// Do not reorder these fields -- they must match the game's own layout.
struct StringEntry {
    uint64_t ctx;     // shared context pointer -- untouched
    uint64_t TotalCharCount;  // total character count across all lines (still testing its real role)
    char** LinePointers;     // pointer to an array of line pointers
};

constexpr uintptr_t RVA_TABLE_START = 0xE27B50; // where the string table lives, relative to module base
constexpr size_t ENTRY_STRIDE = 0x18;           // sizeof(StringEntry), kept as a sanity-check constant

// Decodes the embedded GameTextUS.json resource into memory. Call this
// once, early -- before the hook is installed.
void LoadLocalization();

// Overwrites the game's live string table with our translated lines.
// Call this from inside the hook, every time the game rebuilds its table.
void PatchStrings(uintptr_t base);