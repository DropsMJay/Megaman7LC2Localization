// MiscText.cpp
#include "pch.h"
#include "MiscText.h"
#include "EnglishTextEncoding.h"  // EncodeLineToGameBytes() -- shared UTF-8-aware
                                   // encoder, single source of truth for the
                                   // font's Unicode->byte mapping (see
                                   // EnglishCharTable.h), used across every
                                   // text system in this DLL
#include "Logging.h"
#include "resource.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>

extern HMODULE g_selfModule; // defined in dllmain.cpp -- needed to find our
                              // own embedded resources (see LoadMiscLocalization)

using json = nlohmann::json;

// slotId -> translated lines, one map per table.
static std::unordered_map<int, std::vector<MiscLine>> g_tableA;
static std::unordered_map<int, std::vector<MiscLine>> g_tableB;

// Same near-module allocator pattern as EnglishText.cpp / JapaneseText.cpp
// -- see those files for the full rationale (far-away heap pointers
// confuse the shared dialogue/UI state machine once enough of them are
// in play). Kept as a private copy here rather than sharing a header,
// since each system currently owns its allocation independently; worth
// factoring into a shared Utils.h/.cpp later if a fourth system shows up.
static uint8_t* g_nearModuleBlock = nullptr;
static size_t g_nearModuleBlockUsed = 0;
static size_t g_nearModuleBlockSize = 0;

