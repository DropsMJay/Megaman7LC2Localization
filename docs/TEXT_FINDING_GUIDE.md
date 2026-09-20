# MM7Loc — Guide: How to Find In-Game Text (English and Japanese)

A practical methodology guide -- doesn't repeat what's already decoded
(that lives in `MM7Loc_INVESTIGATION_SUMMARY.md` and
`JAPANESE_CONTROL_CODES.md`), focuses on **how to search** when you don't know
where a specific piece of text is, or when starting to investigate a
new system.

---

## 1. The two languages: same table layout, different data

Both languages live in one **indexed table of 171 entries, stride `0x18`**,
and the dialogue box picks an entry with the same formula for both
(`base[language] + index * 0x18`, see section 4.0 of the investigation
summary). What differs is the shape of the data in each slot:

| | English | Japanese |
|---|---|---|
| Table base | `RVA_TABLE_START = 0xE27B50` (`moduleBase + 0xE27B50 + N*0x18`) | `0xE26B20` (`moduleBase + 0xE26B20 + N*0x18`) — every `dest_rva` in `GameTextJP.json` equals exactly this |
| Index | Position in the table (0-170) | The **same** index: entry N is the same line of dialogue in both languages |
| Slot contents | `{ctx, TotalCharCount, LinePointers}`: an array of `char*`, one per line | `{word_array_ptr, len, aux_table_ptr}`: a word array (control codes + glyphs) plus an auxiliary pointer table |
| Encoding | Plain `char*`, one byte per character (the game's font codepage, ASCII-like for letters) | 16-bit words holding a custom tile index — not Shift-JIS, not UTF-8, not any standard encoding |
| Extraction tool | `decode_en_text.py` (offline, reads the `.exe`) or `EnglishTextDumper.cpp` (`DumpAllStrings`, runs inside the game) | `build_japanese_strings_json.py` (offline, reads the `.exe`) |

**Do not confuse the two numberings by accident**: an earlier version of
these notes claimed "English index 25 and Japanese index 25 are unrelated".
That was wrong — it came from comparing against an old address list whose
order did not match the real table. Read in the table's real order, the two
languages line up 1:1 (see 8.3 of the investigation summary).

---

## 2. Why you can't just "search for the text"

### 2.1 — English: you can, with one caveat

English uses plain `char*` -- an ASCII string search in Ghidra
(`Search → For Strings`) finds the literal text. The caveat: the
`StringEntry` array only holds **pointers** to that text, so finding the
string doesn't automatically give you its index/position in the table --
you still need to cross-reference the pointer array.

### 2.2 — Japanese: **you can't**, and it's worth understanding why

Classic Japanese text (dialogue, cutscenes) uses a **completely
proprietary** encoding scheme: every kana/kanji is a small arbitrary
number (a tile index into the custom font sheet -- e.g. `0xC6` = "・",
`0x91` = "い"), with no relation to Unicode, Shift-JIS, or any standard.
A Ghidra string search (`Search → For Strings` or `For Encoded
Strings`) looks for byte patterns matching **known** encodings -- since
this game's real bytes don't correspond to any of those, the search
**finds nothing**, even when you know exactly which phrase you're
looking for.

**The only way this system was ever decoded was byte-by-byte reverse
engineering** -- decompiling the functions that read/draw each
character, cross-referencing against real screenshots until confirming
which number maps to which glyph (see `JAPANESE_CONTROL_CODES.md` for
the full table).

### 2.3 — The exception: embedded ASCII via the auxiliary table

There's a special mechanism (marker `0x0100`, see the "aux table jump"
sibling of `[TabToColumn]` in the investigation summary) that lets
Japanese text **embed loose Latin text** mid-sentence -- proper names,
acronyms (`"UFO"`), credits (`"AND CAPCOM"`, `"YOU GET"`), and -- as
discovered after a long investigation -- even pure punctuation runs
like `"............"`.

**This embedded text IS plain ASCII, one byte per character** -- unlike
the rest of the Japanese system. This means it **can** be found via a
normal Ghidra string search, or even a raw byte search in the file,
**as long as you search for the right pattern**: raw, consecutive ASCII
bytes, **without** the 16-bit padding the rest of the system uses.
(A mistake made during the investigation: we searched for the "12 dots" as
`2E 00 2E 00...` -- the main system's pattern -- when the real data was
just `2E 2E 2E 2E...`, no interleaved zero bytes. That delayed finding
entry 25 considerably.)

