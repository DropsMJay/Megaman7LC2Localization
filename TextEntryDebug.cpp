// TextEntryDebug.cpp
// -----------------------------------------------------------------------
// See TextEntryDebug.h for the overall design/rationale.
// -----------------------------------------------------------------------
#include "pch.h"
#include "TextEntryDebug.h"
#include "Logging.h"
#include <windows.h>
#include <MinHook.h>
#include <cstdio>

// --- FUN_1400561a0 (dialogue-advance state machine) diagnostic hook ---
//
// Like FUN_140056360, this is called constantly for every on-screen text
// box (HUD, menus, all dialogue -- not just Japanese), with a per-text
// context struct as its only argument (RCX / param_1). Fields of that
// struct confirmed over the course of this investigation:
//   [param_1+0x08] -- function pointer selecting the current "phase"
//   [param_1+0x18] -- resolved text-buffer-table pointer (same field
//                     FUN_140056360 reads as field18 -- StringEntry* for
//                     English, a Japanese word-array slot otherwise)
//   [param_1+0x20] -- read index into ctx / LinePointers
//   [param_1+0x26] -- table index to display, copied from the global
//                     event/script variable DAT_1408fd953 at creation
//   [param_1+0x28] -- current line pointer (English char* reads)
//   [param_1+0x30]/[param_1+0x32] -- packed cursor position / line-start anchor
//   [param_1+0x34] -- per-entry typing pace (fixed-point)
//   [param_1+0x3c] -- written by control idx=1, reader still unknown
//   [param_1+0x3e] -- [ForceWait] flag (blocks skipping a pause)
//   [param_1+0x3f]/[param_1+0x40] -- persistent typing pace set by [SetTypeSpeed]
//   [param_1+0x58] -- completion flag; the outer guard of FUN_1400561a0
//                     only lets the function run while this is 0, and
//                     (per the decompile) only code reachable from
//                     INSIDE the do-while loop can plausibly set it

constexpr uintptr_t RVA_STATE_MACHINE = 0x561a0; // FUN_1400561a0

typedef void(*StateMachine_t)(uint64_t);
static StateMachine_t Real_StateMachine = nullptr;

static uintptr_t g_stateWatchedBuffer = 0;
static uintptr_t g_stateModuleBase = 0;
static uint64_t g_stateCallCount = 0;
static uint64_t g_stateLoggedCount = 0;

// --- English LinePointers watch ---
//
// FUN_1400561a0 is shared by BOTH languages -- it's the same state
// machine, just resolving [param_1+0x18] to a Japanese word-array slot
// OR an English StringEntry* depending on the language flag (see
// PTR_DAT_14045fca8's 2-entry table). For English, FUN_140056360 reads
// the CURRENT LINE'S content through [param_1+0x28] (a char* -- see its
// decompile: `pcVar3 = *(char**)(param_1+0x28)`), not from the word
// array. Something advances [+0x28] to StringEntry.LinePointers[next]
// when a line's null terminator is hit; watching a specific entry's
// address lets us see exactly when that stops happening.
//
// CONFIRMED (live instrumentation against FUN_1400561a0's own decompile):
// [param_1+0x18] resolves as `tableBase + tableIndex * ENTRY_STRIDE`,
// with NO extra offset for the language selector beyond picking which
// of the two table pointers to use -- 828/828 real addresses observed
// across a full session matched this formula exactly (a "+8" variant
// had zero matches).
static std::unordered_map<uintptr_t, std::string> g_englishWatchedAddresses;
static uint64_t g_englishLoggedCount = 0;
static std::unordered_map<uintptr_t, bool> g_englishSeenBefore;

void ClearEnglishWatchedAddresses() {
    g_englishWatchedAddresses.clear();
    DebugOut("[TextEntryDebug][EN] Watched address list cleared.\n");
}

void AddEnglishWatchedAddress(uintptr_t address, const std::string& label) {
    g_englishWatchedAddresses[address] = label;
    char buf[200];
    snprintf(buf, sizeof(buf),
        "[TextEntryDebug][EN] Watching \"%s\" at 0x%llx\n",
        label.c_str(), (unsigned long long)address);
    DebugOut(buf);
}

void AddEnglishWatchedIndex(uintptr_t moduleBase, size_t tableIndex) {
    constexpr uintptr_t RVA_TABLE_START = 0xE27B50; // EnglishText.h
    constexpr uintptr_t ENTRY_STRIDE = 0x18;

    uintptr_t address = moduleBase + RVA_TABLE_START + tableIndex * ENTRY_STRIDE;

    char label[64];
    snprintf(label, sizeof(label), "entry %zu", tableIndex);
    AddEnglishWatchedAddress(address, label);
}

