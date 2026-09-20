#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

// TextEntryDebug.h
// -----------------------------------------------------------------------
// DIAGNOSTIC hook for FUN_1400561a0 -- the function that calls the
// character-reader (FUN_140056360) in a loop and decides when a dialogue
// box has finished displaying and can advance.
//
// BACKGROUND:
//   This hook was written to track down the Japanese dialogue softlock
//   (scene keeps running, text box frozen, no crash, low CPU). That bug
//   is RESOLVED: a header word that must be 0x0200 (the typing-speed
//   parameter) was being truncated to 0, permanently stalling the
//   text-reveal timer (see MM7Loc_INVESTIGATION_SUMMARY.md, section
//   2.1). The hook is kept as a diagnostic tool for similar
//   "box frozen" symptoms.
//
//   How the search was narrowed down:
//
//     void FUN_1400561a0(longlong param_1) {
//         if (*(char*)(param_1 + 0x58) == '\0') {
//             if (*(longlong*)(param_1 + 0x18) == 0) {
//                 *(longlong*)(param_1 + 0x18) = /* resolves EN/JP table
//                                                    entry, same field the
//                                                    char reader hook
//                                                    filters on */;
//             }
//             do {
//                 if (*(code**)(param_1 + 8) == nullptr) break;
//                 cVar2 = (**(code**)(param_1 + 8))(param_1);   // +0x5E
//             } while (cVar2 != '\0');
//
//             // completion/advance block -- only reached once the loop
//             // above exits with cVar2 == 0
//             ...
//             if (*(char*)(param_1 + 0x58) != '\0') {
//                 DAT_1408fd952 = 0;
//                 DAT_1408fd878 = 0;
//             }
//         }
//     }
//
//   [param_1+8] is a FUNCTION POINTER selecting the dialogue box's
//   current "phase" (typing / waiting-for-input / closing / ...).
//   FUN_140056360 is only ONE of these phases. In the softlocked JP
//   game, [param_1+8] never advanced past the phase driven by the
//   reveal-pacing timer (FUN_140056520 / FUN_140056720), so
//   [param_1+0x58] never got set, the outer guard never let this
//   function reach the completion block, and whatever downstream code
//   was waiting on that never saw "dialogue done" -- matching the
//   observed symptom.
//
// KEY INSIGHT USED HERE: param_1 in FUN_1400561a0 is the SAME struct
// pointer passed as RCX into FUN_140056360 (it's literally what the
// indirect call above passes). That means we can reuse the exact same
// filter address JapaneseText.cpp's CharReader hook already uses
// (SetCharReaderWatchedBuffer's moduleBase+destRva) to narrow this hook
// down to just our watched dialogue entry, by checking [param_1+0x18]
// the same way Detour_CharReader checks field18. No new addressing
// scheme needed.
// -----------------------------------------------------------------------

// DIAGNOSTIC: narrows logging to calls whose [param_1+0x18] matches this
// exact value -- pass the SAME address you pass to
// SetCharReaderWatchedBuffer (moduleBase + destRva of the staging slot),
// so both hooks are watching the same dialogue entry. Called from
// PatchJapaneseStrings, right next to the existing
// SetCharReaderWatchedBuffer call.
void SetStateWatchedBuffer(uintptr_t watchedAddr);

// DIAGNOSTIC: installs the MinHook detour on FUN_1400561a0. Call once,
// from dllmain.cpp's MainThread, alongside InstallCharReaderHook.
bool InstallStateMachineHook(uintptr_t moduleBase);

// DIAGNOSTIC: narrows the FUN_140056520 hook the same way -- pass the
// SAME address used for SetCharReaderWatchedBuffer / SetStateWatchedBuffer.
void SetPhase56520WatchedBuffer(uintptr_t watchedAddr);

// DIAGNOSTIC: installs the MinHook detour on FUN_140056520 (the
// "waiting on jump-table condition" phase FUN_1400561a0 gets stuck in).
bool InstallPhase56520Hook(uintptr_t moduleBase);

// DIAGNOSTIC: narrows the FUN_140056720 hook the same way -- pass the
// SAME address used for the other Set*WatchedBuffer calls.
void SetPhase56720WatchedBuffer(uintptr_t watchedAddr);

// DIAGNOSTIC: installs the MinHook detour on FUN_140056720 (the
// per-character reveal-pacing timer FUN_140056520 calls into).
bool InstallPhase56720Hook(uintptr_t moduleBase);

// DIAGNOSTIC: watches a specific English StringEntry (by table index,
// i.e. the same index used as a key in GameTextUS.json) for changes to
// [param_1+0x28] -- the "current line" char* FUN_140056360 reads from
// when processing English text. Call this from PatchStrings or
// alongside it, AFTER moduleBase is known. Reuses the already-installed
// FUN_1400561a0 hook -- no separate MH_CreateHook needed.
// Adds an address to watch (doesn't replace previous ones -- can be
// called multiple times to watch several indices at once). "label"
// shows up in the log to identify which one matched.
void AddEnglishWatchedAddress(uintptr_t address, const std::string& label);

// Shortcut: watches a table index using the CONFIRMED address formula
// (tableBase + tableIndex * ENTRY_STRIDE, no extra offset -- see
// TextEntryDebug.cpp for how this was verified against live data).
void AddEnglishWatchedIndex(uintptr_t moduleBase, size_t tableIndex);

// Clears every watched address (call this before a new test run if you
// don't want it to accumulate with the previous one).
void ClearEnglishWatchedAddresses();

// DIAGNOSTIC: narrows the word-dispatch hook to one dialogue entry -- pass
// the SAME address used for the other Set*WatchedBuffer calls.
void SetWordDispatchWatchedBuffer(uintptr_t watchedAddr);

// DIAGNOSTIC: installs the MinHook detour on LAB_140056260, the main
// control-code dispatcher shared by both languages.
bool InstallWordDispatchHook(uintptr_t moduleBase);

// DIAGNOSTIC: narrows the [NewLine] handler hook the same way.
void SetNewLineWatchedBuffer(uintptr_t watchedAddr);

// DIAGNOSTIC: installs the MinHook detour on FUN_140056970 (the idx=8
// handler behind [NewLine] / [AdvanceCursorLine]); logs the cursor fields
// [+0x30] / [+0x32] before and after each call.
bool InstallNewLineHook(uintptr_t moduleBase);
