// EnglishText.cpp
#include "pch.h"
#include "EnglishText.h"
#include "EnglishTextEncoding.h"
#include "Logging.h"
#include "resource.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstring>

using json = nlohmann::json;

extern HMODULE g_selfModule; // defined in dllmain.cpp

// Translated English strings, and the char* pointer arrays (LinePointers)
// themselves, live inside ONE big VirtualAlloc'd block placed as close to
// the game module's base address as the OS will allow, instead of being
// scattered across the normal process heap via std::string/std::vector's
// own allocator.
//
// This mirrors JapaneseText.cpp's AllocateNearModule, which already had
// to solve the same problem: heap allocations that end up far outside
// the game module's own address range make the shared dialogue state
// machine (FUN_1400561a0 and friends) misbehave once enough far-away
// pointers are in play at once. A single patched English entry used to
// coexist fine with untouched original entries, but patching many
// entries together (the normal case) eventually broke a few entries in.
static uint8_t* g_nearModuleBlock = nullptr;
static size_t g_nearModuleBlockUsed = 0;
static size_t g_nearModuleBlockSize = 0;

static uint8_t* AllocateNearModule(uintptr_t moduleBase, size_t totalBytesNeeded) {
    // Try a handful of candidate addresses within +/-1.5GB of the module
    // base (Windows will refuse any that are already occupied -- that's
    // fine, we just try the next candidate). Falling back to nullptr
    // (let the OS pick anywhere) if none of them work, so this degrades
    // gracefully instead of failing outright.
    const intptr_t offsets[] = {
        0x10000000, -0x10000000, 0x20000000, -0x20000000,
        0x40000000, -0x40000000, 0x60000000, -0x60000000,
    };
    for (intptr_t off : offsets) {
        LPVOID hint = reinterpret_cast<LPVOID>(moduleBase + off);
        LPVOID mem = VirtualAlloc(hint, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mem != nullptr) {
            LogLine("[EnglishText] Allocated block near module base: 0x%llx (offset 0x%llx from moduleBase)\n",
                     (unsigned long long)mem, (unsigned long long)off);
            return reinterpret_cast<uint8_t*>(mem);
        }
    }
    // Fallback: let the OS place it anywhere.
    LPVOID mem = VirtualAlloc(nullptr, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LogLine("[EnglishText] Could not allocate near the module base -- using a generic address.\n");
    return reinterpret_cast<uint8_t*>(mem);
}

// Simple bump allocator over g_nearModuleBlock. Returns nullptr if the
// block is exhausted (shouldn't happen -- we size it exactly up front).
static uint8_t* BumpAlloc(size_t bytes, size_t alignment = 8) {
    size_t aligned = (g_nearModuleBlockUsed + (alignment - 1)) & ~(alignment - 1);
    if (aligned + bytes > g_nearModuleBlockSize) {
        return nullptr;
    }
    uint8_t* result = g_nearModuleBlock + aligned;
    g_nearModuleBlockUsed = aligned + bytes;
    return result;
}

// tableIndex -> where its data ended up inside g_nearModuleBlock.
struct EnglishEntryLocation {
    char** linePointers;   // pointer to the array of char*, itself inside the block
    size_t lineCount;
    size_t totalCharCount;
};
static std::unordered_map<size_t, EnglishEntryLocation> g_indexToLocation;

// GameTextUS.json stores each entry's visible text as ONE string in which
// every line (= one char* in the game's LinePointers array) is separated by
// the literal tag "[NewLine]". Split it back into individual lines.
static std::vector<std::string> SplitOnNewLineTag(const std::string& text)
{
    static const std::string kSep = "[NewLine]";
    std::vector<std::string> lines;
    if (text.empty()) return lines; // no text -> leave the original entry untouched
    size_t start = 0;
    while (true) {
        size_t pos = text.find(kSep, start);
        if (pos == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + kSep.size();
    }
    return lines;
}

void LoadLocalization()
{
    HRSRC hRes = FindResource(g_selfModule, MAKEINTRESOURCE(IDR_GAMETEXT_US), RT_RCDATA);
    if (!hRes) {
        LogLine("FindResource failed for GameTextUS.json\n");
        return;
    }

    HGLOBAL hData = LoadResource(g_selfModule, hRes);
    DWORD size = SizeofResource(g_selfModule, hRes);
    const char* rawData = static_cast<const char*>(LockResource(hData));

    if (!hData || !rawData || size == 0) {
        LogLine("LoadResource/LockResource failed for GameTextUS.json\n");
        return;
    }

    json data;
    try {
        data = json::parse(rawData, rawData + size);
    }
    catch (const json::parse_error& e) {
        LogLine("JSON parse error (embedded resource): %s\n", e.what());
        return;
    }

    LogLine("GameTextUS.json loaded, %zu entries found.\n", data.size());

    // PASS 1: encode every line for every entry into a temporary staging
    // structure (plain heap -- fine, this never gets read by the game;
    // only the copies we make in PASS 2 do). We need every line's final
    // encoded byte length before we can size the near-module block.
    struct StagedEntry {
        size_t tableIndex;
        std::vector<std::string> lines; // already game-byte-encoded
    };
    std::vector<StagedEntry> staged;
    staged.reserve(data.size());

    size_t totalBytesNeeded = 0;
    for (auto& [key, value] : data.items()) {
        size_t tableIndex = std::stoul(key); // JSON keys are always strings

        StagedEntry entry;
        entry.tableIndex = tableIndex;

        // Current format: { "array_rva": "0x...", "text": "line1[NewLine]line2..." }
        // (array_rva is informational only). Legacy formats -- a bare string
        // or an array of line strings -- are still accepted.
        std::vector<std::string> rawLines;
        if (value.is_object()) {
            rawLines = SplitOnNewLineTag(value.value("text", std::string()));
        } else if (value.is_string()) {
            rawLines = SplitOnNewLineTag(value.get<std::string>());
        } else if (value.is_array()) {
            for (auto& lineValue : value) rawLines.push_back(lineValue.get<std::string>());
        }
        for (auto& raw : rawLines) {
            entry.lines.push_back(EncodeLineToGameBytes(raw));
        }

        if (!entry.lines.empty()) {
            // Space for the LinePointers array itself (one char* per line,
            // plus one trailing nullptr terminator -- see PASS 2).
            totalBytesNeeded += (entry.lines.size() + 1) * sizeof(char*);
            // Space for each line's bytes + null terminator.
            for (auto& line : entry.lines) {
                totalBytesNeeded += line.size() + 1;
            }
            // Small cushion per entry for alignment padding between
            // sub-allocations (8 bytes is the max waste per BumpAlloc call).
            totalBytesNeeded += (entry.lines.size() + 1) * 8;
        }

        staged.push_back(std::move(entry));
    }

    if (totalBytesNeeded == 0) {
        LogLine("[EnglishText] Nothing to allocate (0 entries with lines) -- skipping near-module block.\n");
        return;
    }

    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)); // game module
    g_nearModuleBlockSize = totalBytesNeeded;
    g_nearModuleBlock = AllocateNearModule(moduleBase, totalBytesNeeded);
    if (!g_nearModuleBlock) {
        LogLine("[EnglishText] VirtualAlloc failed completely -- English patch aborted.\n");
        return;
    }
    g_nearModuleBlockUsed = 0;

    // PASS 2: copy every staged entry's data into the near-module block,
    // recording where each entry's final LinePointers array and line
    // count/char-count ended up.
    for (auto& entry : staged) {
        if (entry.lines.empty()) {
            continue; // handled as "0 lines" in PatchStrings, same as before
        }

        size_t totalCharCount = 0;
        std::vector<char*> finalLinePointers;
        finalLinePointers.reserve(entry.lines.size());

        for (auto& line : entry.lines) {
            size_t bytesNeeded = line.size() + 1; // + null terminator
            uint8_t* dest = BumpAlloc(bytesNeeded, 1);
            if (!dest) {
                LogLine("[EnglishText] BumpAlloc ran out of space -- entry %zu is incomplete!\n",
                         entry.tableIndex);
                break;
            }
            memcpy(dest, line.data(), line.size());
            dest[line.size()] = '\0';
            finalLinePointers.push_back(reinterpret_cast<char*>(dest));
            totalCharCount += line.size();
        }

        // The LinePointers array itself also needs to live inside the
        // block (it's read directly by the game as StringEntry.LinePointers).
        // The game reads this array until it hits an invalid pointer (see
        // summary 2.4 / 8.4), so it MUST end with an explicit nullptr --
        // otherwise it can run into whatever bytes follow in the block.
        // lineCount below deliberately excludes the terminator.
        const size_t realLineCount = finalLinePointers.size();
        finalLinePointers.push_back(nullptr);
        size_t arrayBytes = finalLinePointers.size() * sizeof(char*);
        uint8_t* arrayDest = BumpAlloc(arrayBytes, alignof(char*));
        if (!arrayDest) {
            LogLine("[EnglishText] BumpAlloc ran out of space for entry %zu's pointer array!\n",
                     entry.tableIndex);
            continue;
        }
        memcpy(arrayDest, finalLinePointers.data(), arrayBytes);

        EnglishEntryLocation loc;
        loc.linePointers = reinterpret_cast<char**>(arrayDest);
        loc.lineCount = realLineCount;
        // CONFIRMED (live instrumentation): TotalCharCount isn't only
        // "how many characters this text has" -- the game ALSO uses it
        // as the read limit for how many words of "ctx" (the control
        // header that runs before the real text) it's allowed to
        // consume before it's allowed to start drawing. Entries with
        // short text (small TotalCharCount) didn't leave enough room
        // for the whole header, so the game stalled forever before
        // ever reaching the marker that redirects to the real text
        // (observed live: stalls at ctx word index 15 when
        // TotalCharCount was 14). We add a safety margin here -- large
        // enough to cover any plausible control header, at no real
        // cost (this value is never shown on screen).
        constexpr size_t CTX_HEADER_SAFETY_MARGIN = 32;
        loc.totalCharCount = totalCharCount + CTX_HEADER_SAFETY_MARGIN;
        g_indexToLocation[entry.tableIndex] = loc;
    }

    LogLine("[EnglishText] Near-module block: %zu of %zu bytes used.\n",
             g_nearModuleBlockUsed, g_nearModuleBlockSize);
}

void PatchStrings(uintptr_t base)
{
    auto* table = reinterpret_cast<StringEntry*>(base + RVA_TABLE_START);

    for (auto& [tableIndex, loc] : g_indexToLocation)
    {
        table[tableIndex].LinePointers = loc.linePointers;
        table[tableIndex].TotalCharCount = loc.totalCharCount;
    }

    LogLine("Hook fired! %zu entries patched (near-module block).\n", g_indexToLocation.size());
}
