// BossNameText.h
// Owns the "big font" boss name banner and "small font" flavor-text line
// on the stage select screen (both rendered via FUN_14005dd80, NOT via
// the StringEntry dialogue table). See docs/BOSSNAME_Font_Investigation.md
// for how this format was reverse engineered.
#pragma once
#include <cstdint>

// Decodes the embedded BossNameText.json resource into memory. Call this
// once, early -- before the hook is installed. Mirrors LoadLocalization()
// in EnglishText.cpp.
void LoadBossNameLocalization();

// Installs the hook on FUN_14005dd80. Every time the game is about to
// draw a "big font" or "small font" string, we check whether it matches
// one of the 16 known original strings (8 boss names + 8 flavor-text
// lines); if so, we substitute the (possibly edited) replacement text
// loaded from BossNameText.json.
bool InstallBossNameHook(uintptr_t moduleBase);
