# MM7Loc — Control Codes (Japanese and English)

Quick reference for every control code decoded so far. These are the `[Tag]`
labels used in the `text` field of `GameTextJP.json` (and, with one-byte
parameters, in `GameTextUS.json` — see "English" below). Complements
`MM7Loc_INVESTIGATION_SUMMARY.md`, which has the investigation behind each one.

## How the dispatcher works

Dialogue is read by one shared function (`LAB_140056260`) for both languages.
Japanese entries are arrays of 16-bit **words**; English entries are plain
`char*` strings read **one byte at a time**. The rule is the same for both:

- A value `≤ 0x15` is a **control code**, dispatched through the jump table
  `PTR_LAB_1404531a0` (one handler per index).
- A value `> 0x15` is drawn as a **glyph**. This is a single comparison
  (`if (0x15 < uVar4)`) with no second check anywhere in the function, so
  there is nothing special above `0x15` in the dispatcher.
- The one special glyph value is `0x0100` (Japanese), handled inside that same
  branch: it jumps to the entry's auxiliary pointer table, which is how loose
  Latin text (proper names, acronyms) is embedded in Japanese lines.

## Full table

The **EN** column says whether the code can be used in English text (see
"English" below). Parameters are 1 byte in English; in Japanese they are a
16-bit word, written `[XX]` when the value fits in one byte and `[XXXX]` when
it doesn't.