---

## 3. Step by step: extracting everything from scratch

### 3.1 — English

**Offline (preferred):**

1. Run `decode_en_text.py MMLC2.exe known_array_starts_ordered.h --json
   GameTextUS.extracted.json` (see `tools/README.md`). It reads the `.exe`
   directly, no game or Ghidra needed.
2. Check the result for **array bleed**: English line arrays have no
   terminator, so an entry can run on into whatever array follows it in
   memory (usually credits text). `GameTextUS.json` in the repository was
   cleaned by hand for that reason — see section 8.4 of the investigation
   summary.

**From inside the game (fallback):**

1. Rebuild the mod with `IsDebugModeOn = true`.
2. Run the game — `EnglishTextDumper.cpp` dumps `mm7loc_dump.txt`
   automatically on startup.

### 3.2 — Japanese

1. **Find the `{dest_rva, source_rva}` pairs**: run the Ghidra script
   `find_jp_text_sources.py` (walks the PCode/SSA of
   `UndefinedFunction_1400017b0`, the function that builds the whole
   table at runtime — `RVA_JP_MASTER_INIT = 0x17b0` in `dllmain.cpp`).
   Produces `ghidra_pairs_clean.txt`.
2. **Decode each pair**: `build_japanese_strings_json.py MMLC2.exe
   ghidra_pairs_clean.txt --out GameTextJP.extracted.json` — doesn't need
   Ghidra running for this step, reads the `.exe` directly.
3. **Validate**: `build_japanese_strings_json.py --validate
   GameTextJP.json` — checks for the `[DrawClosingMark]`-without-
   `[NewLine]` bug (see section 2.6 of the investigation summary).

---

## 4. When you know WHAT you're looking for but not WHERE it is

This was the hardest and most common scenario in this investigation
(e.g. "I saw a screen with 12 dots, which entry is that?"). Order of
attempts, cheapest to most expensive:

### 4.1 — Search the already-extracted JSON (free, always try first)

```python
import json, re
with open('GameTextJP.json', encoding='utf-8') as f:
    data = json.load(f)
for key, entry in data.items():
    visible = re.sub(r'\[[^\]]*\]', '', entry['text'])  # strip control tags
    if 'WHAT_YOU_ARE_LOOKING_FOR' in visible:
        print(key, '->', visible[:80])
```

