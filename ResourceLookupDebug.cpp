#include "pch.h"
#include "ResourceLookupDebug.h"
#include "Logging.h"
#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <intrin.h>

constexpr uintptr_t RVA_RESOURCE_LOOKUP = 0x60620; // FUN_140060620

typedef int64_t* (*ResourceLookup_t)(uintptr_t mgr, uintptr_t strObj);
static ResourceLookup_t Real_ResourceLookup = nullptr;

uintptr_t g_bossNameResourcePtr = 0;

// The "length" field of this string layout is unreliable (it always
// reads 0, even when the buffer holds real content), so we ignore it
// and simply copy the raw bytes, treating them as a normal C string.
static const char* ReadStdString(uintptr_t strObj, char* scratch, size_t scratchSize)
{
    memcpy(scratch, reinterpret_cast<void*>(strObj), scratchSize - 1);
    scratch[scratchSize - 1] = '\0';
    return scratch;
}

int64_t* Detour_ResourceLookup(uintptr_t mgr, uintptr_t strObj)
{
    int64_t* result = Real_ResourceLookup(mgr, strObj);

    char scratch[64] = {};
    const char* name = ReadStdString(strObj, scratch, sizeof(scratch));

    // Logs every NEW (not previously seen) name so far -- helps identify
    // unknown resources (e.g. the name behind a resource pointer that
    // showed up in GlyphDrawDebug).
    static char s_seenNames[128][64] = {};
    static int s_seenCount = 0;

    if (name[0] != '\0') {
        bool alreadySeen = false;
        for (int i = 0; i < s_seenCount; i++) {
            if (strcmp(s_seenNames[i], name) == 0) { alreadySeen = true; break; }
        }
        if (!alreadySeen && s_seenCount < 128) {
            strcpy_s(s_seenNames[s_seenCount], name);
            s_seenCount++;
            LogLine("=== NEW NAME: \"%s\" -> %p ===\n", name, (void*)result);
        }

        if (strstr(name, "OSSNAME") != nullptr) {
            g_bossNameResourcePtr = (uintptr_t)result;
        }
    }

    if (name[0] != '\0') {
        void* retAddr = _ReturnAddress();
        LogLine("ResourceLookup(\"%s\") -> %p   [called from %p]\n",
            name, (void*)result, retAddr);
    }

    return result;
}

bool InstallResourceLookupHook(uintptr_t moduleBase)
{
    uintptr_t addr = moduleBase + RVA_RESOURCE_LOOKUP;

    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
        reinterpret_cast<LPVOID>(&Detour_ResourceLookup),
        reinterpret_cast<LPVOID*>(&Real_ResourceLookup));
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));

    LogLine("InstallResourceLookupHook @ RVA 0x%llx (addr %p) -> create=%d enable=%d\n",
        (unsigned long long)RVA_RESOURCE_LOOKUP, (void*)addr, (int)s1, (int)s2);

    return s1 == MH_OK && s2 == MH_OK;
}