// Best-effort symbol names for known phase-handler function pointers, so
// the log is directly readable against Ghidra without a lookup step.
// Extend this as new phase handlers get identified.
static const char* SymbolNameForRva(uintptr_t rva) {
    switch (rva) {
        case 0x56360: return "FUN_140056360 (character reader)";
        default:      return nullptr;
    }
}

void SetStateWatchedBuffer(uintptr_t watchedAddr) {
    g_stateWatchedBuffer = watchedAddr;
    char buf[128];
    snprintf(buf, sizeof(buf), "[TextEntryDebug] Watching calls with field18=0x%llx\n",
             (unsigned long long)watchedAddr);
    DebugOut(buf);
}

static void Detour_StateMachine(uint64_t param_1) {
    g_stateCallCount++;

    // DIAGNOSTIC: unconditional heartbeat, same purpose as
    // [CharReader][HEARTBEAT] -- confirms the hook fires at all,
    // independent of whether the filter below ever matches.
    if (g_stateCallCount == 1 || (g_stateCallCount % 500) == 0) {
        char hb[128];
        snprintf(hb, sizeof(hb), "[TextEntryDebug][HEARTBEAT] total calls so far: %llu\n",
                 (unsigned long long)g_stateCallCount);
        DebugOut(hb);
    }

    // Guard every read with SEH -- this function is shared by multiple
    // text subsystems, so param_1 might not always point at a struct
    // shaped like the one we expect.
    uint64_t field18 = 0, field8Before = 0;
    uint8_t field58Before = 0;
    uint16_t field34Before = 0;
    uint8_t field3fBefore = 0, field40Before = 0;
    uint32_t stateField20Before = 0;
    bool readOk = true;
    __try {
        field18 = *reinterpret_cast<uint64_t*>(param_1 + 0x18);
        field8Before = *reinterpret_cast<uint64_t*>(param_1 + 0x08);
        field58Before = *reinterpret_cast<uint8_t*>(param_1 + 0x58);
        field34Before = *reinterpret_cast<uint16_t*>(param_1 + 0x34);
        // CONFIRMED: [+0x3f] is written by the idx=2 handler
        // (UndefinedFunction_1400565d0) and consumed by FUN_140056360 to
        // decide whether to enter the PER-CHARACTER wait phase
        // (LAB_140056430, which copies it to [+0x40] and decrements it
        // per glyph). This is a PERSISTENT typing pace -- unlike the
        // one-off pause from [07] -- confirmed via [SetTypeSpeed].
        field3fBefore = *reinterpret_cast<uint8_t*>(param_1 + 0x3f);
        field40Before = *reinterpret_cast<uint8_t*>(param_1 + 0x40);
        // [+0x20] is the read index into ctx (or LinePointers). Kept
        // here because it's what exposed the TotalCharCount bug: for
        // short English entries, this index would get stuck partway
        // through ctx's control header, since TotalCharCount (used as
        // the read bound) was too small to cover the whole header --
        // see EnglishText.cpp's CTX_HEADER_SAFETY_MARGIN.
        stateField20Before = *reinterpret_cast<uint32_t*>(param_1 + 0x20);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }

    bool isWatched = readOk && g_stateWatchedBuffer != 0 && field18 == g_stateWatchedBuffer;

    // DIAGNOSTIC: English side -- checks against ANY address in the
    // watch list, not just one. Uses const char* (not std::string) on
    // purpose: __try/__except in this function doesn't accept C++
    // objects with a destructor in the same scope (error C2712).
    const char* englishMatchLabel = "";
    bool isEnglishWatched = false;
    if (readOk) {
        auto it = g_englishWatchedAddresses.find(field18);
        if (it != g_englishWatchedAddresses.end()) {
            isEnglishWatched = true;
            englishMatchLabel = it->second.c_str();
        }
    }
    uint64_t field28Before = 0, field20Before = 0;
    if (isEnglishWatched) {
        g_stateLoggedCount++;
        uintptr_t rvaBefore = (g_stateModuleBase != 0 && field8Before >= g_stateModuleBase)
                                   ? (field8Before - g_stateModuleBase) : 0;
        const char* symBefore = SymbolNameForRva(rvaBefore);
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "[TextEntryDebug] #%llu BEFORE param_1=0x%llx [+8]=0x%llx (RVA 0x%llx%s%s) [+0x58]=%u [+0x34]=0x%04x\n",
                 (unsigned long long)g_stateLoggedCount, (unsigned long long)param_1,
                 (unsigned long long)field8Before, (unsigned long long)rvaBefore,
                 symBefore ? " = " : "", symBefore ? symBefore : "",
                 (unsigned)field58Before, (unsigned)field34Before);
        DebugOut(buf);
    }

    // DIAGNOSTIC (broad, no address assumption): logs every time
    // [+0x18] changes value since the last call -- without filtering by
    // any specific address. Useful for seeing the real picture of which
    // addresses show up during a session, instead of trusting we know
    // in advance which address an entry "should" resolve to.
    static uint64_t s_lastSeenField18 = 0;
    if (readOk && field18 != s_lastSeenField18) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "[TextEntryDebug][ALLADDR] [+0x18] changed to 0x%llx\n",
            (unsigned long long)field18);
        DebugOut(buf);
        s_lastSeenField18 = field18;
    }

    // DIAGNOSTIC: DAT_1408fd953 is the global variable the game's
    // event/script engine uses to say "show dialogue entry N" -- it
    // gets copied into [+0x26] when the context is created
    // (FUN_140057a10). Logging it every time it changes lets us see the
    // exact value right when a given entry gets chosen, without having
    // to guess which of its ~67 write sites in the code is responsible.
    static uint8_t s_lastSeenEventIndex = 0xFF;
    if (g_stateModuleBase != 0) {
        __try {
            uint8_t eventIndex = *reinterpret_cast<uint8_t*>(g_stateModuleBase + 0x8fd953);
            if (eventIndex != s_lastSeenEventIndex) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                    "[TextEntryDebug][EVENTIDX] DAT_1408fd953 changed to %u (0x%02x)\n",
                    (unsigned)eventIndex, (unsigned)eventIndex);
                DebugOut(buf);
                s_lastSeenEventIndex = eventIndex;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    Real_StateMachine(param_1);

    // DIAGNOSTIC: English -- log only when [+0x28] (the current line
    // pointer) actually changes, so we don't spam a line every frame
    // while the game sits mid-line waiting for the reveal timer.
    if (isEnglishWatched) {
        uint64_t field28After = 0, field20After = 0;
        bool readOkAfter = true;
        __try {
            field28After = *reinterpret_cast<uint64_t*>(param_1 + 0x28);
            field20After = *reinterpret_cast<uint64_t*>(param_1 + 0x20);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            readOkAfter = false;
        }
        bool isFirstSight = !g_englishSeenBefore[field18];
        g_englishSeenBefore[field18] = true;

        if (readOkAfter && (field28After != field28Before || isFirstSight)) {
            g_englishLoggedCount++;
            char preview[64] = {0};
            if (field28After != 0) {
                __try {
                    const char* s = reinterpret_cast<const char*>(field28After);
                    size_t i = 0;
                    for (; i < sizeof(preview) - 1 && s[i] != '\0'; i++) {
                        preview[i] = s[i];
                    }
                    preview[i] = '\0';
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    snprintf(preview, sizeof(preview), "<read failed>");
                }
            } else {
                snprintf(preview, sizeof(preview), "<null>");
            }
            char buf[420];
            snprintf(buf, sizeof(buf),
                "[TextEntryDebug][EN] #%llu [%s]%s [+0x28] changed from 0x%llx to 0x%llx  "
                "[+0x20]: %llu -> %llu  text=\"%s\"\n",
                (unsigned long long)g_englishLoggedCount, englishMatchLabel,
                isFirstSight ? " FIRST-SIGHT" : "",
                (unsigned long long)field28Before, (unsigned long long)field28After,
                (unsigned long long)field20Before, (unsigned long long)field20After,
                preview);
            DebugOut(buf);
        }
    }

    if (isWatched) {
        uint64_t field8After = 0;
        uint8_t field58After = 0;
        uint16_t field34After = 0;
        uint8_t field3fAfter = 0, field40After = 0;
        uint32_t stateField20After = 0;
        bool readOkAfter = true;
        __try {
            field8After = *reinterpret_cast<uint64_t*>(param_1 + 0x08);
            field58After = *reinterpret_cast<uint8_t*>(param_1 + 0x58);
            field34After = *reinterpret_cast<uint16_t*>(param_1 + 0x34);
            field3fAfter = *reinterpret_cast<uint8_t*>(param_1 + 0x3f);
            field40After = *reinterpret_cast<uint8_t*>(param_1 + 0x40);
            stateField20After = *reinterpret_cast<uint32_t*>(param_1 + 0x20);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            readOkAfter = false;
        }

        if (readOkAfter) {
            uintptr_t rvaAfter = (g_stateModuleBase != 0 && field8After >= g_stateModuleBase)
                                      ? (field8After - g_stateModuleBase) : 0;
            const char* symAfter = SymbolNameForRva(rvaAfter);
            char buf[384];
            snprintf(buf, sizeof(buf),
                     "[TextEntryDebug] #%llu AFTER param_1=0x%llx [+8]=0x%llx (RVA 0x%llx%s%s) [+0x58]=%u [+0x34]=0x%04x\n",
                     (unsigned long long)g_stateLoggedCount, (unsigned long long)param_1,
                     (unsigned long long)field8After, (unsigned long long)rvaAfter,
                     symAfter ? " = " : "", symAfter ? symAfter : "",
                     (unsigned)field58After, (unsigned)field34After);
            DebugOut(buf);

            // Logs [+0x3f] transitions -- confirmed to be the persistent
            // typing-pace field written by [SetTypeSpeed].
            if (field3fAfter != field3fBefore) {
                char t[256];
                snprintf(t, sizeof(t),
                    "[TextEntryDebug] #%llu TRANSITION: [+0x3f] changed from %d to %d\n",
                    (unsigned long long)g_stateLoggedCount,
                    (int)field3fBefore, (int)field3fAfter);
                DebugOut(t);
            }
            if (stateField20After != stateField20Before) {
                char t[256];
                snprintf(t, sizeof(t),
                    "[TextEntryDebug] #%llu TRANSITION: [+0x20] changed from %u to %u\n",
                    (unsigned long long)g_stateLoggedCount,
                    stateField20Before, stateField20After);
                DebugOut(t);
            }
            if (field40After != field40Before) {
                char t[256];
                snprintf(t, sizeof(t),
                    "[TextEntryDebug] #%llu TRANSITION: [+0x40] changed from %d to %d\n",
                    (unsigned long long)g_stateLoggedCount,
                    (int)field40Before, (int)field40After);
                DebugOut(t);
            }

            // [+8] (the current phase handler) and [+0x58] (the
            // completion flag) are the two fields that reveal whether a
            // dialogue box's state machine is stuck or has finished
            // normally.
            if (field8After != field8Before) {
                char t[384];
                snprintf(t, sizeof(t),
                         "[TextEntryDebug] #%llu TRANSITION: [+8] changed from 0x%llx to 0x%llx\n",
                         (unsigned long long)g_stateLoggedCount,
                         (unsigned long long)field8Before, (unsigned long long)field8After);
                DebugOut(t);
            }
            if (field58After != field58Before) {
                char t[256];
                snprintf(t, sizeof(t),
                         "[TextEntryDebug] #%llu TRANSITION: [+0x58] changed from %u to %u\n",
                         (unsigned long long)g_stateLoggedCount,
                         (unsigned)field58Before, (unsigned)field58After);
                DebugOut(t);
            }
        }
    }
}

