// dllmain.cpp
// Entry point for the DLL. Installs a hook on the game's string-init
// function so our translated text (see EnglishText.cpp) gets applied every
// time the game rebuilds its dialogue table.
#include "pch.h"
#include <windows.h>
#include <MinHook.h>
#include "EnglishText.h"
#include "EnglishTextDumper.h"
#include "Logging.h"
#include "JapaneseText.h"
#include "TextEntryDebug.h"
#include "ResourceLookupDebug.h"
//#include "GlyphDrawDebug.h"
#include "BossNameText.h"
#include "MiscText.h"


static uintptr_t g_base = 0;      // handle of the GAME's module (used to find its data)
constexpr uintptr_t RVA_JP_MASTER_INIT = 0x17b0;  // UndefinedFunction_1400017b0

typedef void(*JpMasterInit_t)();
JpMasterInit_t Real_JpMasterInit = nullptr;

void Detour_JpMasterInit() {
    Real_JpMasterInit();
    PatchJapaneseStrings(g_base);   // <- fixed: g_base, not g_moduleBase
}

// Fixed distance from the function we hook to the start of the .exe module.
// This never changes, even though the actual load address does (ASLR).
constexpr uintptr_t RVA_INIT_STRINGS = 0x2BC0;

HMODULE g_selfModule = nullptr;   // handle of THIS dll (used to find our embedded resource)

typedef void (*InitStrings_t)();
static InitStrings_t Original_InitStrings = nullptr;

// This is our "detour" -- runs INSTEAD of the original function, until we
// explicitly call the original ourselves.
void Detour_InitStrings()
{
    Original_InitStrings();
    DumpAllStrings(g_base);
    PatchStrings(g_base);
    PatchMiscTable(g_base);
}

DWORD WINAPI MainThread(LPVOID)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)); // where Windows loaded the game this run (changes every run, due to ASLR)
    uintptr_t target = base + RVA_INIT_STRINGS;                            // real base address + fixed offset = the function's true address right now
    g_base = base;

    LogLineReset("Module base: 0x%llx\n", (unsigned long long)base);
    LogLine("Target address (base + RVA): 0x%llx\n", (unsigned long long)target);

    LoadLocalization(); // decode the embedded JSON before installing the hook
    LoadMiscLocalization();
    LoadBossNameLocalization();

    MH_STATUS s1 = MH_Initialize();
    LogLine("MH_Initialize -> status %d\n", s1);

    MH_STATUS s2 = MH_CreateHook(reinterpret_cast<LPVOID>(target),
        reinterpret_cast<LPVOID>(&Detour_InitStrings),
        reinterpret_cast<LPVOID*>(&Original_InitStrings));
    LogLine("MH_CreateHook -> status %d\n", s2);

    MH_STATUS s3 = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    LogLine("MH_EnableHook -> status %d\n", s3);
	uintptr_t jpInitAddr = g_base + RVA_JP_MASTER_INIT;
	MH_STATUS s5 = MH_CreateHook(reinterpret_cast<LPVOID>(jpInitAddr), &Detour_JpMasterInit, reinterpret_cast<LPVOID*>(&Real_JpMasterInit));
	LogLine("MH_CreateHook (JP) -> status %d\n", s5);
	MH_STATUS s6 = MH_EnableHook(reinterpret_cast<LPVOID>(jpInitAddr));
	LogLine("MH_EnableHook (JP) -> status %d\n", s6);
    InstallCharReaderHook(g_base);
    InstallStateMachineHook(g_base);
    InstallPhase56520Hook(g_base);
    InstallPhase56720Hook(g_base);
    InstallWordDispatchHook(g_base);
    InstallNewLineHook(g_base);
	InstallResourceLookupHook(g_base);
    InstallBossNameHook(g_base);
	//InstallGlyphDrawHook(g_base);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = hModule;
        DisableThreadLibraryCalls(hModule);
        MainThread(nullptr);
    }
    return TRUE;
}