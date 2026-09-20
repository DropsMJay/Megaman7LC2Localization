#pragma once
#include <cstdint>

// Installs the hook on FUN_14005dd80 -- the DRAW function called by the
// generic loop in FUN_14005c070, with the signature:
//   FUN_14005dd80(*(descriptor+0x10), uVar3, resolvedResource, lVar8,
//                 descriptor+0x40, *(descriptor+0x90), *(descriptor+0x91));
// The 5th parameter (descriptor+0x40) is the prime suspect for holding
// the actual TEXT / glyph indices (e.g. the letters of "FREEZE MAN").

// NOTE: this diagnostic hooks FUN_14005dd80, the same function that
// BossNameText.cpp hooks. MinHook allows only one hook per address, so
// this file must NOT be enabled together with BossNameText. It is
// intentionally excluded from the build; to use it, add it to the
// project and disable InstallBossNameHook in dllmain.cpp.

bool InstallGlyphDrawHook(uintptr_t moduleBase);