bool InstallStateMachineHook(uintptr_t moduleBase) {
    if (!IsDebugModeOn) return true;
    g_stateModuleBase = moduleBase;
    uintptr_t addr = moduleBase + RVA_STATE_MACHINE;
    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
                                  reinterpret_cast<LPVOID>(&Detour_StateMachine),
                                  reinterpret_cast<LPVOID*>(&Real_StateMachine));
    if (s1 != MH_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[TextEntryDebug] MH_CreateHook failed: status %d\n", (int)s1);
        DebugOut(buf);
        return false;
    }
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    char buf[128];
    snprintf(buf, sizeof(buf), "[TextEntryDebug] Hook installed (rva=0x%llx) -- MH_EnableHook status %d\n",
             (unsigned long long)RVA_STATE_MACHINE, (int)s2);
    DebugOut(buf);
    return s2 == MH_OK;
}

// --- FUN_140056970 (idx=8 handler -- confirmed to fire in pairs,
// matching the "08 08 = newline" observation) diagnostic hook ---
//
// Decompile/disassembly:
//   DX = [+0x32]
//   DX = DX ^ [+0x30]
//   DX = DX & 0x1F
//   DX = DX ^ [+0x30]
//   DX = DX + 0x20
//   [+0x30] = DX
//   return 1
//
// Hypothesis: [+0x30]/[+0x32] are a packed cursor position (row+column
// or X+Y), and this swaps in the low 5 bits from one into the other,
// explaining why it always fires twice in a row (once per half).
// Logging both fields before/after every call to see the real values.

