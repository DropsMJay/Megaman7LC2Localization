// EnglishTextDumper.h
// Dumps the game's live string table to mm7loc_dump.txt for inspection --
// useful for confirming the hook is reading/patching the right memory.
#pragma once
#include <cstdint>

void DumpAllStrings(uintptr_t base);