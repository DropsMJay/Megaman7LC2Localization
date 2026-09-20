# MM7Loc — Full Investigation Summary

Reference document for the reverse engineering behind MM7Loc. Covers: bugs
fixed, mechanisms decoded, project architecture, and what's still open.

Related documents (all in `docs/`):

- `JAPANESE_CONTROL_CODES.md` -- per-code reference table for the dialogue
  control codes (Japanese and English).
- `TEXT_FINDING_GUIDE.md` -- how to search for text when you don't know where
  it lives.
- `BOSSNAME_Font_Investigation.md` -- the boss-name banner, the stage-select
  flavor text and the intro cutscene text. A separate system with its own
  font and encoding, patched by `BossNameText.cpp`.

---

## 1. Project overview

**MM7Loc** is a MinHook-based DLL that translates the text of *Mega Man
Legacy Collection 2* (`MMLC2.exe`), injecting English and Japanese text
directly into the game's memory at runtime. Both languages share the
game's internal dialogue state machine (`FUN_1400561a0` and related
functions), just resolved differently depending on the language.

**Important context**: Japanese support was added on purpose — the
original US release has scenes the original Capcom localization team
never got around to translating due to development time constraints, and
simply reused generic placeholder text (like "YOU GET") in those spots
instead of leaving them in Japanese or removing them. The end goal is to
recover that content by translating the Japanese source for those
"disabled" scenes.

A companion reference, **`JAPANESE_CONTROL_CODES.md`**, holds a quick-lookup
table of every control code with its confidence level — check there first
for a fast answer before re-reading this whole document.

---

## 2. Major bugs resolved (in chronological order)

### 2.1 — Japanese text softlock (RESOLVED)

**Symptom**: Japanese dialogue would hang forever on certain entries.

**Root cause**: the extraction script (`build_japanese_strings_json.py`)
read each word of the 16-word header as **a single byte** (`b[0]`),
discarding the high byte. One specific word needed to be `512` (0x0200) —
the "typing speed" parameter — but got truncated to `0`, permanently
stalling the text-reveal timer.

**Fix**: the extraction script now reads the full word (`b[0] | (b[1]<<8)`);
the C++ side switched `vector<uint8_t> headerBytes` to
`vector<uint16_t> headerWords`.

### 2.2 — Wrong English table size (RESOLVED)

**Symptom**: 3 entire lines of dialogue (the Mega Man / Dr. Light
conversation about Wily's UFO) never showed up anywhere in the extraction.

**Root cause**: the dump assumed the English `StringEntry` table had
**163** entries (an unverified guess). The real size is **171**.

**Fix**: `DumpAllStrings` now scans up to 500 slots, stopping on its own
once it detects a long run of blank entries (10 in a row with
`TotalCharCount==0` and `LinePointers==nullptr`).

### 2.3 — Empty English dialogue box that "loads then unloads" (RESOLVED)

This was the longest bug to track down, with **two distinct, independent
root causes** that both needed fixing.

**Symptom**: certain English entries (first seen at 22/23/24, later also
25/26) opened the dialogue box empty, never showed any text, and closed
on their own.

**Root cause #1 — memory too far from the module**: the original patch
stored translated text in plain `std::string`/`std::vector` -- generic
heap, potentially **far** from the game module's base address.
`JapaneseText.cpp` already had this exact problem solved before
(allocating everything near the module via `VirtualAlloc` with hint
addresses like `moduleBase ± 0x10000000/0x20000000/etc`), but that fix
had never been ported to the English side.

*Fix*: `EnglishText.cpp` was rewritten to allocate **all** translated
text (line content plus the `LinePointers` pointer arrays themselves)
inside **one single `VirtualAlloc` block**, placed near the module — using
a simple bump allocator (`AllocateNearModule` + `BumpAlloc`). This fixed
entries 22/23/24.

**Root cause #2 — `TotalCharCount` also bounds how much of the control
header can be read**: even with the fix above, entry 26 kept breaking.
Deep investigation (see section 3) revealed that `TotalCharCount` doesn't
only mean "how many characters this text has" — the game **also** uses
that number as the limit on how many words of `ctx` (the control header
that runs before the real text, see 3.8) it's allowed to read before it's
allowed to draw anything. Short texts (few characters) didn't leave
enough room for the whole header, and the game stalled forever before
ever reaching the marker that redirects to the real text.

*Fix*: add a safety margin (`CTX_HEADER_SAFETY_MARGIN = 32`) on top of
the real `TotalCharCount`, generous enough to cover any plausible control
header, at no cost (this number is never shown on screen).

**Confirmed**: the two fixes together definitively resolved the symptom —
verified in a real play session, with live instrumentation showing
correct data all the way through the chain.

### 2.4 — English translations gaining a line silently lost the extra line (RESOLVED)

**Symptom**: translating an English entry from N original lines to N+1
lines -- the extra line simply never rendered, as if it didn't exist.

**Root cause**: `EnglishText.cpp` built the `LinePointers` array with
exactly N valid pointers and no terminator. The game apparently expects
this array to be **null-terminated** (standard C convention) to know
where the lines end; without it, reading could stop early (by luck,
whatever followed happened to be zero for the previously-tested
same-line-count cases) or read garbage.

*Fix*: append an explicit trailing `nullptr` to the `LinePointers` array,
and account for that extra pointer in the up-front buffer size
calculation (`totalBytesNeeded`).

### 2.5 — Straight quotes (`"`) fell back to `?` in English credits text (RESOLVED)

**Symptom**: `WARNING: no game byte mapped for U+0022` in the log; a
composer's nickname in the credits (`T."ANIE".N`) rendered with `?`
instead of quote marks.

**Root cause**: `EnglishCharTable.h` only mapped the curly closing quote
(`”`, U+201D) to the game's quote glyph (byte `0x22`) -- not the plain
straight quote (`"`, U+0022) actually used in that credits string. A
look at the game's own font sheet confirms it only has ONE quote glyph
in the first place (no separate straight/curly, opening/closing
variants), so this was purely a missing input-side mapping, not a font
limitation.

*Fix*: added `{ 0x0022, 0x22 }` as an additional entry pointing to the
same byte as the existing `{ 0x201D, 0x22 }` -- both Unicode inputs now
resolve to the one glyph the font actually has.

### 2.6 — `[DrawClosingMark]` without an immediate `[NewLine]`/`[SetLeftMargin]` after it renders the new box shifted (IDENTIFIED, translator-authoring issue, not an engine bug)

**Symptom**: text after `[DrawClosingMark]` starts wherever the cursor happened to
be left by the previous box, instead of the left margin -- looks
visually warped/shifted, sometimes overlapping the previous box's tail.

**Root cause**: confirmed via decompile (see 3.10) that `[DrawClosingMark]`
creates or destroys nothing -- the box object is created exactly **once
per entry** (`FUN_140057a10`), never per `[DrawClosingMark]`. The left-margin
reset only happens when `[NewLine]` (or `[SetLeftMargin]` in the header)
explicitly runs. If a translation adds a `[DrawClosingMark]` transition without a
`[NewLine]` right after it, nothing resets the cursor.

**This is not a bug to fix in the engine** -- it's a rule translators
need to follow: **always put `[NewLine]` immediately after `[DrawClosingMark]`**
unless you've specifically confirmed the new box should continue from
the old cursor position. `build_japanese_strings_json.py --validate
GameTextJP.json` checks for this automatically.

**12 entries currently violate this rule** in the original game data
itself (native to the untranslated Japanese text, not something the
translation introduced): **9, 11, 13, 34 (×4 occurrences), 35, 45, 78,
85, 107**. These need a `[NewLine]` (or equivalent reflow) inserted when
translated, or they'll render shifted just like the original does.

### 2.7 — `header_bytes` cut one word short for the majority of entries (RESOLVED)

**Symptom**: for a large fraction of entries (106 out of 171 --
confirmed by pattern, not a guess), `header_bytes` ended exactly on a
control-code opcode that requires a parameter (`[SetSpeaker?]`,
`[SetTypeSpeed]`, `[SetInitialTypeSpeed?]`, `[RepeatBlankTile]`,
`[PacingTick]`, `[WaitFrames]`, `[TabToColumn]`, `[SetLeftMargin]`,
`[SetTypeSound?]`), with that opcode's own parameter word missing --
silently absorbed into the START of the `text` field instead, disguised
as if it were the first real character.

**Root cause**: `find_header_and_text`'s boundary heuristic
(`is_strong_text_signal`) only checks "does this word look like a real
kana byte" (`hi==0 and lo>=0x80`) -- it has no concept of
opcode/parameter pairing. If a parameter value happened to fall in that
"looks like kana" range, the heuristic declared the boundary one word
too early, splitting a two-word instruction in half.

**Practical impact**: for any of these 106 entries, if translated,
the translator's own **first typed character** would get silently
consumed as that instruction's numeric parameter (most commonly the
margin value) instead of being drawn -- the rest of the translation
would still render, just starting one character short and at whatever
margin that stray value happened to decode to. This is the same class
of bug that caused the original margin-shift symptom that kicked off
this whole investigation (see 2.6), just for the `header`/`text` split
itself rather than a missing `[NewLine]`.