constexpr uintptr_t RVA_NEWLINE_HANDLER = 0x56970;

typedef uint64_t(*NewLineHandler_t)(uint64_t);
static NewLineHandler_t Real_NewLineHandler = nullptr;

static uintptr_t g_newLineWatchedBuffer = 0;
static uint64_t g_newLineLoggedCount = 0;

void SetNewLineWatchedBuffer(uintptr_t watchedAddr) {
    g_newLineWatchedBuffer = watchedAddr;
    char buf[160];
    snprintf(buf, sizeof(buf), "[NewLineHandler] Watching calls with field18=0x%llx\n",
        (unsigned long long)watchedAddr);
    DebugOut(buf);
}

static uint64_t Detour_NewLineHandler(uint64_t param_1) {
    uint64_t field18 = 0;
    uint16_t field30Before = 0, field32Before = 0;
    bool isWatched = false;
    __try {
        field18 = *reinterpret_cast<uint64_t*>(param_1 + 0x18);
        isWatched = (g_newLineWatchedBuffer != 0 && field18 == g_newLineWatchedBuffer);
        if (isWatched) {
            field30Before = *reinterpret_cast<uint16_t*>(param_1 + 0x30);
            field32Before = *reinterpret_cast<uint16_t*>(param_1 + 0x32);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        isWatched = false;
    }

    uint64_t result = Real_NewLineHandler(param_1);

    if (isWatched) {
        __try {
            uint16_t field30After = *reinterpret_cast<uint16_t*>(param_1 + 0x30);
            uint16_t field32After = *reinterpret_cast<uint16_t*>(param_1 + 0x32);
            g_newLineLoggedCount++;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "[NewLineHandler] #%llu BEFORE [+0x30]=0x%04x [+0x32]=0x%04x  "
                "AFTER [+0x30]=0x%04x [+0x32]=0x%04x\n",
                (unsigned long long)g_newLineLoggedCount,
                (unsigned)field30Before, (unsigned)field32Before,
                (unsigned)field30After, (unsigned)field32After);
            DebugOut(buf);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    return result;
}

bool InstallNewLineHook(uintptr_t moduleBase) {
    if (!IsDebugModeOn) return true;
    uintptr_t addr = moduleBase + RVA_NEWLINE_HANDLER;
    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
        reinterpret_cast<LPVOID>(&Detour_NewLineHandler),
        reinterpret_cast<LPVOID*>(&Real_NewLineHandler));
    if (s1 != MH_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[NewLineHandler] MH_CreateHook failed: status %d\n", (int)s1);
        DebugOut(buf);
        return false;
    }
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    char buf[128];
    snprintf(buf, sizeof(buf), "[NewLineHandler] Hook installed (rva=0x%llx) -- status %d\n",
        (unsigned long long)RVA_NEWLINE_HANDLER, (int)s2);
    DebugOut(buf);
    return s2 == MH_OK;
}

// --- FUN_140056520 (jump-table condition wait) diagnostic hook ---
//
// Decompile:
//   cVar1 = (*(code*)(&PTR_LAB_1404531a0)[*(ushort*)(param_1+0x24)])();
//   if (cVar1 != 0) {
//       *(uint32*)(param_1+0x54) = 0;
//       *(void**)(param_1+8) = &LAB_140056260;
//   }
//
// This is the phase our logs show [param_1+8] gets stuck on. We log the
// SELECTED condition (index + resolved jump-table target RVA) BEFORE
// calling the real function, and infer whether the condition fired
// AFTER, by checking if [+8] moved away from this function's own
// address -- without calling the condition function ourselves a second
// time (which could consume an input event and mask the real behavior).

constexpr uintptr_t RVA_PHASE_56520 = 0x56520;      // FUN_140056520
constexpr uintptr_t RVA_JUMP_TABLE_1404531A0 = 0x4531A0; // PTR_LAB_1404531a0

typedef void(*Phase56520_t)(uint64_t);
static Phase56520_t Real_Phase56520 = nullptr;

static uintptr_t g_phase56520WatchedBuffer = 0;
static uintptr_t g_phase56520ModuleBase = 0;
static uint64_t g_phase56520CallCount = 0;
static uint64_t g_phase56520LoggedCount = 0;

void SetPhase56520WatchedBuffer(uintptr_t watchedAddr) {
    g_phase56520WatchedBuffer = watchedAddr;
    char buf[128];
    snprintf(buf, sizeof(buf), "[Phase56520] Watching calls with field18=0x%llx\n",
             (unsigned long long)watchedAddr);
    DebugOut(buf);
}

static void Detour_Phase56520(uint64_t param_1) {
    g_phase56520CallCount++;

    if (g_phase56520CallCount == 1 || (g_phase56520CallCount % 500) == 0) {
        char hb[128];
        snprintf(hb, sizeof(hb), "[Phase56520][HEARTBEAT] total calls so far: %llu\n",
                 (unsigned long long)g_phase56520CallCount);
        DebugOut(hb);
    }

    uint64_t field18 = 0;
    uint16_t idx = 0;
    uint32_t field54Before = 0;
    uint64_t jumpTarget = 0;
    bool readOk = true;
    __try {
        field18 = *reinterpret_cast<uint64_t*>(param_1 + 0x18);
        idx = *reinterpret_cast<uint16_t*>(param_1 + 0x24);
        field54Before = *reinterpret_cast<uint32_t*>(param_1 + 0x54);
        uintptr_t tableAddr = g_phase56520ModuleBase + RVA_JUMP_TABLE_1404531A0;
        jumpTarget = *reinterpret_cast<uint64_t*>(tableAddr + (uint64_t)idx * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }

    bool isWatched = readOk && g_phase56520WatchedBuffer != 0 && field18 == g_phase56520WatchedBuffer;

    if (isWatched) {
        g_phase56520LoggedCount++;
        uintptr_t targetRva = (jumpTarget >= g_phase56520ModuleBase)
                                   ? (jumpTarget - g_phase56520ModuleBase) : 0;
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "[Phase56520] #%llu BEFORE param_1=0x%llx idx(+0x24)=%u target=0x%llx (RVA 0x%llx) [+0x54]=%u\n",
                 (unsigned long long)g_phase56520LoggedCount, (unsigned long long)param_1,
                 (unsigned)idx, (unsigned long long)jumpTarget,
                 (unsigned long long)targetRva, (unsigned)field54Before);
        DebugOut(buf);
    }

    Real_Phase56520(param_1);

    if (isWatched) {
        uint64_t field8After = 0;
        uint32_t field54After = 0;
        bool readOkAfter = true;
        __try {
            field8After = *reinterpret_cast<uint64_t*>(param_1 + 0x08);
            field54After = *reinterpret_cast<uint32_t*>(param_1 + 0x54);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            readOkAfter = false;
        }
        if (readOkAfter) {
            uintptr_t selfAddr = g_phase56520ModuleBase + RVA_PHASE_56520;
            bool transitioned = (field8After != selfAddr);
            char buf[320];
            snprintf(buf, sizeof(buf),
                     "[Phase56520] #%llu AFTER [+0x54]=%u condition_fired=%s\n",
                     (unsigned long long)g_phase56520LoggedCount, (unsigned)field54After,
                     transitioned ? "YES" : "no");
            DebugOut(buf);
        }
    }
}

bool InstallPhase56520Hook(uintptr_t moduleBase) {
    if (!IsDebugModeOn) return true;
    g_phase56520ModuleBase = moduleBase;
    uintptr_t addr = moduleBase + RVA_PHASE_56520;
    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
                                  reinterpret_cast<LPVOID>(&Detour_Phase56520),
                                  reinterpret_cast<LPVOID*>(&Real_Phase56520));
    if (s1 != MH_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[Phase56520] MH_CreateHook failed: status %d\n", (int)s1);
        DebugOut(buf);
        return false;
    }
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    char buf[128];
    snprintf(buf, sizeof(buf), "[Phase56520] Hook installed (rva=0x%llx) -- MH_EnableHook status %d\n",
             (unsigned long long)RVA_PHASE_56520, (int)s2);
    DebugOut(buf);
    return s2 == MH_OK;
}