| Code | Readable tag | Parameter | What it does | Confidence | EN |
|---|---|---|---|---|---|
| `0x01` | `[SetSpeaker?][XX]` | 1 byte | Writes to `[+0x3c]`, passed as the 3rd argument to the function that resolves which balloon/box to draw. **Hypothesis**: selects which character the balloon points at. | Well-supported hypothesis | ✓ |
| `0x02` | `[SetTypeSpeed][XX]` | 1 byte | Sets the **persistent** typing pace (`[+0x3f]`/`[+0x40]`) — stays in effect until the next `[SetTypeSpeed]`. | ✅ Confirmed (exact mathematical proof) | ✓ |
| `0x03` | `[SetInitialTypeSpeed?][XX]` | 1 byte | Writes to `[+0x34]` (a "typing pace" field known earlier, with no writer found until now). **Hypothesis**: a base/initial speed for the entry, distinct from `[SetTypeSpeed]`. Never appears in any of the 171 current entries. | Hypothesis, no observed real-world use | ✓ |
| `0x04` | `[RepeatBlankTile][XX or XXXX]` | 1-2 bytes | Draws tile 0 (likely blank/space) N times, advancing the cursor each time — "print N blank tiles". **Hypothesis**: a spacing/indent command. | Behavior confirmed, purpose is hypothesis | ✓ |
| `0x05` | `[PacingTick][XX]` | 1 byte | Same function as the normal pacing timer (`FUN_140056720`), called from the post-`[DrawClosingMark]` footer. Only **reads** the current pace, doesn't write it — might be a side effect of dispatch reuse rather than a "real" command. | Behavior confirmed, purpose in the footer uncertain | ✓ |
| `0x06` | `[WaitForConfirm]` | none | The blinking "▼ press to continue" prompt — draws tile `0xFE`, blinks every 32 frames, and suspends execution until the player presses confirm. | ✅ Confirmed | ✓ |
| `0x07` | `[WaitFrames][XX]` | 1 byte | A **one-off** pause of XX frames, then back to normal pace. | ✅ Confirmed | ✓ |
| `0x08` (pair `08 08`) | `[NewLine]` | none | Visible line break. Copies the anchor saved in `[+0x32]` (see `[SetLeftMargin]`) back into the cursor `[+0x30]` **and** adds `0x20` to the combined value — "return to left margin + move down 1 line" in a single instruction. | ✅ Confirmed (with mathematical precision) | see note |
| `0x08` (lone) | `[AdvanceCursorLine]` | none | The **exact same handler** as `[NewLine]` (same dispatch slot), called once instead of twice, so it has half the effect. Only seen in the post-`[DrawClosingMark]` footer. | ✅ Same handler as `[NewLine]`; purpose in the footer unconfirmed | ✓ |
| `0x09` | `[TabToColumn][XX or XXXX]` | 1-2 bytes | An **absolute** cursor-X jump: `[+0x30] = ([+0x38] & 0xFFF8)×4 + [+0x32] + parameter`. Handler `FUN_140056990`. Uses the margin anchor **and** an undocumented field (`[+0x38]`) that this handler only reads. **Only confirmed use**: the credits screen (entry 68, value `0x0550`), positioning "PRODUCER"/"PROFESSOR F". It does **not** appear in the mid-dialogue footer (an early misread, caused by `0x04` not being decoded yet). | Behavior confirmed, purpose is a well-supported hypothesis | ✓ (1 byte only) |
| `0x0A` | `[SetLeftMargin][XX]` (renamed from `ResetCursorX`) | 1 byte | Writes the same value into both `[+0x30]` (current cursor) **and** `[+0x32]` (saved anchor). Typically runs once, in the header, before the real text — it does **not** repeat at every `[DrawClosingMark]`; it is `[NewLine]` that re-applies the anchor afterward. Some entries need a value above 255 (390, 643). | ✅ Confirmed | ✓ (1 byte only) |
| `0x0B` | `[IncrementCounter]` | none | Increments a **global** counter (`DAT_1408fd954`), unrelated to this entry's own state. Doesn't affect this box's layout or timing. | ✅ Confirmed | ✓ |
| `0x0C` | *(combines with the next character)* | — | Dakuten — te→de, ka→ga, etc. The handler (`FUN_140056a90`) pushes the cursor up one line-step, draws a small fixed tile (`0xC9`), then pops the cursor back; the next word draws normally underneath. The decoder shows these as precomposed characters (が, ぱ), so no tag is needed. | ✅ Confirmed | — |
| `0x0D` | *(combines with the next character)* | — | Han-dakuten — ha→pa, etc. Same mechanism (`FUN_140056ad0`, tile `0xC7`). | ✅ Confirmed | — |
| `0x0E` | `[ClearForceWait]` | none | Clears `[+0x3e]`, the exact field `[ForceWait]` sets to 1 — the "undo" of `[ForceWait]`. | ✅ Confirmed (decompiled) | ✓ |
| `0x0F` | `[ForceWait]` | none | Sets `[+0x3e]` to 1, which blocks skipping a `[WaitFrames]` pause with the confirm button (only that — it doesn't prevent skipping some other way). | ✅ Confirmed | ✓ |
| `0x10` | `[SetTypeSound?][XX]` | 1 byte (only the low byte is used) | Writes to `[+0x41]`, read after every glyph drawn (`FUN_140056360`) to call `FUN_1400fba90(value, 0)` when nonzero. That function is decompiled and is a real sound-effect manager (remaps alias IDs, has a special case `0x5e` that stops 8 sounds at once, uses a bit mask to decide whether a sound plays or only stops another). **Very strong hypothesis**: a per-character "typewriter blip". | Read mechanism and sound function confirmed; still needs an in-game test | ✓ |
| `0x11` | `[PlaySound][XX or XXXX]` | 1-2 bytes | Reads a parameter and calls `FUN_1400fba90(value, 0)` directly — the same function `[SetTypeSound?]` uses, but plays the sound **once, immediately** instead of setting up a per-character one. | ✅ Read mechanism confirmed | ✓ |
| `0x12` | `[ConditionalTrigger?][XX or XXXX]` | 1-2 bytes | Reads a parameter; if it is **greater than 16**, calls `FUN_1400fb840()`, which **plays/resumes music** (decompiled: its play call uses `iVar1 << 0x12`, an 18-bit shift that encodes music channel vs. effect, with `loop = 1`). Live evidence: entry 28's `[ConditionalTrigger?][14]` (20 > 16) is exactly where the intro cutscene's music resumes after being stopped at the end of entry 24. | ✅ Function decompiled; behavior seen live | ✓ |
| `0x13` / `0x14` | `[ClearIconList?]` | none | Two raw values confirmed as **synonyms** (both point at `FUN_140056c70`). Walks the linked list of icon/sprite items attached to the dialogue box and destroys or hides each one, then empties the list. Only seen as the first word of weapon-description entries. **Hypothesis**: clears the previous weapon's icon before the new one shows. | Behavior decompiled; purpose is hypothesis | ✓ |
| `0x15` | `[ResetToMarginSameLine]` | none | Clears `[+0x38]` (the field `[TabToColumn]` reads) and copies the margin anchor `[+0x32]` back into the cursor `[+0x30]` — half of what `[NewLine]` does (return to the margin) without moving down a line. | ✅ Confirmed (decompiled) | ✓ |
| `0xC2` / `0xC3` | *(mapped in the character table)* | — | Full-width `？`/`！` (Japanese), distinct from the narrow `?`/`!` used by English. | ✅ Confirmed | — |
| `0x0100` | *(special mechanism, no tag)* | 1 word (index) | Jumps to the entry's auxiliary pointer table, resolving embedded loose ASCII text (proper names, acronyms like "UFO"). Resolved automatically during extraction. | ✅ Confirmed | — |
| `0xCA` | `[DrawClosingMark]` (was `[NEWBOX]`) | none | Draws the closing "。". Renamed because it **closes nothing**: `0xCA` is above `0x15`, so it is an ordinary glyph, and the dialogue box object is created **once per entry** (`FUN_140057a10`), never per `[DrawClosingMark]`. The "new page" a player sees is `[DrawClosingMark]` (draws "。") + `[WaitForConfirm]` (waits) + `[NewLine]` (moves the cursor back to the margin). | ✅ Confirmed | — |

## English

English dialogue goes through the **same dispatcher** (confirmed live: the
initial phase of every dialogue box is `LAB_140056260`, and a watch on an
English entry landed in the same glyph-drawing function as Japanese). What
differs is only the data shape:

- Text is a plain `char*`, so a command's **parameter is one byte**. Tags are
  written `[Tag][XX]` with exactly two hex digits, e.g. `[SetTypeSpeed][04]`.
  A parameter that doesn't fit in one byte (Japanese `[TabToColumn][0550]`,
  or a margin above 255) cannot be expressed in English.