static uint8_t* AllocateNearModule(uintptr_t moduleBase, size_t totalBytesNeeded) {
    const intptr_t offsets[] = {
        0x10000000, -0x10000000, 0x20000000, -0x20000000,
        0x40000000, -0x40000000, 0x60000000, -0x60000000,
    };
    for (intptr_t off : offsets) {
        LPVOID hint = reinterpret_cast<LPVOID>(moduleBase + off);
        LPVOID mem = VirtualAlloc(hint, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mem != nullptr) {
            LogLine("[HistoryScreenText] Allocated block near module base: 0x%llx (offset 0x%llx)\n",
                     (unsigned long long)mem, (unsigned long long)off);
            return reinterpret_cast<uint8_t*>(mem);
        }
    }
    LPVOID mem = VirtualAlloc(nullptr, totalBytesNeeded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LogLine("[HistoryScreenText] Could not allocate near the module base -- using a generic address.\n");
    return reinterpret_cast<uint8_t*>(mem);
}

static uint8_t* BumpAlloc(size_t bytes, size_t alignment = 1) {
    size_t aligned = (g_nearModuleBlockUsed + (alignment - 1)) & ~(alignment - 1);
    if (aligned + bytes > g_nearModuleBlockSize) {
        return nullptr;
    }
    uint8_t* result = g_nearModuleBlock + aligned;
    g_nearModuleBlockUsed = aligned + bytes;
    return result;
}

// ---------------------------------------------------------------------
// Original-chain reading: captures texSel/validCheck/x/y per line so a
// translation that doesn't explicitly override them keeps the original
// look/position. Mirrors the read loop from find_history_text_sources_v2.py.
// ---------------------------------------------------------------------
struct OriginalLineHeader {
    uint8_t texSel;
    uint8_t validCheck;
    uint8_t x;
    uint8_t y;
};

static std::vector<OriginalLineHeader> ReadOriginalHeaders(uintptr_t chainAddr) {
    std::vector<OriginalLineHeader> headers;
    const uint8_t* cursor = reinterpret_cast<const uint8_t*>(chainAddr);
    for (int i = 0; i < 64; ++i) {
        uint8_t count = cursor[0];
        if (count == 0) break;
        OriginalLineHeader h{ cursor[1], cursor[2], cursor[3], cursor[4] };
        headers.push_back(h);
        cursor = cursor + 5 + count;
    }
    return headers;
}

// Encodes one line (header + text bytes) into dest. Returns bytes written.
static size_t EncodeLine(uint8_t* dest, const MiscLine& line, const OriginalLineHeader* fallback) {
    // Uses the project's shared UTF-8-aware encoder (same one EnglishText.cpp
    // uses) instead of a hand-rolled byte-by-byte lookup -- that approach
    // broke on any multi-byte UTF-8 character (e.g. (c) U+00A9, or accented
    // PT-BR letters), since each raw byte got looked up independently
    // instead of the full decoded codepoint.
    std::string gameBytes = EncodeLineToGameBytes(line.text);
    std::vector<uint8_t> encodedChars(gameBytes.begin(), gameBytes.end());

    uint8_t texSel = line.texSelOverride.value_or(fallback ? fallback->texSel : 0x00);
    uint8_t validCheck = line.validCheckOverride.value_or(fallback ? fallback->validCheck : 0x00);
    uint8_t x = line.xOverride.value_or(fallback ? fallback->x : 0x00);
    uint8_t y = line.yOverride.value_or(fallback ? fallback->y : 0x00);

    dest[0] = static_cast<uint8_t>(encodedChars.size());
    dest[1] = texSel;
    dest[2] = validCheck;
    dest[3] = x;
    dest[4] = y;
    std::memcpy(dest + 5, encodedChars.data(), encodedChars.size());
    return 5 + encodedChars.size();
}

// Builds and writes a full replacement chain for one slot, returns the
// near-module address of the new chain (or 0 on failure).
static uintptr_t BuildChain(const std::vector<MiscLine>& lines,
                             const std::vector<OriginalLineHeader>& originalHeaders) {
    // Size pass
    size_t totalBytes = 1; // terminator byte
    for (const auto& line : lines) {
        totalBytes += 5 + line.text.size(); // upper bound (1 byte/char before mapping)
    }

    uint8_t* dest = BumpAlloc(totalBytes, 1);
    if (dest == nullptr) {
        LogLine("[MiscText] BumpAlloc ran out of space for a chain (%zu bytes needed).\n", totalBytes);
        return 0;
    }

    uint8_t* cursor = dest;
    for (size_t i = 0; i < lines.size(); ++i) {
        const OriginalLineHeader* fallback = (i < originalHeaders.size()) ? &originalHeaders[i] : nullptr;
        OriginalLineHeader synthesized{};
        if (fallback == nullptr && !originalHeaders.empty()) {
            // Ran off the end of the original chain (translation added
            // lines) -- synthesize a plausible header from the last known
            // one: same x/texSel/validCheck, y advanced by the same 24px
            // step observed between the history screen's lines.
            synthesized = originalHeaders.back();
            synthesized.y = static_cast<uint8_t>(synthesized.y + 3 * (i - originalHeaders.size() + 1)); // 3*8=24px per extra line
            fallback = &synthesized;
        }
        cursor += EncodeLine(cursor, lines[i], fallback);
    }
    *cursor = 0x00; // terminator

    return reinterpret_cast<uintptr_t>(dest);
}

// Overwrites one 8-byte pointer slot in a .rdata table (needs a
// temporary VirtualProtect since .rdata is normally read-only).
static void WritePointerSlot(uintptr_t slotAddr, uintptr_t newValue) {
    DWORD oldProtect;
    LPVOID addr = reinterpret_cast<LPVOID>(slotAddr);
    if (!VirtualProtect(addr, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
        LogLine("[HistoryScreenText] VirtualProtect failed for slot 0x%llx (err %lu)\n",
                 (unsigned long long)slotAddr, GetLastError());
        return;
    }
    *reinterpret_cast<uintptr_t*>(addr) = newValue;
    DWORD unused;
    VirtualProtect(addr, sizeof(uintptr_t), oldProtect, &unused);
}

static void PatchOneTable(uintptr_t moduleBase, uintptr_t tableRva,
                           const std::unordered_map<int, std::vector<MiscLine>>& translations,
                           const char* tableLabel) {
    if (translations.empty()) return;

    uintptr_t tableBase = moduleBase + tableRva;

    for (const auto& [slotId, lines] : translations) {
        uintptr_t slotAddr = tableBase + slotId * MISC_ENTRY_STRIDE;
        uintptr_t originalChainAddr = *reinterpret_cast<uintptr_t*>(slotAddr);
        if (originalChainAddr == 0) {
            LogLine("[HistoryScreenText] Table %s slot %d: null original pointer, skipping.\n", tableLabel, slotId);
            continue;
        }

        std::vector<OriginalLineHeader> originalHeaders = ReadOriginalHeaders(originalChainAddr);
        LogLine("[HistoryScreenText] Table %s slot %d: original chain @ 0x%llx, %zu original line(s), %zu translated line(s).\n",
                 tableLabel, slotId, (unsigned long long)originalChainAddr, originalHeaders.size(), lines.size());

        uintptr_t newChainAddr = BuildChain(lines, originalHeaders);
        if (newChainAddr == 0) {
            LogLine("[HistoryScreenText] Table %s slot %d: failed to build replacement chain, leaving original.\n", tableLabel, slotId);
            continue;
        }

        WritePointerSlot(slotAddr, newChainAddr);
        LogLine("[HistoryScreenText] Table %s slot %d: patched -> 0x%llx\n", tableLabel, slotId, (unsigned long long)newChainAddr);
    }
}

// ---------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------
// Expected MiscText.json shape:
// {
//   "A": {
//     "15": ["1987  ROCKMAN", "1988  ROCKMAN 2", ..., "...AND"]
//   },
//   "B": {
//     "15": ["CAPCOM CO., LTD.1995", "CAPCOM USA, INC.1995"]
//   }
// }
// Each line can ALSO be an object instead of a plain string, to override
// position/texture for that specific line:
//   {"text": "EXTRA LINE", "y": 26}
static std::unordered_map<int, std::vector<MiscLine>> ParseTable(const json& tableJson) {
    std::unordered_map<int, std::vector<MiscLine>> result;
    if (tableJson.is_null()) return result;

    for (auto it = tableJson.begin(); it != tableJson.end(); ++it) {
        int slotId = std::stoi(it.key());
        std::vector<MiscLine> lines;
        for (const auto& lineJson : it.value()) {
            MiscLine line;
            if (lineJson.is_string()) {
                line.text = lineJson.get<std::string>();
            } else {
                line.text = lineJson.value("text", std::string());
                if (lineJson.contains("texSel"))     line.texSelOverride = static_cast<uint8_t>(lineJson["texSel"].get<int>());
                if (lineJson.contains("validCheck")) line.validCheckOverride = static_cast<uint8_t>(lineJson["validCheck"].get<int>());
                if (lineJson.contains("x"))           line.xOverride = static_cast<uint8_t>(lineJson["x"].get<int>());
                if (lineJson.contains("y"))           line.yOverride = static_cast<uint8_t>(lineJson["y"].get<int>());
            }
            lines.push_back(std::move(line));
        }
        result[slotId] = std::move(lines);
    }
    return result;
}

void LoadMiscLocalization() {
    // Embedded as a Win32 resource (IDR_MISCTEXT, see MM7Loc.rc/resource.h)
    // instead of a loose file on disk -- matches GameTextUS.json,
    // GameTextJP.json and BossNameText.json, and avoids depending on the
    // process's working directory at runtime (which isn't always the
    // game's folder, depending on how Steam/the launcher starts it).
    HRSRC hRes = FindResource(g_selfModule, MAKEINTRESOURCE(IDR_MISCTEXT), RT_RCDATA);
    if (!hRes) {
        LogLine("[MiscText] FindResource failed for MiscText.json\n");
        return;
    }

    HGLOBAL hData = LoadResource(g_selfModule, hRes);
    DWORD size = SizeofResource(g_selfModule, hRes);
    const char* rawData = static_cast<const char*>(LockResource(hData));
    if (!hData || !rawData || size == 0) {
        LogLine("[MiscText] LoadResource/LockResource failed for MiscText.json\n");
        return;
    }

    json root;
    try {
        root = json::parse(rawData, rawData + size);
    } catch (const json::parse_error& e) {
        LogLine("[MiscText] JSON parse error: %s\n", e.what());
        return;
    }

    g_tableA = ParseTable(root.value("A", json()));
    g_tableB = ParseTable(root.value("B", json()));
    LogLine("[MiscText] Loaded %zu slot(s) for table A, %zu slot(s) for table B.\n",
             g_tableA.size(), g_tableB.size());
}

void PatchMiscTable(uintptr_t moduleBase) {
    if (g_tableA.empty() && g_tableB.empty()) return;

    // Size pass: rough upper bound across both tables, all slots.
    size_t totalBytesNeeded = 0;
    for (const auto& [slotId, lines] : g_tableA)
        for (const auto& line : lines) totalBytesNeeded += 5 + line.text.size();
    for (const auto& [slotId, lines] : g_tableB)
        for (const auto& line : lines) totalBytesNeeded += 5 + line.text.size();
    totalBytesNeeded += (g_tableA.size() + g_tableB.size()) * 1; // terminators
    totalBytesNeeded += 64; // slack

    g_nearModuleBlock = AllocateNearModule(moduleBase, totalBytesNeeded);
    if (g_nearModuleBlock == nullptr) {
        LogLine("[HistoryScreenText] VirtualAlloc failed completely -- history/label patch aborted.\n");
        return;
    }
    g_nearModuleBlockSize = totalBytesNeeded;
    g_nearModuleBlockUsed = 0;

    PatchOneTable(moduleBase, RVA_TABLE_A, g_tableA, "A");
    PatchOneTable(moduleBase, RVA_TABLE_B, g_tableB, "B");
}