// --- FUN_140056720 (per-character reveal-pacing timer) diagnostic hook ---
//
// Decompile:
//   if (*(int*)(param_1+0x54) == 0) {
//       ... resolve/cache the character code into [+0x3a] ...
//       *(int*)(param_1+0x54) = 1;
//   }
//   uVar6 = *(ushort*)(param_1+0x38);
//   *(short*)(param_1+0x36) += *(ushort*)(param_1+0x34);   // <-- delay, READ only
//   uVar9 = ((short)*(ushort*)(param_1+0x34) >> 8) + uVar6;
//   *(ushort*)(param_1+0x38) = uVar9;
//   return uVar9 ^ uVar6;   // caller only checks the LOW BYTE (truthy = "elapsed")
//
// CONFIRMED via With18/Without18 log comparison: [+0x34] is 0x0200 (512)
// constant across EVERY character in the original, and 0x0000 constant
// across EVERY character in the patched game -- for the ENTIRE visible
// text (idx 16 through 48). Since it never varies per-character, it's a
// per-ENTRY pacing value, not a per-character one, and this function
// never writes it -- only reads it. This hook exists to correlate
// [+0x34]'s value with idx/char/tick/result at the moment it's consumed;
// TextEntryDebug.cpp's [+0x34] logging (added to Detour_StateMachine) is
// what actually pinpoints WHEN it gets set.