**Important limitation discovered during the investigation**: this only finds text
that lives in the `"text"` field. If the content is **trapped inside
`header_bytes`** (because the boundary heuristic doesn't recognize pure
punctuation as "real text" -- see section 6 of the investigation
summary, entry 25's case), this search **won't find it**, even though
the content genuinely exists in the game.

### 4.2 — Search `header_decoded` too

Same code as above, but running against `entry['header_decoded']` as
well -- covers the case above, but only works if that specific entry's
auxiliary table is already registered in `AUX_TABLE_BY_ENTRY_INDEX` in
the script (that dictionary was curated by hand and does not cover every
entry — see 2.10 and 8.4 of the investigation summary).

### 4.3 — Cross-reference against the real game script (external source)

Under-used during the investigation -- the full Japanese Rockman 7 script is
documented on fan sites like `hondoori.wordpress.com` (see their
scripts/localizations section). Cross-referencing story ORDER against
already-confirmed entries (e.g. "entry 24 is 'you forgot something',
entry 26 is 'sorry' -- what's in between?") helps predict what an
unknown entry should contain, even without finding the exact text.

### 4.4 — Live diagnostics, no debugger (more expensive, but reliable)

Needs `IsDebugModeOn = true` (with it off, nothing is watched and no log is
written). If you suspect a specific entry (by index or `dest_rva`), add it
to `IsWatchedEntry` (`JapaneseText.cpp`) — it produces a detailed log via
DebugView (through `DebugOut`) every time that entry loads, no debugger
needed. `IsWatchedEntry` currently watches **every** entry (useful when you
don't know which entry it is, only how to recognize the screen visually),
which makes the log huge — narrow it back down to an explicit list once you
have found the entry.

### 4.5 — Dynamic debugger (x64dbg) — most expensive, but solves anything

Used successfully to:
- Find the real box-creation function (`FUN_140057a10`), impossible to
  find via static search alone (indirect call, no direct XREF).
- Confirm exactly which entry is loaded at a given moment (reading
  `[object+0x26]`, the index stored in the dialogue-box object).

Basic steps:
1. Breakpoint at `MMLC2.exe+<creation function RVA, 57a10>`.
2. When it hits, `Ctrl+F9` to finish the function.
3. Read `[RCX+0x38]` (pointer to the new object), then
   `[that_pointer+0x26]` (first byte = the entry's index).
4. Repeat with `F9` until it lands on the index you're looking for.

**Caution**: **memory** breakpoints (`bpm`, watchpoint-style) are
unreliable in this game -- they fire on any access within the same 4KB
**page**, not just the exact address, and frequently catch noise from
completely unrelated DLLs (D3D, graphics drivers). Prefer **execution**
breakpoints on specific function addresses instead.

### 4.6 — Decompile the whole initializer function (what solved the "12 dots")

When everything else fails: decompile
`UndefinedFunction_1400017b0` (`RVA_JP_MASTER_INIT`) **in full**, start
to finish. It builds all 171 pairs one by one, in sequence -- and
Ghidra, while analyzing it, will often **already identify and name**
literal strings that show up along the way (variables named
`PTR_s_CONTENT_ADDRESS`). That's how the "12 dots" in entry 25 were
found: Ghidra had already decoded and named the pointer on its own,
it just needed a full read-through to notice.

---

## 5. Dead ends (and two that turned out not to be)

Two entries that used to be listed here as "ruled out" were later **overturned**
(see section 4.0 of the investigation summary) — keep that in mind before
discarding an idea because an older note says it failed:

- **`PTR_DAT_14045fca8`, the "two tables" theory** — it was logged as wrong
  (thought to be "just the hardcoded English credits block"). It is in fact a
  **2-pointer array**: slot 0 points at the Japanese table (`0xE26B20`), slot 1
  at the English one (`0xE27B50`), and `FUN_1400561a0` selects between them.
- **`DAT_140942e50 + 0x400` as a "language flag"** — logged as tested and
  useless (same value on an EN boot and a JP boot). The real check has **two
  dereferences**: `*(int*)(*DAT_140942e50 + 0x400)`. A watchpoint on the
  literal `DAT_140942e50 + 0x400` address watched the wrong memory.
- **`0xE26B20` as "the whole Japanese table"** — first logged as just the
  credits block, then corrected: it really is the start of the clean,
  `0x18`-stride, 171-entry Japanese table. (`find_jp_text_sources.py` was
  still solving a real problem: the third field, the auxiliary-table pointer,
  isn't populated for every slot, and the array alone doesn't say how to
  decode the word format.)

Still true dead ends:

- **Real UTF-16 strings in the `.exe`** (found via `Search → For Encoded
  Strings`, UTF-16 charset) — they exist, but belong to **another game in the
  collection** (Mega Man 9, confirmed), not MM7. Classic MM7 only uses the
  custom tile system.
- **Searching for repeated 16-bit sequences (`XX 00`) to find pure
  punctuation** — only works for the main tile system; auxiliary-table text
  is raw ASCII, without that padding (see section 2.3).

---

## 6. Tools used, summary

| Tool | What it's for here |
|---|---|
| **Ghidra** (static) | Decompiling functions, reading the control-code dispatch table, finding the initializer function, reading strings Ghidra already named on its own |
| **x64dbg** (dynamic) | Confirming in real time which entry is loaded, finding indirectly-called functions (no static XREF) |
| **DebugView** | Capturing the diagnostic output the mod's hooks send through `DebugOut` (needs `IsDebugModeOn = true`), no debugger needed |
| **`decode_en_text.py`** | Offline extraction of the English table (see section 3.1 for its array-bleed caveat) |
| **`find_jp_text_sources.py`** (Ghidra script) | Finding the Japanese `{dest_rva, source_rva}` pairs |
| **`build_japanese_strings_json.py`** | Offline extraction + validation, doesn't need the game or Ghidra running |
| **Fan/wiki scripts (e.g. hondoori.wordpress.com)** | Cross-referencing story context against unknown entries |