- Each English entry is a **list of lines**, one `char*` per line. In
  `GameTextUS.json` the lines of an entry are joined with `[NewLine]`, and the
  patcher splits them back. So `[NewLine]` is the **line separator** in the
  English JSON, not an inline control code. Use `[AdvanceCursorLine]` for a
  lone `0x08` inside a line.
- **A `00` parameter can't be written in a text line**: a `0x00` byte would
  read as the end of the string, so the encoder drops the tag with a warning.
  (The per-entry `ctx` header uses `00` parameters everywhere, but it is read
  by count, not up to a NUL.)
- The encoder support lives in `EnglishTextEncoding.cpp`; see section 8.7 of
  the investigation summary for the exact rules.

The 1-byte parameter form is consistent with every English example decoded so
far and passed a quick in-game test, but the individual effect of each tag on
the English side has not been recorded.

## Still open

- **`[+0x38]`** (used in `[TabToColumn]`'s formula, zeroed by
  `[ResetToMarginSameLine]`) — the reader formula is known, but **what writes
  to this field has not been found** (Ghidra doesn't link the references). Next
  step: a write breakpoint on `<dialogue object> + 0x38` in x64dbg.
- **Isolated in-game tests** — none of `[SetSpeaker?]`,
  `[SetInitialTypeSpeed?]`, `[SetTypeSound?]`, `[ClearIconList?]` and
  `[TabToColumn]` (outside the credits screen) is confirmed with a dedicated
  test that pins down its exact visible effect. `[ConditionalTrigger?]` is
  backed by the entry 28 observation and the decompile.
- **English** — per-tag confirmation of the one-byte parameter form.

## The 12 entries with a confirmed margin bug

Entries where `[DrawClosingMark]` is followed by real text (or the standard
footer) with no `[NewLine]`/`[SetLeftMargin]` before it — candidates for the
shifted-box bug (the cursor is not reset, so the next box starts wherever the
previous one left it):

**9, 11, 13, 34 (×4 occurrences), 35, 45, 78, 85, 107**

Run `python build_japanese_strings_json.py --validate GameTextJP.json` to
re-check this at any time (the list can change if the text is edited).