constexpr uintptr_t RVA_PHASE_56720 = 0x56720; // FUN_140056720

typedef uint64_t(*Phase56720_t)(uint64_t);
static Phase56720_t Real_Phase56720 = nullptr;

static uintptr_t g_phase56720WatchedBuffer = 0;
static uint64_t g_phase56720CallCount = 0;
static uint64_t g_phase56720LoggedCount = 0;

void SetPhase56720WatchedBuffer(uintptr_t watchedAddr) {
    g_phase56720WatchedBuffer = watchedAddr;
    char buf[128];
    snprintf(buf, sizeof(buf), "[Phase56720] Watching calls with field18=0x%llx\n",
             (unsigned long long)watchedAddr);
    DebugOut(buf);
}

static uint64_t Detour_Phase56720(uint64_t param_1) {
    g_phase56720CallCount++;

    if (g_phase56720CallCount == 1 || (g_phase56720CallCount % 500) == 0) {
        char hb[128];
        snprintf(hb, sizeof(hb), "[Phase56720][HEARTBEAT] total calls so far: %llu\n",
                 (unsigned long long)g_phase56720CallCount);
        DebugOut(hb);
    }

    uint64_t field18 = 0;
    uint32_t idxBefore = 0, cacheFlagBefore = 0;
    uint16_t sVar5Before = 0, delayBefore = 0, accBefore = 0, tickBefore = 0;
    bool readOk = true;
    __try {
        field18      = *reinterpret_cast<uint64_t*>(param_1 + 0x18);
        idxBefore    = *reinterpret_cast<uint32_t*>(param_1 + 0x20);
        cacheFlagBefore = *reinterpret_cast<uint32_t*>(param_1 + 0x54);
        sVar5Before  = *reinterpret_cast<uint16_t*>(param_1 + 0x3a);
        delayBefore  = *reinterpret_cast<uint16_t*>(param_1 + 0x34);
        accBefore    = *reinterpret_cast<uint16_t*>(param_1 + 0x36);
        tickBefore   = *reinterpret_cast<uint16_t*>(param_1 + 0x38);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }

    bool isWatched = readOk && g_phase56720WatchedBuffer != 0 && field18 == g_phase56720WatchedBuffer;

    if (isWatched) {
        g_phase56720LoggedCount++;
        char buf[400];
        snprintf(buf, sizeof(buf),
                 "[Phase56720] #%llu BEFORE idx=%u cacheFlag(+0x54)=%u char(+0x3a)=0x%04x "
                 "delay(+0x34)=0x%04x acc(+0x36)=0x%04x tick(+0x38)=0x%04x\n",
                 (unsigned long long)g_phase56720LoggedCount, idxBefore, cacheFlagBefore,
                 (unsigned)sVar5Before, (unsigned)delayBefore, (unsigned)accBefore, (unsigned)tickBefore);
        DebugOut(buf);
    }

    uint64_t result = Real_Phase56720(param_1);

    if (isWatched) {
        uint16_t sVar5After = 0, delayAfter = 0, accAfter = 0, tickAfter = 0;
        bool readOkAfter = true;
        __try {
            sVar5After = *reinterpret_cast<uint16_t*>(param_1 + 0x3a);
            delayAfter = *reinterpret_cast<uint16_t*>(param_1 + 0x34);
            accAfter   = *reinterpret_cast<uint16_t*>(param_1 + 0x36);
            tickAfter  = *reinterpret_cast<uint16_t*>(param_1 + 0x38);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            readOkAfter = false;
        }
        if (readOkAfter) {
            char buf[400];
            snprintf(buf, sizeof(buf),
                     "[Phase56720] #%llu AFTER char(+0x3a)=0x%04x delay(+0x34)=0x%04x "
                     "acc(+0x36)=0x%04x tick(+0x38)=0x%04x result=0x%llx\n",
                     (unsigned long long)g_phase56720LoggedCount, (unsigned)sVar5After,
                     (unsigned)delayAfter, (unsigned)accAfter, (unsigned)tickAfter,
                     (unsigned long long)result);
            DebugOut(buf);
        }
    }

    return result;
}

