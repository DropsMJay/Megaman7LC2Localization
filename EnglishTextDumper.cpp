// EnglishTextDumper.cpp
#include "pch.h"
#include "EnglishTextDumper.h"
#include "Logging.h"
#include "EnglishText.h" // for StringEntry, RVA_TABLE_START
#include "EnglishArrayBoundaries.h"
#include <cstdio>

static bool IsKnownArrayStart(uintptr_t base, char* ptr)
{
    uintptr_t rva = reinterpret_cast<uintptr_t>(ptr) - base;
    for (size_t i = 0; i < KNOWN_ARRAY_START_COUNT; i++) {
        if (KNOWN_ARRAY_START_RVAS[i] == rva) {
            return true;
        }
    }
    return false;
}

void DumpAllStrings(uintptr_t base)
{
    if (!IsDebugModeOn) return;

    auto* table = reinterpret_cast<StringEntry*>(base + RVA_TABLE_START);
    // BUG FIX: this used to be hardcoded to 163 -- an unverified guess,
    // not the confirmed real size of the table. Missing dialogue (a
    // whole Mega Man / Dr. Light conversation) turned up during
    // playtesting that never appeared anywhere in the dump, strongly
    // suggesting the real table extends past index 162 and the old
    // fixed loop bound was silently cutting it off. Scan much further
    // now, and stop automatically once we hit a solid run of entries
    // that look like we've walked off the end of the real table into
    // unrelated/unmapped memory, logging exactly where that happened
    // so the real count is visible instead of assumed.
    constexpr size_t ENTRY_COUNT_MAX = 500;
    constexpr size_t MAX_LINES_PER_ENTRY = 20;
    constexpr size_t BLANK_RUN_STOP_THRESHOLD = 10; // consecutive fully-zero
                                                     // entries (BOTH fields
                                                     // zero, unlike our known
                                                     // legitimate null-lines
                                                     // entries which still
                                                     // have a nonzero
                                                     // TotalCharCount) before
                                                     // we conclude we're past
                                                     // the real table

    FILE* f = nullptr;
    fopen_s(&f, "mm7loc_dump.txt", "w");
    if (!f) return;

    size_t consecutiveBlank = 0;
    size_t lastRealEntry = 0;

    for (size_t i = 0; i < ENTRY_COUNT_MAX; i++) {
        bool fullyBlank = (table[i].TotalCharCount == 0 && table[i].LinePointers == nullptr);
        if (fullyBlank) {
            consecutiveBlank++;
        } else {
            consecutiveBlank = 0;
            lastRealEntry = i;
        }

        fprintf(f, "[Entry %zu] length=0x%llx lines_ptr=%p\n",
            i, (unsigned long long)table[i].TotalCharCount, (void*)table[i].LinePointers);
        fflush(f);

        if (table[i].LinePointers == nullptr) {
            fprintf(f, "  (skipped -- lines is null)\n\n");
            fflush(f);
            if (consecutiveBlank >= BLANK_RUN_STOP_THRESHOLD) {
                fprintf(f, "STOPPED: %zu consecutive blank entries (both fields "
                        "zero) -- likely walked past the end of the real table. "
                        "Last entry with real data: %zu\n",
                        consecutiveBlank, lastRealEntry);
                fflush(f);
                break;
            }
            continue;
        }

        for (size_t lineIndex = 0; lineIndex < MAX_LINES_PER_ENTRY; lineIndex++) {
            // The ADDRESS of this slot -- checked against known array starts,
            // since an adjacent entry's array begins exactly here when
            // there's no padding between the two arrays in memory.
            char** slotAddr = &table[i].LinePointers[lineIndex];

            if (lineIndex > 0 && IsKnownArrayStart(base, reinterpret_cast<char*>(slotAddr))) {
                fprintf(f, "  Line %zu: (stopped -- this slot is the START of another entry's array)\n", lineIndex);
                fflush(f);
                break;
            }

            char* line = *slotAddr;
            uintptr_t linePtr = reinterpret_cast<uintptr_t>(line);

            if (line == nullptr || linePtr < base || linePtr > base + 0x10000000) {
                fprintf(f, "  Line %zu: (stopped -- pointer looks invalid: %p)\n", lineIndex, (void*)line);
                fflush(f);
                break;
            }

            fprintf(f, "  Line %zu: %s\n", lineIndex, line);
            fflush(f);
        }
        fprintf(f, "\n");
        fflush(f);
    }

    fprintf(f, "END OF DUMP. Last index with real data: %zu\n", lastRealEntry);
    fclose(f);
}