**Fix**: `find_header_and_text` no longer trusts the window-scan's
candidate boundary directly. It now walks word-by-word from position 0
of the entry, using the exact same two-word-instruction rule the real
decoder (`decode_word_array`) uses (including the aux-table jump and
dakuten/han-dakuten, which also consume a second word), and snaps the
boundary forward to the first instruction-aligned position at or after
the heuristic's original guess. This can never land mid-instruction,
regardless of what the parameter value looks like. Verified via a full
round-trip test (re-encoding every entry's `header_decoded` back into
words and comparing against the original `header_bytes`) across all
171 entries before and after the fix.

### 2.8 — Two entries (71, 72) wrongly extracted as empty (RESOLVED)

**Symptom**: entries 71 and 72 extracted with a 0-word header and an
empty `text` field, indistinguishable at a glance from genuine
non-dialogue noise (see 2.9).

**Root cause**: `find_header_and_text`'s search window
(`max_header_words`) defaulted to 160 words. Both entries have
unusually long headers -- 176 and 240 words respectively, part of the
same credits/setup-heavy family as entries like 69, 70, 73-76 -- so the
real text (confirmed real dialogue: entry 71 is Roll asking to trim the
overgrown garden trees, matching the Slash Claw description in the
Japanese script; entry 72 is a shop greeting) sat past where the
search gave up.

**Fix**: raised the default `max_header_words` to 500. Re-verified with
an even larger window (2000) that no other entries are still being cut
short.

### 2.9 — Entries 51 and 58 confirmed as genuine non-text noise (not a bug)

Unlike 71/72, these two do **not** contain real dialogue at any search
window size (tested up to 2000 words). Manually reading the raw bytes
at their addresses shows data that doesn't match the dialogue format at
all -- entry 51 looks like small flag/counter values, entry 58 looks
like a table of large pointer-shaped values paired with a small
constant. Neither starts with anything resembling the near-universal
`[SetTypeSpeed]` opening every real dialogue entry has. These are false
positives from `find_jp_text_sources.py`'s pair-finding heuristic (which
walks PCode/SSA and can occasionally latch onto unrelated data that
happens to look like a `{dest_rva, source_rva}` pair) -- consistent
with the "~33 noise entries" mentioned in earlier investigation notes.
Not fixable on the `build_japanese_strings_json.py` side; would need a
fix in `find_jp_text_sources.py` itself to stop generating these two
pairs in the first place.

### 2.10 — Entry 25's missing auxiliary-table address (RESOLVED)

**Symptom**: entry 25's `header_decoded` showed
`[AUX_TABLE_JUMP:UNRESOLVED]` -- the only entry in the whole 171 still
showing this after every other fix in this session. The underlying
`header_bytes` was still correct/preserved (this only affected the
informational `header_decoded` field), but it meant nobody could see
*what* that unresolved reference actually was.

**Long investigation, short resolution**: this turned out to be the
literal answer to a multi-message "where's the 12-dot screen"
side-quest -- entry 25's header embeds `"............"` (12 literal
ASCII periods) via the same 0x0100 aux-table mechanism used for
credits names and "YOU GET". `AUX_TABLE_BY_ENTRY_INDEX` (a
hand-populated map from entry index to that entry's auxiliary
pointer-table RVA, built from an earlier manual Ghidra pass and never
covering all 171 entries) simply didn't have entry 25 in it.

**How it was actually found**: static content search failed (word-level
16-bit pattern search for repeated dot bytes found nothing, because
auxiliary-table text is raw one-byte-per-character ASCII, not
16-bit-padded like the main tile system -- a real blind spot in the
search approach, corrected in `TEXT_FINDING_GUIDE.md`
section 2.3). What worked was decompiling
`UndefinedFunction_1400017b0` (`RVA_JP_MASTER_INIT = 0x17b0`, the
function that builds the entire Japanese table at startup) **in full**
-- Ghidra had already auto-named the relevant global
`PTR_s_............_14057a3a0` while analyzing it, since it recognized
the literal string content on its own. Confirmed live: resolving that
pointer yields exactly `"............"`, 12 characters.

**Fix**: added `25: 0x57a3a0` to `AUX_TABLE_BY_ENTRY_INDEX`. Zero
entries now show `AUX_TABLE_JUMP:UNRESOLVED` anywhere in the corpus.

**Still open** (see section 6): the 12 dots resolve correctly now, but
they still land inside `header_bytes`, not `text` -- the boundary
heuristic doesn't treat a long run of `.`/`。`/`・` as a "real text"
signal the way it does real kana, so this content remains invisible to
anyone translating entry 25 from the `text` field alone. Not fixed yet.

### 2.11 — Dakuten/han-dakuten-initial words losing their first character (RESOLVED)

**Symptom**: 7 entries (19, 21, 46, 48, 52, 60, 66) had a real,
meaningful word or name missing its first character in `text` -- e.g.
entry 19's `text` started with `"ってん！..."` instead of the real
`"がってん！..."` ("Got it!"); entry 60 started with `"ャンクマン"`
instead of `"ジャンクマン"` ("Junk Man", a Robot Master name). The
missing character was sitting at the very end of `header_bytes`
instead, one entry earlier discovered by manually reconstructing the
join between the two fields (the fix was to start each entry with an
explicit check for one leftover character at the header/text seam).

**Root cause**: `is_strong_text_signal` (the check `find_header_and_text`'s
window scan uses to decide "does this word look like real text")
requires a single word to be ≥0x80 by itself. A dakuten/han-dakuten
character is two words -- the trigger (`0x0C`/`0x0D`, always a small
value, never ≥0x80) followed by the base kana's own byte. The scan
would find the base-kana word "strong" and correctly detect that real
text was near, but always started the boundary one word (one full
character) too late, silently stranding the trigger+lookahead pattern
in the header and the reader never seeing that first voiced/semi-voiced
character at all.

**Fix**: added `is_dakuten_lead()` -- checks whether a word is a
dakuten/han-dakuten trigger immediately followed by a valid base-kana
word, and if so treats that position as equally valid evidence of
"real text starts here" as the existing ≥0x80 check. Applied both to
the window's hit-count and to picking the actual boundary offset.
Verified live: all 7 previously-affected entries now show the complete
word in `text`; the full-corpus round-trip test (header_decoded
re-encoded against header_bytes) shows no new divergences beyond the
already-known, expected aux-table-resolution cases (see 2.10).

This directly answers "is there a way to translate the text trapped in
the header?" for this specific class of case: **yes** -- once the
missing first character is correctly classified as `text` instead of
`header`, it's fully editable like any other word. Punctuation-only
cases like entry 25's 12 dots (see 2.10) are a *different* situation --
there's no missing "word" to recover, just a pause that (arguably)
doesn't need translating in the first place, and the underlying issue
(the boundary heuristic not treating a punctuation run as "real text")
is still open.

### 2.12 — `[ConditionalTrigger?]` tag name mismatch between Python and C++ (RESOLVED)

**Symptom**: reported live in-game -- background music that should stop
at the end of entry 24 and resume at the end of entry 28 (during the
opening cutscene) stopped correctly, but never resumed, even with
completely untranslated, original Japanese text.