bool InstallPhase56720Hook(uintptr_t moduleBase) {
    if (!IsDebugModeOn) return true;
    uintptr_t addr = moduleBase + RVA_PHASE_56720;
    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
                                  reinterpret_cast<LPVOID>(&Detour_Phase56720),
                                  reinterpret_cast<LPVOID*>(&Real_Phase56720));
    if (s1 != MH_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[Phase56720] MH_CreateHook failed: status %d\n", (int)s1);
        DebugOut(buf);
        return false;
    }
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    char buf[128];
    snprintf(buf, sizeof(buf), "[Phase56720] Hook installed (rva=0x%llx) -- MH_EnableHook status %d\n",
             (unsigned long long)RVA_PHASE_56720, (int)s2);
    DebugOut(buf);
    return s2 == MH_OK;
}

// --- LAB_140056260 (word dispatch: glyph vs control-code vs aux-table
// jump) diagnostic hook ---
//
// Decompile confirms: when uVar4==0x100, the NEXT word (uVar5) indexes
// into an auxiliary pointer table (plVar2[2]), and the resolved pointer
// gets written into [param_1+0x28] -- the SAME field FUN_140056360 reads
// as a plain char* (the English "current line" mechanism). So [0001]
// switches this entry from "read kana words" to "read a byte string",
// almost certainly for embedding non-kana content (item names, "UFO",
// numbers) via the same path English text uses.

