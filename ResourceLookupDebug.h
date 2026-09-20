#pragma once
#include <cstdint>

extern uintptr_t g_bossNameResourcePtr;
// Installs the hook on FUN_140060620. Call once from dllmain.cpp,
// alongside the other Install*Hook functions.
bool InstallResourceLookupHook(uintptr_t moduleBase);