**Root cause**: `JapaneseText.cpp`'s encoder recognized the literal
string `[ConditionalTrigger][` (without the `?`), but
`build_japanese_strings_json.py` always emits `[ConditionalTrigger?][`
(with it) -- the two sides were never actually saying the same tag
name. Every occurrence of this tag anywhere in a JSON `text` field
(here, entry 28's closing footer, `[ConditionalTrigger?][14]`) silently
fell through the C++ tag-matching logic entirely and got processed
character-by-character instead, producing wrong bytes at that exact
position when patched -- even for entries nobody had translated.
`header_bytes` occurrences (like entry 25's own
`[ConditionalTrigger?][32]`, sitting in its *header*) were unaffected,
since header content is copied verbatim and never passes through this
tag parser at all -- only entries where the tag shows up inside `text`
were at risk.

**Fix**: corrected the C++ literal to `[ConditionalTrigger?][` and its
length offset (21 -> 22 chars). Cross-checked every other `?`-suffixed
tag (`[SetSpeaker?]`, `[SetInitialTypeSpeed?]`, `[SetTypeSound?]`,
`[ClearIconList?]`) between both files to confirm none of them had the
same mismatch -- only `[ConditionalTrigger?]` did.

**Bonus finding**: this accidentally supplies strong behavioral
evidence for what `FUN_1400fb840` does (see section 6) -- entry 28's
`[ConditionalTrigger?][14]` (20 > 16, so the call fires) sits exactly
where the music resumes in-game. Not proven by decompile, but a
strong, live-tested clue.

**Lesson for future new tags**: whenever a new tag is added to the
Python decoder AND the C++ encoder in the same session, diff the two
tag name lists directly (e.g. `grep` both files for the tag literals)
rather than trusting that copy-pasting the name into both files went
correctly -- this exact class of typo is easy to introduce and, for
tags that only show up in `header_bytes` (like this one did until entry
28's *text* footer surfaced it), can go unnoticed by every check in
this document except an actual in-game playtest.


### 2.13 — `EnglishText.cpp` out of sync with the new `GameTextUS.json` format, and the `LinePointers` terminator missing again (RESOLVED)

Found while reviewing `EnglishText.cpp` against the rebuilt English
extraction (section 8). Two independent problems, both fixed in the
same edit:

**Problem 1 — format mismatch.** The loader still expected the old
format `"index": ["line1", "line2", ...]`. The current
`GameTextUS.json` (produced by `decode_en_text.py`, see 8.6) is
`"index": {"array_rva": "0x...", "text": "line1[NewLine]line2..."}`.
Iterating that object with the old loop would have treated
`"0x573868"` (the `array_rva` string) and the whole joined text as two
"lines", and the literal `[NewLine]` would have gone through the
encoder (a `[` falls back to nothing special).

*Fix*: `LoadLocalization()` now reads the `text` field and splits it on
the literal `[NewLine]` (new helper `SplitOnNewLineTag`) -- each piece
becomes one `char*` in the game's `LinePointers` array. `array_rva` is
informational only and ignored. The old shapes (bare string, array of
line strings) are still accepted. An entry with empty text (51, 58,
162, 169) is skipped, leaving the game's original entry untouched.

**Problem 2 — regression of 2.4.** Section 2.4 documents the trailing
`nullptr` terminator on the `LinePointers` array, but the
`EnglishText.cpp` in the repo at review time did not have it
(`finalLinePointers` was copied as-is, and `totalBytesNeeded` didn't
count the extra slot).

*Fix*: PASS 2 now appends an explicit `nullptr` before copying the
array into the near-module block, and PASS 1 counts
`(lines + 1) * sizeof(char*)` for it. `EnglishEntryLocation::lineCount`
deliberately excludes the terminator.

**Status**: confirmed working in a quick in-game test after the change
(reported by the project owner; the individual entries tested were not
recorded).

**Lesson**: when a fix is recorded in this document, re-check that it is
actually present in the source that gets built -- 2.4 was "resolved" on
paper but absent from the file.

---

## 3. Japanese text control codes — decoded so far

Japanese text uses 16-bit words. Values `≤0x15` are dispatched as control
codes (jump table `PTR_LAB_1404531a0`, one function per index); values
`>0x15` (except `0x0100`) are normal glyphs; `0x0100` triggers a special
redirect (see 3.9). **A full per-code reference table with confidence
levels lives in `JAPANESE_CONTROL_CODES.md`** -- this section covers the
handful of codes with enough backstory to be worth narrating, plus the
architecture-level findings.

Codes decoded in the original investigation:

- `0xCA` `[DrawClosingMark]` — draws the closing "。" (see 3.10 for what it
  does NOT do)
- `0x02` `[SetTypeSpeed][XX]` — persistent typing pace, exact
  mathematical proof
- `0x07` `[WaitFrames][XX]` — one-off pause
- `0x08` (pair) `[NewLine]` — visible line break (see 3.10.1 for the
  lone/unpaired case, decoded later)
- `0x0A` `[SetLeftMargin][XX]` (renamed from `[ResetCursorX]` -- the old
  name implied a one-off action; it actually sets a **persistent
  anchor** that `[NewLine]` reapplies every time, which "reset" doesn't
  convey) — sets the line-start margin
- `0x0F` `[ForceWait]` — blocks skipping the pause via the confirm
  button
- `0x0C` / `0x0D` — dakuten / han-dakuten combiners
- `0xC2` / `0xC3` — full-width `？`/`！`

Codes decoded in a later session (see `JAPANESE_CONTROL_CODES.md` for
full detail on each):

- `0x01` `[SetSpeaker?][XX]` — writes `[+0x3c]`, passed to the
  box/balloon resolver; hypothesis: which character the balloon points
  at
- `0x03` `[SetInitialTypeSpeed?][XX]` — writes `[+0x34]` (a field
  already known, no writer found until now); hypothesis: a base/initial
  typing speed distinct from `[SetTypeSpeed]`. Never actually used in
  any of the 171 entries.
- `0x04` `[RepeatBlankTile][XX/XXXX]` — draws N blank tiles, advancing
  the cursor; hypothesis: spacing/indent
- `0x06` `[WaitForConfirm]` — the blinking "▼ press to continue" prompt
- `0x08` (lone/unpaired) `[AdvanceCursorLine]` — turned out to be the
  **exact same handler** as `[NewLine]` (same dispatch slot, index 8),
  just called once instead of twice -- not a separate mystery, half the
  effect of a full line break
- `0x09` `[TabToColumn][XX/XXXX]` — absolute cursor-X jump using the
  margin anchor plus an undocumented field (`[+0x38]`); only confirmed
  use is the credits screen (entry 68, positioning "PRODUCER"/
  "PROFESSOR F"). **Bug found and fixed**: the parameter isn't always a
  single byte (entry 68's real value is 1360/0x0550) -- the decoder now
  accepts the full 16-bit word instead of silently misreading the high
  byte as a stray glyph.
- `0x0B` `[IncrementCounter]` — increments a global counter, unrelated
  to this entry's state
- `0x10` `[SetTypeSound?][XX]` — writes `[+0x41]`, read after every
  glyph to call a sound-effect-manager-shaped function
  (`FUN_1400fba90`, bounds check + active-sound array + vtable
  play/stop calls); very strong hypothesis: a per-character "typewriter
  blip"
- `0x0E` `[ClearForceWait]` — clears `[+0x3e]`, the exact same field
  `[ForceWait]` (`0x0F`) sets to 1. The "undo"/cancel counterpart to
  `[ForceWait]`.
- `0x11` `[PlaySound][XX/XXXX]` — reads a parameter and calls
  `FUN_1400fba90(value, 0)` directly (the same function
  `[SetTypeSound?]` uses), playing a sound once immediately instead of
  setting up a persistent per-character one.
- `0x12` `[ConditionalTrigger?][XX/XXXX]` — reads a parameter; calls
  `FUN_1400fb840()` (not decompiled) if it's greater than 16.
  Hypothesis: a threshold-gated conditional trigger/flag.
- `0x13`/`0x14` `[ClearIconList?]` — two raw values, **confirmed
  synonyms via a direct Ghidra XREF** (both point at `FUN_140056c70`).
  Walks a linked list of icon/sprite items on the dialogue box,
  destroying or hiding each one, then empties the list. Only seen as
  the first word of weapon-description entries. Hypothesis: clears the
  previous weapon's icon before the new one shows.
- `0x15` `[ResetToMarginSameLine]` — clears `[+0x38]` (the same
  undocumented field `[TabToColumn]` uses) and copies the margin anchor
  back into the cursor, like half of `[NewLine]` (return to margin,
  without moving down a line).

**Resolved, not a conflict**: dispatch-table indices 12 (`0x0C`) and 13
(`0x0D`) also have decompiled handlers (`FUN_140056a90`/`FUN_140056ad0`)
that push the cursor up one line-step, draw a small fixed tile (`0xC9`/
`0xC7`), then pop the cursor back. At first this looked like it might
conflict with the already-confirmed dakuten/han-dakuten combiners (which
also use `0x0C`/`0x0D`) -- it doesn't. It's the actual glyph-level
mechanism *behind* dakuten: `0x0C` draws the raised `゛` mark, then the
next word (the base kana's own byte) draws normally on the next loop
iteration, landing at the same column. No separate tag needed -- the
existing precomposed-character handling (`DAKUTEN_MAP`/`HANDAKUTEN_MAP`)
already represents this correctly at a readable level.

**Important correction from that session**: `[TabToColumn]` was
initially (wrongly) believed to also appear in the common mid-dialogue
`[DrawClosingMark]` footer pattern. That was a misread -- `0x04` wasn't decoded
yet at the time, so its own parameter word (which happened to be `09`,
i.e. `[TabToColumn]`'s own opcode) was being read as a separate,
coincidental `[TabToColumn]` invocation. Once `0x04` got its own
handler, the footer decoded correctly as `[RepeatBlankTile][XX]` +
`[WaitForConfirm]` (or `[AdvanceCursorLine]`), with no `[TabToColumn]`
in it at all.

### 3.8 — `ctx` (the first field of `StringEntry`)

Every `StringEntry` (English) has a `ctx` field (offset `+0x00`) that had
never been used. It turns out to point at a **mini control header**, in
the same format as the Japanese header — it runs automatically before the
real text appears. Real decoded example:

```
SetTypeSpeed(4), sets [+0x34]=0x200, idx=16?(32), ResetCursorX(70), idx=1?(32), ...
```

This `ctx` is **shared/identical** across several different entries — it
is NOT what causes entry-specific bugs (tested and ruled out as the cause
of entry 26 breaking).

### 3.9 — Auxiliary table (`0x0100`) — embedded Latin-text mechanism

When Japanese text needs to embed loose Latin letters mid-sentence
(proper names, acronyms like "UFO"), it uses the `0x0100` marker: the
next word (`uVar5`) indexes into an **auxiliary pointer table**
(`plVar2[2]`, the third field of the array — which **is literally the
same `LinePointers` field English uses**, reused!), resolving a pointer
to a plain ASCII string, read by the same `char*` mechanism English uses.

**Confirmed live**: correctly resolved to `"UFO"`.

**Automatically resolved during extraction**: `build_japanese_strings_json.py`
now reads each entry's auxiliary table address (extracted statically from
the Japanese initializer's decompile) and resolves the real text, without
needing to run the game. **30 of 171 entries** used this mechanism — all
resolved, 0 exceptions.

### 3.10 — How the dialogue box is actually created (confirmed via dynamic debugging)

For a long time it wasn't clear what `[DrawClosingMark]` (`0xCA`) really *did* --
static reading of `FUN_1400561a0` and friends never turned up a real
"close/reopen the box" branch, and a scalar search for `0xCA` across the
whole binary only matched unrelated code (stack offsets, constants).

The answer needed a **dynamic** debugger (x64dbg): a breakpoint on
`FUN_140056170` (the constructor half of the box object's vtable pair,
identified from `PTR_FUN_14050aab0`), triggered by opening any dialogue
in-game, with the call stack read at the break. That led to
**`FUN_140057a10`** -- the real box-creation function -- one level up
from everything decompiled before.

`FUN_140057a10`:
- Is called **exactly once per entire JSON entry**, never per `[DrawClosingMark]`.
- Destroys any still-active previous box (via the vtable destructor
  already identified) and allocates a fresh `0x60`-byte object.
- Zeroes every field mapped during this whole investigation: `[+0x18]`
  (word array), `[+0x20]` (read index), `[+0x28]` (English text
  pointer), `[+0x3c]` (`[SetSpeaker?]`'s target), `[+0x50]`
  (drawn-character counter), `[+0x58]` (conversation-finished flag).
- Sets two global flags (`_DAT_1408fd878`, `DAT_1408fd952`) that
  `FUN_1400561a0` clears when the conversation ends -- confirms they
  mean "a dialogue is currently active".
- Copies the current entry's index (`DAT_1408fd953`, the same
  `EVENTIDX` visible in `TextEntryDebug`'s log output) into the object.
- Sets the initial phase to `LAB_140056260` (the already-known word
  dispatcher).

**Conclusion**: `[DrawClosingMark]` creates or destroys nothing. Every "box"
within one entry is the **same object**, the whole time -- no
close/reopen mechanism exists. The "new page" effect players see is
purely the combination of: `[DrawClosingMark]` draws "。", `[WaitForConfirm]`
pauses for input, and `[NewLine]` (or the header's `[SetLeftMargin]`)
manually moves the cursor back to the left margin. This is exactly why
skipping the `[NewLine]` after a `[DrawClosingMark]` produces a shifted box (see
2.6) rather than some other kind of failure -- there's no "box system"
underneath to fail, just a cursor that wasn't told to move.

---

## 4. Project architecture (file map)

### 4.0 — MAJOR CORRECTION: both languages share one unified table-selection mechanism

**This overturns two "dead ends" from earlier investigation notes** (both
originally logged as ruled out, before this was found). Found by
decompiling `FUN_1400561a0` (the shared dialogue state machine already
known to be common to both languages) in full, specifically the branch
that resolves `param_1 + 0x18` (the dialogue object's word-array/string
pointer) when it's still unset:

```c
*(param_1 + 0x18) =
    *(&PTR_DAT_14045fca8 + (language_flag != 0 ? 8 : 0))
    + (*(byte*)(param_1 + 0x26)) * 0x18;
```

Where `language_flag` is `*(int*)(*DAT_140942e50 + 0x400) != 0` and
`param_1 + 0x26` is the per-entry index byte already used throughout
this document (confirmed live via x64dbg many times) to identify which
entry is currently loaded.

**What this means**: `PTR_DAT_14045fca8` is a **2-pointer array** --
slot 0 (`DAT_14045fca8` itself) points at `DAT_140e26b20` (the start of
the Japanese table), slot 1 (`DAT_14045fcb0`, 8 bytes later) points at
`RVA_TABLE_START` (`0xE27B50`, the confirmed English `StringEntry[]`
base). **Both languages are resolved by the exact same formula**
(`base[language] + index*0x18`), through the exact same code path, in
the exact same function. The only thing that differs between languages
is the *shape of the data* at each 0x18-byte slot (English: a simple
`{char* text, u32 len, ...}` triple; Japanese: the `{word_array_ptr,
len, aux_table_ptr}` triple already documented in section 3.8) -- not
the indexing/selection mechanism itself, which is fully shared.

**Corrects dead end #1** (`0xE26B20` as "the whole Japanese table"):
earlier notes concluded this was just the credits block, with the real
table being ~170+ scattered heterogeneous pairs found individually by
`find_jp_text_sources.py`. **This was wrong, or at least incomplete.**
`0xE26B20` genuinely IS the start of a clean, indexed, `0x18`-stride
array -- confirmed by decompiling `FUN_1400017b0` in full (see 2.10)
and observing the fill addresses (`140e26b20`, `140e26b38`,
`140e26b50`, ...) increment by exactly `0x18` each time, for exactly
171 entries, ending at `140e27b28`. The `find_jp_text_sources.py`
pair-hunting approach was solving a real problem (that array's *third
field*, the auxiliary-table pointer, isn't populated for every slot,
and the array alone doesn't tell you how to decode the word-array
format) but the underlying premise that "there's no single table" was
not quite right.

**Corrects dead end #2** (`DAT_140942e50 + 0x400` as a "language
flag"): earlier notes say this was tested directly with a watchpoint
and gave the same value on an EN boot and a JP boot. The likely
explanation: the real check is `*(int*)(*DAT_140942e50 + 0x400)` --
**two dereferences**, not one. `DAT_140942e50` is itself a pointer; the
flag lives at `[the value DAT_140942e50 points to] + 0x400`, not at
`DAT_140942e50`'s own address + `0x400`. A watchpoint placed on the
literal `DAT_140942e50 + 0x400` address (rather than on where its
*contents* point plus `0x400`) would be watching the wrong memory
entirely, explaining why it never showed a difference. Not re-tested
live this session -- flagged for a future session to confirm with the
corrected address calculation.

---

| File | Role |
|---|---|
| `EnglishText.cpp/h` | Applies the English text patch. Loads `GameTextUS.json` (embedded via `.rc`), splits each entry's `text` on `[NewLine]` into one `char*` per line (see 2.13), and allocates everything in one `VirtualAlloc` block near the module (`AllocateNearModule`/`BumpAlloc`), including a null-terminated `LinePointers` array (see 2.4). |
| `JapaneseText.cpp/h` | Applies the Japanese text patch. Same near-module allocation pattern. Loads `GameTextJP.json` (embedded via `.rc`). Contains `EncodeJapaneseWordArray`, which recognizes every `[XX]` label and reconstructs the correct control bytes (round-trip guaranteed). |
| `EnglishTextEncoding.cpp/h` + `EnglishCharTable.h` | Character-to-byte encoding for English (the game font's table, including hiragana at positions `0x90`-`0x9B`, restored to the original, and both straight/curly quote inputs mapped to the one quote glyph -- see 2.5). Also recognizes the shared control-code tags with one-byte parameters (see 8.7). |
| `EnglishTextDumper.cpp/h` | `DumpAllStrings` — dumps the game's original `StringEntry[]` table to `mm7loc_dump.txt`, used to rebuild the JSON. |
| `EnglishArrayBoundaries.h` | List of known array-start addresses, used by the dumper to know where one entry ends. |
| `TextEntryDebug.cpp/h` | All diagnostic hooks (`FUN_1400561a0`, `Phase56520`, `Phase56720`, `WordDispatch`, `NewLineHandler`; the `CharReader` hook lives in `JapaneseText.cpp`). Every `Install*Hook` returns immediately when `IsDebugModeOn` is false. Reusable tools: `AddEnglishWatchedIndex`, `SetStateWatchedBuffer`, etc -- **not called automatically** in the production flow; they need to be wired in manually when investigating something new. `IsWatchedEntry` (`JapaneseText.cpp`) currently watches every entry while debug mode is on. |
| `Logging.h` | `IsDebugModeOn` (the master switch), `LogLine`/`LogLineReset` -- write to `mm7loc_log.txt`, path anchored to the DLL's own folder (doesn't depend on the process's working directory) -- and `DebugOut`, the gated wrapper around `OutputDebugStringA` (DebugView). |
| `dllmain.cpp` | Entry point, installs every hook in the right order. The built DLL is deployed as an `.asi` file (copied manually, no post-build step). |
| `resource.h` / `MM7Loc.rc` | `IDR_GAMETEXT_US`/`IDR_GAMETEXT_JP` -- both JSONs embedded as an `RCDATA` resource inside the DLL itself (no longer depend on an external file). |
| `GameTextUS.json` / `GameTextJP.json` | The actual scripts -- what you edit to translate. |
| `build_japanese_strings_json.py` | Extracts `GameTextJP.json` fresh from `MMLC2.exe` + a Ghidra pairs file. Also has a **`--validate GameTextJP.json`** mode (no exe needed) that scans an already-translated JSON for `[DrawClosingMark]` without an immediate `[NewLine]`/`[SetLeftMargin]` after it (see 2.6) -- run this after any translation edit. |
| `BossNameText.cpp/h` + `BossNameText.json` | Patches the boss-name banner, the stage-select flavor text and the intro cutscene text (a separate system with its own fonts -- see `BOSSNAME_Font_Investigation.md`). Hooks `FUN_14005dd80`; has its own near-module allocator. Loads `BossNameText.json` (embedded, `IDR_BOSSNAMETEXT`). |
| `MiscText.cpp/h` + `MiscText.json` | Patches the "system label" text (Sound Test, Versus Mode / Player Select, "THANK YOU FOR PLAYING!", the opening history/copyright screen) found behind `FUN_1401003c0` / `FUN_140100610`. These are static `.rdata` pointer tables (`RVA_TABLE_A = 0x56d110`, `RVA_TABLE_B = 0x56d020`), so there is no init function to hook: the table slots are overwritten once at startup. Loads `MiscText.json` (embedded, `IDR_MISCTEXT`). |
| `ResourceLookupDebug.cpp/h` | Hook on `FUN_140060620` (resource lookup by name). Stores the resolved BOSSNAME resource pointer in `g_bossNameResourcePtr` and, in debug mode, logs every new resource name. Diagnostic only: nothing else currently reads that pointer. |
| `GlyphDrawDebug.cpp/h` | Diagnostic-only `CHARDUMP` hook on `FUN_14005dd80`. **Not part of the build**, because `BossNameText.cpp` hooks the same function and MinHook allows one hook per address -- swap one for the other to use it. |
| `tools/` | The Python scripts (`decode_en_text.py`, `build_japanese_strings_json.py`, ...) and address lists used to extract the JSON files. Not needed to use or build the mod. |
| `JAPANESE_CONTROL_CODES.md` | Quick per-code reference table -- confidence level, parameter shape, and a one-line description for every decoded control code, plus the English notes. |
| `TEXT_FINDING_GUIDE.md` | Methodology for finding text in the game when you don't know where it lives. |
| `BOSSNAME_Font_Investigation.md` | The boss-name / flavor-text / intro-text font system. |

### 4.1 — `header_decoded`: a readable header, but not the source of truth

Every entry in `GameTextJP.json` now also has a `header_decoded` field
alongside `header_bytes` -- the same raw header words, decoded with the
exact same control-code logic used for `text` (via a small
`WordListReader` adapter that lets `decode_word_array()` run against an
in-memory word list instead of the exe). It exists purely for
readability (e.g. instantly seeing `[SetSpeaker?][20]` instead of
puzzling over a `1, 32` pair buried in a 16-number list) -- it is
**not** read by `JapaneseText.cpp` at all, and isn't meant to replace
`header_bytes`.

**Why not replace it**: a round-trip test (re-encoding every entry's
`header_decoded` back into words and comparing against the original
`header_bytes`) was run across all 171 entries before considering that.
It caught two real things:
- A genuine bug (now fixed) -- `[SetLeftMargin]` only supported a
  1-byte parameter, but some entries need a margin value over 255
  (390, 643).
- A deeper, still-open question for ~22 entries, where the header
  contains an `[AUX_TABLE_JUMP:UNRESOLVED]` marker or very large,
  pointer-shaped values that don't decode cleanly (see section 6). For
  these, `header_decoded` does NOT reconstruct the original
  `header_bytes` exactly.

`header_bytes` remains byte-for-byte preserved and is what actually
gets patched -- it doesn't depend on any of this decoding being
correct. `header_decoded` is regenerated fresh on every extraction and
safe to ignore.

---

## 5. Important conventions

- **`IsDebugModeOn`** (in `Logging.h`): the master switch for every
  diagnostic. `true` = logging, diagnostic hooks and the per-entry watcher
  threads are active; `false` = the mod runs silently, writing nothing to
  disk and installing no diagnostic hooks. It must be `false` in a shipped
  build.
- **Two log channels, both gated by that switch**: `LogLine()` writes to
  `mm7loc_log.txt` (a file); `DebugOut()` sends to the debugger output,
  captured only by **DebugView** (it never goes to the file). Always check
  which one you need before asking for a log. Don't call
  `OutputDebugStringA` directly, or the switch won't cover it.
- **Confirmed address formula**: `StringEntry[N]` (English) lives at
  `moduleBase + 0xE27B50 + N*0x18` -- with **no** extra language offset.
  An old note in the code claimed a `+8` was needed; that was tested and
  **disproven** with live evidence (828/828 real addresses matched
  without the `+8`).
- **Round-trip guaranteed**: any untranslated entry (Japanese or English)
  reconstructs the exact original bytes, even after going through the
  decoder with human-readable labels.
- **`[DrawClosingMark]` always needs a `[NewLine]` right after it** (or
  `[SetLeftMargin]`, if you're deliberately reproducing the header-level
  mechanism) -- see 2.6 and 3.10. There is no automatic reset. Run
  `build_japanese_strings_json.py --validate` to check.
- **Names ending in `?`** (`[SetSpeaker?]`, `[SetInitialTypeSpeed?]`,
  `[SetTypeSound?]`) mark a well-supported but **not yet in-game
  confirmed** hypothesis -- the mechanism (what field gets written, what
  reads it) is decompiled and solid, but the exact *purpose* hasn't been
  isolated with a dedicated test. Don't rename away the `?` without an
  actual confirming test.

---

## 6. What's still open (low priority, nothing blocking)

- **`FUN_1400fba90`** (the sound-effect function called by
  `[SetTypeSound?]`/`[PlaySound]`) -- **fully decompiled and confirmed**
  (see 8.8): a real sound-effect manager.
- **`FUN_1400fb840`** (called by `[ConditionalTrigger?]` when its
  parameter exceeds 16) -- **decompiled: it plays/resumes music** (see
  8.8). The earlier behavioral clue (entry 28's
  `[ConditionalTrigger?][14]` resuming the cutscene music, hidden until
  the bug in 2.12 was fixed) is now backed by the decompile.
- **`[+0x38]`** (used in `[TabToColumn]`'s formula, and zeroed by
  `[ResetToMarginSameLine]`) -- the reader formula is now known (see
  8.9), but **the code that WRITES this field has still not been
  found** (Ghidra doesn't link the references automatically). Next
  step: a write breakpoint on `<dialogue object>+0x38` in x64dbg.
- **Entry 25's leading "............" (12 dots) lives entirely inside
  `header_bytes`, invisible in `text`.** (The aux-table lookup itself
  is already fixed -- see 2.10 -- this item is specifically about the
  boundary heuristic not surfacing it in `text`.) Found by fully decompiling
  `FUN_1400017b0` (the function that builds the whole Japanese table,
  `RVA_JP_MASTER_INIT = 0x17b0`) and spotting a Ghidra-auto-named
  `PTR_s_............_14057a3a0` global among entry 25's fields. The
  boundary heuristic (see 3.10-adjacent `find_header_and_text`) only
  recognizes real kana (byte ≥0x80) as "real text starts here" -- since
  the 12 dots are plain ASCII (`0x2E`), the scan slides straight past
  them into the header, and they never surface in `text` at all. A
  translator editing entry 25 today would have no way to know this
  pause exists or to translate/adjust it -- it's permanently baked in
  as the original Japanese `header_bytes`, which is never touched.
  Fixing this properly would mean teaching the boundary heuristic to
  ALSO recognize a "long run of `.`/`。`/`・`" as a real-text signal
  (not just ≥0x80 kana), so it stops before swallowing runs like this
  one. Not attempted yet.
- **`[SetSpeaker?]`, `[SetInitialTypeSpeed?]`, `[SetTypeSound?]`,
  `[ClearIconList?]`, `[ConditionalTrigger?]`, `[TabToColumn]`
  (outside its one confirmed credits-screen use)** -- decompiled and
  behaviorally understood, but none confirmed with an isolated in-game
  test pinning down the exact user-visible effect.
- **The 12 entries needing a `[NewLine]` fix** (see 2.6) -- identified,
  not yet applied to translations.
- **Indices 22 (`0x16`) and up** -- **confirmed** (see 8.9):
  `LAB_140056260` has exactly one comparison, `if (0x15 < uVar4)`, and
  no second check anywhere in the function; `0x16+` are always drawn as
  glyphs. The special value `0x100` (auxiliary-table jump) is handled
  inside that same `> 0x15` branch.
- **`find_jp_text_sources.py`** itself was never touched this session --
  it's the source of entries 51/58's false-positive noise pairs (see
  2.9), but fixing that requires editing the Ghidra pair-finding script,
  not `build_japanese_strings_json.py`.
- The `32`-word `TotalCharCount` safety margin -- works, but it's a
  generous estimate, not the exact minimum required value.
- Formally mapping which English entries are the "disabled" ones --
  scenes that lost their Japanese translation (reuse generic placeholder
  text like "YOU GET") -- to know where to start recovering that content.
- **English side, still open** (see 8.10): who writes `[+0x38]`; the
  "····BASS····?" decorative dots; why indices 51, 58, 162 and 169 have
  no valid field 2; what English field 1 really is; per-opcode in-game
  confirmation of the 1-byte parameter form of every English tag.

None of these block normal use of the mod -- they're technical curiosities
or content tasks, not pending engine bugs.

---

## 7. Current status

The text engine (English and Japanese) is **working with no known
engine bugs** -- extraction, patching, and round-trip are confirmed for
both languages. Real bugs found and fixed this round: the English
`LinePointers` missing terminator (2.4), the missing straight-quote
mapping (2.5), the `header_bytes` boundary-alignment bug affecting 106
Japanese entries (2.7), two Japanese entries wrongly extracted as empty
due to a too-small search window (2.8), a missing auxiliary-table
address for entry 25 (2.10), 7 entries losing a dakuten-initial first
character to the header/text boundary (2.11), and a Python/C++ tag-name
mismatch on `[ConditionalTrigger?]` that silently corrupted its bytes
whenever it appeared in `text` -- confirmed live in-game, fixed the
missing music-resume symptom (2.12). Two Japanese
entries (51, 58) were confirmed as genuine non-text noise, not a bug
(2.9). The one open item that affects actual translation work is
authoring guidance, not a bug: **12 native Japanese entries use
`[DrawClosingMark]` without an immediate `[NewLine]`**, which will render shifted
once translated unless a `[NewLine]` is added at that point (`--validate`
catches this automatically). Ready for active translation work.

**English side update:** the English patching path now matches the
rebuilt extraction (2.13: new JSON format + restored `LinePointers`
terminator) and the English encoder accepts the shared control-code
tags (8.7). Confirmed working in a quick in-game test. The
array-bleed leftovers in `GameTextUS.json` were cleaned by hand (list
in 8.4); regenerating that file from the extraction script would bring
them back.

---

## 8. English side: shared control codes, index alignment, and the array-bleeding gap

A follow-up investigation, done after the Japanese side above was
considered complete. Short version: **English shares almost the entire
mechanism with Japanese** -- far more than originally assumed -- but
its own data layout has a real structural weakness that Japanese
doesn't.

### 8.1 — English uses the exact same control-code dispatcher (CONFIRMED)

`FUN_140057a10` (dialogue box creation, already documented in section
3.10) sets the initial phase to `LAB_140056260` **unconditionally**,
regardless of language. Confirmed live, twice independently:

- **Cheat Engine, "find out what accesses this address"** on the RVA
  holding "BASS"'s text pointer showed a `call MMLC2.exe+56CB0` --
  `FUN_140056cb0`, the exact same glyph-drawing function already
  decompiled for Japanese in section 3.10.
- Decompiling `LAB_140056260` itself shows it explicitly branches on
  which pointer is set (`[+0x18]`, the Japanese word array, vs.
  `[+0x28]`, a plain `char*`) and reads **one byte at a time** from the
  `char*` path (`uVar4 = (ushort)*pcVar3`), applying the identical
  `> 0x15 == control code` rule either way.

**Practical result**: English strings can (and, per the per-entry
`ctx` field -- see 8.2 -- already do) contain the same control codes
Japanese uses (`[SetTypeSpeed]`, `[NewLine]`, `[WaitForConfirm]`,
etc.), just with 1-byte parameters instead of Japanese's 2-byte words
(unconfirmed for every opcode individually, but consistent with every
example decoded so far).
The English encoder now emits these tags (see 8.7); the game accepted
them in a quick in-game test, but each opcode's isolated effect on the
English side has not been individually recorded.

### 8.2 — English's `StringEntry` is a 3-field, 0x18-byte struct, mirroring Japanese

Decompiling `UndefinedFunction_140002bc0` (English's counterpart to
`FUN_1400017b0`, the function that fills the whole runtime table at
startup) shows each of the 171 blocks has three 8-byte fields, exactly
like Japanese's `{word_array_ptr, len, aux_table_ptr}` triple (3.8):

- **Field 0 = `ctx`**: a short control-code-only string (decoded via
  the same shared mechanism as 8.1), e.g.
  `[SetTypeSpeed][00][SetTypeSound?][00][SetInitialTypeSpeed?][00]`.
  Confirmed live via raw bytes for two different entries that **every
  parameter is literally 0x00** -- English apparently never customizes
  typing speed/sound/initial-speed per entry, unlike Japanese (which
  uses real, varied values like 512 or specific margins). Not a
  parsing bug; the underlying `.rdata` bytes are genuinely `02 00 00
  00` etc.
- **Field 1**: a small integer, long assumed to be a raw byte-count/
  size field. A hypothesis that it's actually a **line count** (tested
  because entry 0's field 1, `0x18`=24, exactly equals its real line
  count times 8) did not hold up against a second entry (entry 3's
  field 1 is `0x46`=70, not a clean multiple of 8) -- likely
  coincidence for entry 0, not a working formula. Not resolved.
- **Field 2**: the address of a pointer variable (one more indirection
  needed) that resolves to the **start of an array of `char*` line
  pointers** -- i.e. the actual visible dialogue text, one pointer per
  line, read sequentially until an invalid pointer or implausible
  content is hit. This is `read_entry_text()` in `decode_en_text.py`.

Getting to this model took three wrong guesses first (treating field 0
as the real text; treating field 2 as a single already-resolved
string; treating field 2's resolved address as a further single
string rather than itself being an array start) -- worth remembering
if this code is revisited.

### 8.3 — Entry indices align 1:1 between English and Japanese (CONFIRMED, overturns an earlier claim)

Once `decode_en_text.py` was reading entries in the TRUE table order
(index 0..170, derived from `UndefinedFunction_140002bc0`'s own fill
order -- see `known_array_starts_ordered.h`/`known_ctx_starts_ordered.h`,
generated by grouping that function's assignments by destination
address block, 0x18 bytes each), comparing English and Japanese index
by index showed strong, consistent thematic alignment: index 0 is
credits ("AND CAPCOM"/"ALL STAFF") on **both** sides; index 3/4 is the
Bass/Forte encounter on both; index 8 is "it's been a while" on both;
etc., for essentially the whole early range checked (0-14).

This makes complete sense given section 4.0's finding (`FUN_1400561a0`
selects `table[language] + index*0x18` using the **same** per-box index
byte, `[+0x26]`, regardless of language) -- of course the same index
has to mean the same dialogue moment in both tables, since it's
literally the same index value driving both lookups.

**This overturns an earlier, wrong conclusion from earlier in this
session**, which found no correspondence between "English index 25"
and "Japanese index 25" and concluded the two languages' numbering was
fully independent. That earlier test used the OLD, pre-existing
`known_array_starts.h` (built in a still-earlier session via a
different, more heuristic method) with no guarantee of matching the
TRUE table order -- the mismatch was very likely due to that file
being wrongly ordered, not because the indices are actually unrelated.
**Anywhere in this document that states or implies English and Japanese
indices are unrelated is known to be incorrect** and should be read
with this correction in mind. `TEXT_FINDING_GUIDE.md` has already been
corrected (its section 1).

### 8.4 — Some English entries "bleed" into the next entry's lines (RESOLVED; the affected JSON keys were also cleaned by hand -- see the update at the end of this section)

**Symptom**: several entries' extracted `text` runs on past where the
real dialogue ends, appending lines that belong to a completely
different, unrelated entry. Confirmed concretely for entry 3 (Bass's
introduction): its real 9 lines ("I'M BASS AND HE'S" ... "HELP...")
are immediately followed, with zero gap, by 3 more lines ("ROCK MAN
7", "PRODUCER", "PROFESSOR F") that belong to the credits section
(entry 68 and friends). Confirmed in Ghidra: `PTR_s_ROCK_MAN_7_140572848`
sits at EXACTLY `array_rva(entry 3) + 9*8` -- the compiler/linker
simply placed the two pointer arrays back-to-back in `.rdata` with no
gap or terminator between them.

**Root cause, as far as this investigation got**: unlike Japanese
(where every entry's word stream ends with a genuine `(0x00, 0x00)`
terminator baked into the data itself -- see `decode_word_array`'s
handling of that case), English's array-of-line-pointers model has
**no terminator of its own**. `read_entry_text()`'s current stopping
condition (`looks_like_garbage()`, an invalid/out-of-range pointer) is
a heuristic operating on content plausibility, not a real end-of-array
marker -- and it necessarily fails whenever the *next* array in memory
happens to also contain plausible-looking text, which credits/name
lists (real English words, real pointers) do.

**What was ruled out**: field 1 as a line-count limit doesn't hold up
(8.2). `plVar2[1]` (mirroring field 1, per pointer arithmetic on the
resolved struct) IS read and compared against the running read-index
inside `LAB_140056260`, which looked promising -- but this is almost
certainly the same `TotalCharCount`-style safety-margin mechanism
already documented in section 2.3 (`CTX_HEADER_SAFETY_MARGIN`), which
bounds how many words of the `ctx` **header** can be read before
drawing starts -- not a limit on the real line array's length. Not
re-derived independently for the English side; assumed to carry over
by analogy since the underlying dispatcher is shared (8.1).

**Not yet found**: whatever the REAL game uses at runtime to know
"stop after N lines" for a given entry (if anything single and
per-entry actually exists -- it's possible the real game simply never
triggers this failure mode because normal gameplay flow always closes
the dialogue box, via player input or a fixed frame count, before
reading runs far enough to reach the next array; the bleed might be
purely an artifact of this offline extraction script reading further
than the game itself ever does at runtime).

**Practical impact**: cosmetic/informational only for extraction
purposes (translators reading the JSON would notice and simply not
translate the extra bled-in lines) -- doesn't affect the game itself,
since this bleeding is a property of the *offline extraction script*
choosing to keep reading, not something that happens during actual
gameplay.

**Update — resolved in `decode_en_text.py`, two different causes:**

a) *Bleed into another known English entry (35+ cases).* Fixed with a
   two-pass read: first compute each entry's natural extent, then re-read
   everything, stopping as soon as a read is about to enter the
   territory (whole extent, not just the exact start) of any other
   known English entry.

b) *Bleed into the auxiliary table of the JAPANESE side (entries 3, 39,
   90, 92, 98-106, 131, 144-151, 160).* Confirmed via a Ghidra XREF
   that the destination of the leak is built inside `FUN_1400017b0`
   (the JAPANESE table builder). A first attempt used
   `build_japanese_strings_json.AUX_TABLE_BY_ENTRY_INDEX`
   (~50 entries, curated earlier) and **failed silently**: that dict
   was incomplete and lacked exactly the needed address (`0x576db0`,
   the "PLANNER" credits array). Fixed by extracting the COMPLETE set of
   field-2 values of all 171 blocks of `FUN_1400017b0` from a full
   decompile (kept as `JP_TABLE_AUX_ADDRS` inside `decode_en_text.py`).
   Extra detail: 2 of those 42 addresses coincide exactly with the
   start of legitimate English entries (72 "SPECIAL", 73 "PROGRAMMER" --
   literally the same address used by both systems, unlike "PLANNER",
   which is a separate physical copy). Handled by passing the set of
   known English starts as `protect`, so those slots are skipped.

**Status of the delivered JSON, and the manual cleanup.** The
`GameTextUS.json` in `MM7Loc.zip` reviewed on 2026-09-20 (dated
2026-09-19 18:53) still showed the bleed despite the extraction-script
fixes above -- e.g. entry 3 ending in "ROCK MAN 7 / PRODUCER /
PROFESSOR F" (12 lines instead of 9), entries 98/99/106 (array
`0x576d98`) running 9 lines into "PLANNER / HISAYOSHI...", and credits
text after the dialogue in 39, 90, 131, 144 and 151.

The project owner then **cleaned the affected keys directly in
`GameTextUS.json`**. The keys corrected by hand are:

- 3
- 39
- 90
- 98 through 106
- 113 through 122
- 131
- 144 through 151
- 157
- 160

Notes on that list:

- **113-122 and 157 were not in the earlier list of known leaks** (the
  one in this section and in the handoff). They only turned up during
  the manual pass, so the script-side analysis above did not cover
  them.
- **Entry 92 was in the earlier list but not in the manual one.** In the
  reviewed JSON it had only 3 lines (ending at "DR."), so it may never
  have leaked in the delivered file; its status is unverified.
- The cleaned JSON itself has not been re-reviewed for this document,
  so any leak outside the list above is not ruled out.

**Regenerating the JSON overwrites the manual fix.** The cleanup lives
in the JSON, not in `decode_en_text.py` (which is not part of the
repository archive). If the file is regenerated, re-check every key in
the list above, or fix the script for the missing cases (113-122, 157)
first.

Because the patcher (2.13) writes every line of an entry into the
game's array, leftover lines are harmless to the game's reading but
pointless to translate -- one more reason to keep the JSON clean.

### 8.5 — The "····BASS····?" decorative dots are not stored as text (OPEN, likely a rendering-level effect)

In-game, Mega Man's reaction to first seeing Bass renders as
"····BASS····?" (four dots, the name, four more dots, a question
mark). The underlying `char*` string, confirmed via raw bytes read
directly (bypassing all decoding), is **exactly** `B A S S \0` -- 4
bytes, nothing more. Checked and ruled out:

- Neighboring array slots (±16 bytes around the pointer holder): none
  resolve to anything meaningful, all garbage/unrelated data.
- The entry's own `ctx` header (field 0): just the same boilerplate
  zero-parameter setup codes seen everywhere else (8.2), nothing
  dot-related.
- Font/character-table gaps: both `.` (0x2E) and `・` (0xC6, the
  Japanese nakaguro) are already mapped in `BYTE_TO_CHAR` on both the
  Japanese and English sides -- not a missing-glyph issue.

**Conclusion**: the dots and question mark are very likely added by
rendering/game logic specific to this dialogue box or moment, not
present in any text data this investigation has found. The Japanese
equivalent (`・・・・フォルテ・・・・？`) stores its dots as literal
characters in the string itself -- the two language versions may reach
the same visual effect through genuinely different mechanisms.
**Not resolved**: attempts to catch this live (x64dbg memory
breakpoint on the exact 4-byte "BASS" address, both narrowed to that
address and via Cheat Engine's "find access") either caught unrelated
noise (a `timeGetTime` call) or crashed the game. Finding the actual
trigger would need an execution breakpoint at the exact moment this
specific dialogue box opens, not a memory breakpoint -- not attempted.
**Practical impact for translation**: a translated line for this entry
would need the decorative dots added manually as literal text (matching
Japanese's own approach), since there's no code-level behavior to
inherit automatically.

### 8.6 — Tooling from this section

- **`decode_en_text.py`** -- rewritten several times this session; the
  current version reads `known_array_starts_ordered.h` (field 2 per
  entry, the real multi-line text) and, optionally,
  `--ctx-starts known_ctx_starts_ordered.h` (field 0, the `ctx`
  header) to add a `header_decoded` key matching `GameTextJP.json`'s
  shape. Output format: `{"0": {"array_rva": "0x...", "text":
  "LINE1[NewLine]LINE2..."}, ...}`, one key per index, in true table
  order.
- **`known_array_starts_ordered.h`** / **`known_ctx_starts_ordered.h`**
  -- generated by parsing a full decompile of
  `UndefinedFunction_140002bc0` (paste the whole function body into a
  text file, group its assignments by destination-address block //
  0x18, take field 2 or field 0 of each block respectively, convert
  VA -> RVA by subtracting `0x140000000`). Always exactly 171 entries,
  **not deduplicated** and **not skipping entries with no valid
  pointer** (written as a `0x0u` placeholder instead) -- both are
  required to keep positional alignment between the two files, since
  `main()` zips them by index via `enumerate()`. The OLD
  `known_array_starts.h` (a different, earlier-session file, still
  present in the repo) should be considered superseded by these two
  for any English-related work.
- **`EnglishText.cpp`** / **`EnglishTextEncoding.cpp`** (DLL side) --
  updated in this round, see 2.13 and 8.7.

### 8.7 — English control-code tags in the encoder (DONE)

`EnglishTextEncoding.cpp` (`EncodeLineToGameBytes`) now recognizes the
same tags `JapaneseText.cpp` uses, but with the English constraint that
a command parameter is **one byte**:

- No parameter: `[WaitForConfirm]` (06), `[ForceWait]` (0F),
  `[ClearForceWait]` (0E), `[ResetToMarginSameLine]` (15),
  `[IncrementCounter]` (0B), `[ClearIconList?]` (13),
  `[AdvanceCursorLine]` (08, lone).
- One-byte parameter, written `[Tag][XX]` with exactly two hex digits:
  `[SetSpeaker?]` (01), `[SetTypeSpeed]` (02),
  `[SetInitialTypeSpeed?]` (03), `[RepeatBlankTile]` (04),
  `[PacingTick]` (05), `[WaitFrames]` (07), `[TabToColumn]` (09),
  `[SetLeftMargin]` (0A), `[SetTypeSound?]` (10), `[PlaySound]` (11),
  `[ConditionalTrigger?]` (12).

Rules and deliberate omissions:

- `[NewLine]` is **not** an encoder tag on the English side: each line is
  its own `char*`, so `EnglishText.cpp` splits on it before encoding.
- **A `00` parameter is dropped with a warning**, because a 0x00 byte
  inside a `char*` line would read as the end of the string. So
  `[SetTypeSpeed][00]` cannot be written in English dialogue text
  (the `ctx` header uses 0x00 parameters, but it is read by count, not
  by NUL -- see 8.2). If that value is ever needed, first confirm how
  the game treats a 0x00 mid-line.
- A tag missing its `[XX]` parameter is ignored with a warning; the
  rest of the line is kept as ordinary text.
- Unknown `[...]` text is left alone (`[` is a normal glyph, byte 0x5B).
  No raw `[XX]` byte-escape exists on the English side (unlike
  Japanese), to avoid clashing with literal bracketed text.
- `[DrawClosingMark]` is not included: `0xCA` is Japanese's full-width
  "。" glyph and has no meaning in the English font.
- Parameters wider than one byte (e.g. Japanese `[TabToColumn][0550]`)
  can't be expressed in English.
- **Sync rule:** tag names and opcodes must match `JapaneseText.cpp`
  literally (see the 2.12 lesson -- diff the two lists whenever either
  changes).

**Status**: the change works in a quick in-game test. Not yet recorded:
the visible effect of each individual tag on the English side.
Suggested checks: a `[WaitForConfirm]` in the middle of a short line;
`[SetTypeSpeed][04]` at the start of a line.

### 8.8 — Audio functions decoded (`FUN_1400fba90`, `FUN_1400fb840`)

- **`FUN_1400fba90`** -- the real sound-effect manager: remaps "alias"
  sound IDs, has a special case (`0x5e` = 94) that stops 8 sounds at
  once (batch cleanup), and uses a bit mask to decide whether a sound
  actually plays or just stops another. Confirms the meaning of
  `[SetTypeSound?]` and `[PlaySound]`.
- **`FUN_1400fb840`** -- called by `[ConditionalTrigger?]`; confirmed to
  **play/resume music**. Its play call uses `iVar1 << 0x12` (an 18-bit
  shift, a classic way to encode "music channel" vs "effect") and a
  fixed `loop = 1` (versus `loop = 0` in the effects function).

### 8.9 — Dispatcher limit and `[TabToColumn]` formula

- **`0x15` is definitive.** Decompiling `LAB_140056260` shows a single
  literal comparison `if (0x15 < uVar4) { ... }` and no other check in
  the function. Values `0x16+` are glyphs; the special `0x100`
  (auxiliary-table jump) is handled inside that same branch.
- **`[TabToColumn]` (opcode `0x09`)**: handler `FUN_140056990`, reached
  through the function-pointer table `PTR_LAB_1404531a0` (not a source
  `switch`). Formula:

  ```c
  *(ushort *)(param_1 + 0x30) =
       (*(ushort *)(param_1 + 0x38) & 0xfff8) * 4
       + *(short *)(param_1 + 0x32) + sVar4;
  ```

  `[+0x30]` = current cursor X (written), `[+0x32]` = left margin (same
  field `[SetLeftMargin]` writes), `sVar4` = the command's own
  parameter, `[+0x38] & 0xFFF8` = a packed counter whose low 3 bits are
  something else, times 4. The handler only **reads** `[+0x38]`; the
  writer is **not found** (see 8.10).

### 8.10 — Still open on the English side

In suggested priority order:

1. Find the writer of `[+0x38]` -- write breakpoint in x64dbg
   (`bpm <object+0x38>, w`); static analysis didn't link the
   references.
2. The "····BASS····?" dots (8.5) -- execution breakpoint at the moment
   that specific box opens.
3. Why indices **51, 58, 162 and 169** have no valid field 2
   (`0x0u` placeholders in `known_array_starts_ordered.h`). For 51 and
   58 the Japanese side already concluded they are non-text noise
   (2.9); the English side hasn't been investigated.
4. English field 1 (8.2) -- what it really is.
5. Per-tag in-game confirmation of the 1-byte parameter forms (8.7).

---

## 9. Address and constant reference

Confirmed addresses, in one place. RVAs are relative to the module base; a
virtual address is `RVA + 0x140000000` (the Ghidra names carry the VA).

### Tables and globals

| Name | RVA | What it is |
|---|---|---|
| `RVA_INIT_STRINGS` | `0x2BC0` | `UndefinedFunction_140002bc0`: fills the English `StringEntry[]` at startup (one-shot). Hooked in `dllmain.cpp`, followed by the English patch. |
| `RVA_TABLE_START` | `0xE27B50` | English `StringEntry` table: **171** entries, stride `0x18`, fields `{ctx, TotalCharCount, LinePointers}`. |
| Japanese table | `0xE26B20` | Japanese table: **171** entries, stride `0x18`. `dest_rva` in `GameTextJP.json` = `0xE26B20 + index * 0x18`. |
| `ENTRY_STRIDE` | `0x18` | Size of one table entry (both languages). |
| `RVA_JP_MASTER_INIT` | `0x17B0` | `UndefinedFunction_1400017b0`: fills the Japanese table (body `0x17b0`-`0x2bba`). Not a proper Ghidra function. |
| `PTR_DAT_14045fca8` | `0x45FCA8` | 2-pointer array of table bases: slot 0 = Japanese, slot 1 = English (see 4.0). |
| `DAT_140942e50` | `0x942E50` | Pointer whose target `+ 0x400` holds the language flag (two dereferences, see 4.0). |
| `PTR_LAB_1404531a0` | `0x4531A0` | Jump table of the `0x00`-`0x15` control-code handlers, one function pointer per index. |
| `RVA_TABLE_A` / `RVA_TABLE_B` | `0x56D110` / `0x56D020` | The two sibling "system label" tables patched by `MiscText.cpp` (naming provisional, see `MiscText.h`). |

### Dialogue functions

| Function | RVA | What it does |
|---|---|---|
| `FUN_140056360` | `0x56360` | Character reader. Branches between the English `[RCX+0x28]` `char*` path and the Japanese `[RCX+0x18]`/`[RCX+0x20]` word-array path. Its stopping at `[DrawClosingMark]` is normal behavior. |
| `FUN_1400561a0` | `0x561A0` | The dialogue-advance state machine. Calls the current phase function (`[param_1+8]`) in a loop; all reads of `FUN_140056360` come from `0x561fe` (+`0x5E`). Resolves `[param_1+0x18]` from the table base and the entry index byte `[param_1+0x26]` (see 4.0). |
| `LAB_140056260` | `0x56260` | The shared word/byte dispatcher: the initial phase of every dialogue box, in both languages. `> 0x15` = glyph, otherwise control code. |
| `FUN_140057a10` | `0x57A10` | Creates the dialogue box object, **once per entry** (see 3.10). |
| `FUN_140056520` / `FUN_140056720` | `0x56520` / `0x56720` | The phase waiting on a jump-table condition, and the per-character reveal-pacing timer it calls. |
| `FUN_140056970` | `0x56970` | `[NewLine]` / `[AdvanceCursorLine]` handler (index 8). |
| `FUN_140056990` | `0x56990` | `[TabToColumn]` handler (index 9). |
| `FUN_140056c70` | `0x56C70` | `[ClearIconList?]` handler (indices `0x13` and `0x14`). |
| `FUN_140056cb0` | `0x56CB0` | Glyph-drawing function, shared by both languages. |
| `FUN_1400fba90` | `0xFBA90` | Sound-effect manager (`[SetTypeSound?]`, `[PlaySound]`; see 8.8). |
| `FUN_1400fb840` | `0xFB840` | Plays/resumes music (`[ConditionalTrigger?]`; see 8.8). |

### Other systems

| Function | RVA | What it does |
|---|---|---|
| `FUN_14005dd80` | `0x5DD80` | Draws a text object character by character. The hook point of `BossNameText.cpp` (and of `GlyphDrawDebug.cpp`). Called from `FUN_14005dc60` (`0x5DC60`), itself called by the generic draw loop `FUN_14005c070` (`0x5C070`). |
| `FUN_140060620` | `0x60620` | Generic resource lookup by name (hooked by `ResourceLookupDebug.cpp`). |
| `FUN_1401003c0` / `FUN_140100610` | `0x1003C0` / `0x100610` | The "system label" text system patched by `MiscText.cpp`. |