constexpr uintptr_t RVA_WORD_DISPATCH = 0x56260;

typedef void(*WordDispatch_t)(uint64_t);
static WordDispatch_t Real_WordDispatch = nullptr;

static uintptr_t g_dispatchWatchedBuffer = 0;
static uint64_t g_dispatchLoggedCount = 0;

void SetWordDispatchWatchedBuffer(uintptr_t watchedAddr) {
    g_dispatchWatchedBuffer = watchedAddr;
    char buf[160];
    snprintf(buf, sizeof(buf), "[WordDispatch] Watching calls with field18=0x%llx\n",
        (unsigned long long)watchedAddr);
    DebugOut(buf);
}

static void Detour_WordDispatch(uint64_t param_1) {
    uint64_t field18 = 0;
    uint32_t idxBefore = 0;
    uint16_t uVar4 = 0;
    bool isWatched = false;
    __try {
        field18 = *reinterpret_cast<uint64_t*>(param_1 + 0x18);
        isWatched = (g_dispatchWatchedBuffer != 0 && field18 == g_dispatchWatchedBuffer);
        if (isWatched) {
            idxBefore = *reinterpret_cast<uint32_t*>(param_1 + 0x20);
            uint64_t bufferPtr = *reinterpret_cast<uint64_t*>(field18);
            uVar4 = *reinterpret_cast<uint16_t*>(bufferPtr + (uint64_t)idxBefore * 2);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        isWatched = false;
    }

    if (isWatched) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "[WordDispatch] idx=%u uVar4=0x%04x%s\n",
            idxBefore, uVar4, (uVar4 == 0x100) ? " (AUX TABLE JUMP)" : "");
        DebugOut(buf);
    }

    Real_WordDispatch(param_1);

    if (isWatched && uVar4 == 0x100) {
        __try {
            uint64_t field28 = *reinterpret_cast<uint64_t*>(param_1 + 0x28);
            char preview[64] = { 0 };
            if (field28 != 0) {
                const char* s = reinterpret_cast<const char*>(field28);
                size_t i = 0;
                for (; i < sizeof(preview) - 1 && s[i] != '\0'; i++) {
                    preview[i] = s[i];
                }
                preview[i] = '\0';
            }
            g_dispatchLoggedCount++;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "[WordDispatch] #%llu AFTER: [+0x28] resolved = 0x%llx  preview=\"%s\"\n",
                (unsigned long long)g_dispatchLoggedCount,
                (unsigned long long)field28, preview);
            DebugOut(buf);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

bool InstallWordDispatchHook(uintptr_t moduleBase) {
    if (!IsDebugModeOn) return true;
    uintptr_t addr = moduleBase + RVA_WORD_DISPATCH;
    MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(addr),
        reinterpret_cast<LPVOID>(&Detour_WordDispatch),
        reinterpret_cast<LPVOID*>(&Real_WordDispatch));
    if (s1 != MH_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[WordDispatch] MH_CreateHook failed: status %d\n", (int)s1);
        DebugOut(buf);
        return false;
    }
    MH_STATUS s2 = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    char buf[128];
    snprintf(buf, sizeof(buf), "[WordDispatch] Hook installed (rva=0x%llx) -- status %d\n",
        (unsigned long long)RVA_WORD_DISPATCH, (int)s2);
    DebugOut(buf);
    return s2 == MH_OK;
}
