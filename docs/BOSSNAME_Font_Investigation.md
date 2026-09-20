# `BOSSNAME.bin` Font Investigation — Reference Document

How the boss-name banner, the stage-select "flavor text" line and the intro
cutscene text of *Mega Man 7* (as shipped in *Mega Man Legacy Collection 2*)
are stored and drawn, and how `BossNameText.cpp` patches them. This is a
separate system from the dialogue tables covered in
`MM7Loc_INVESTIGATION_SUMMARY.md`: it uses a different font and a different
encoding, so none of the dialogue tools can see it.

Covers: the initial suspicion, the path through the port's native engine, the
pivot to the original ROM, the letter → tile tables, the current state of the
mod integration, and what is still open.

---

## 1. Context / motivation

The suspicion: some text in the game was not being captured by `MM7Loc`
because it uses a different font than the normal dialogue font, and therefore
a different encoding.

**Confirmed**: there is a `BOSSNAME.bin` resource (extracted as
`BOSSNAME.png` with `obj_out.zip`). It is a tile sheet (not a scenery image)
containing:

- A large alphabet (A-Z), white→blue gradient, used for the boss-name banner
  on the stage select screen (and for the intro cutscene text).
- A small alphabet overlapping the same sheet.
- Katakana/hiragana, suggesting the same sheet is used for both English and
  Japanese builds.

## 2. Path through the native engine (MMLC2.exe) — partially frustrated

An extensive Ghidra + MinHook investigation tried to trace where text drawn
with this font comes from:

- **`FUN_140060620`** — a generic "resource lookup by name" function used all
  over the engine (including `BOSSNAME.bin` and dozens of other resources).
  Hooked successfully; it reveals the resolved buffer pointer, but not which
  letter is which.
- **`FUN_14005c070`** — generic loop that draws a list of on-screen objects
  (6 linked lists, by layer/category).
- **`FUN_14005ba20`** — converts an object descriptor into a resource name
  (reads `descriptor + 0x70`). Confirmed, but no letter identity here.
- **`FUN_14005dc60`** — draw function called by that loop. Descriptor fields
  investigated: `+0x40`/`+0x44` (X/Y position, confirmed), `+0x64` (constant,
  not a letter index), `+0x90`/`+0x91` (always zero on this path), `+0x10`
  (pointer to a sub-object, also constant across letters).

**Resolved.** Every field reachable from the main descriptor really is shared
by all letters (only X changes), because **the loop in `FUN_14005c070` does
not iterate per letter — it iterates per text object** ("FREEZE MAN" as a
whole is ONE node in those lists). Drawing several characters happens
**inside** a deeper function:

- `FUN_14005dc60` has one more line at its end (lost in a cropped screenshot
  at first): when `param_6 = param_7 = 0` it falls through to
  `FUN_14005dd80(param_1, param_2, param_3, param_4, X, Y, 0x100)`.
- **`FUN_14005dd80`** is the real draw function, and it iterates over all the
  characters internally:
  - `param_1 + 0x40` → pointer to the **string object**.
  - `*(strObj + 0x40)` → **character count**.
  - `*(strObj + 0x48)` → pointer to an **array of pointers, one per
    character**.
  - Each array entry (`charEntry`) holds, at offset **`+0x30`**, the **base
    tile index of that letter** — the piece of data the whole search was
    after.
  - The "bottom half = top half + 16" rule found in the ROM shows up here as
    `(uVar7>>1)*0x10` inside the final index calculation.

The final hook reads the whole array at once (`strObj+0x40` = count,
`strObj+0x48` = array, each `array[i]+0x30` = tile), which yields the full tile
list of a text in a single capture.

## 3. The pivot: original ROM via bsnes-plus

Instead of continuing to fight the recompiled executable, the **original**
SNES ROM was run in bsnes-plus (Tile Viewer + Sprite Viewer). Much more
direct: SNES games draw text with sprites/tilemaps that carry an explicit tile
index per character.

### Graphics architecture (original ROM)

- The normal dialogue font is **2bpp**.
- The font used for "IN THE YEAR 20XXAD" and the boss name is **4bpp**, loaded
  into VRAM at base `0xc000` (same base as sprite OAM1) — i.e. it is **drawn
  with sprites**, not BG/tilemap.
- Each "big" letter (boss/intro font) is made of **two stacked 8x8 tiles**
  (16px tall): top half (lower Y) + bottom half (higher Y). **The bottom half
  is always the top half + 16** (the sheet has 16 tiles per row).
- The small font (used in "DON'T SLIP!") uses **one 8x8 tile per letter**, no
  pair — a different (smaller) section of the same sheet.
- Sprite Viewer lists the text with **X decreasing** in normal reading order,
  so reading the table top to bottom gives the string **backwards**.

### Technique

1. Pause the emulator with the target text on screen.
2. Open the Sprite Viewer and find entries at the text's position (ignore
   junk entries at `Y=224`, off-screen).
3. Note each sprite's `Char` field, ordered by X.
4. Compare with the expected text (remembering the reversed order).

## 4. Big font (16px, pair of 8x8 tiles)

### Original ROM (SNES, via bsnes-plus)

Tested with two texts: "IN THE YEAR 20XXAD" (intro) and "FREEZE MAN" (boss
name, stage select).

| Letter | Intro — top | Intro — bottom | Boss — top | Boss — bottom |
|---|---|---|---|---|
| A | 0 | 16 | 256 | 272 |
| E | 4 | 20 | 260 | 276 |
| N | 13 | 29 | 269 | 285 |
| R | 32 | 48 | 288 | 304 |
| D | 3 | 19 | — | — |
| H | 7 | 23 | — | — |
| I | 8 | 24 | — | — |
| T | 34 | 50 | — | — |
| Y | 66 | 82 | — | — |
| 0 (digit) | 103 | 104* | — | — |
| 2 (digit) | 105 | 106* | — | — |
| X | 64 (single 16x16 tile, not an 8x8 pair) | — | — | — |

In the original ROM the boss banner uses `intro_tile + 256` (confirmed with A,
E, N, R: 4 letters, top and bottom, 8 values matching exactly). It is the same
font; there is simply a second copy of it in VRAM.

`*` The "bottom" values for digits 0 and 2 came out very close to "top"
(103→104, 105→106, a difference of 1, not 16). See the digits note in
section 6.

### Ported game (Legacy Collection 2 / MMLC2.exe) — **CONFIRMED LIVE**

Captured from memory at runtime (hook on `FUN_14005dd80`, reading the
character array of the string object). Tested with the intro ("IN THE YEAR
20XXAD") and 4 boss names (Freeze Man, Junk Man, Burst Man, Cloud Man), all
captured on the stage select screen — **all through the same reused string
object** (the game overwrites the content of the same `strObj` on every boss
switch instead of creating a new one, which is why the hook's dedup had to be
by CONTENT, not by address).

| Letter | Top | Bottom | Confirmed by |
|---|---|---|---|
| A | 0 | 16 | intro + Freeze/Junk/Burst/Cloud Man |
| B | 1 | 17 | Burst Man |
| C | 2 | 18 | Cloud Man |
| D | 3 | 19 | intro + Cloud Man (exact match in both) |
| E | 4 | 20 | intro + Freeze Man |
| F | 5 | 21 | Freeze Man |
| H | 7 | 23 | intro |
| I | 8 | 24 | intro |
| J | 9 | 25 | Junk Man |
| K | 10 | 26 | Junk Man |
| L | 11 | 27 | Cloud Man |
| M | 12 | 28 | Freeze/Junk/Burst/Cloud Man |
| N | 13 | 29 | intro + every boss name |
| O | 14 | 30 | Cloud Man |
| R | 32 | 48 | intro + Freeze/Burst Man |
| T | 34 | 50 | intro (but see the anomaly below) |
| U | 35 | 51 | Junk/Burst/Cloud Man |
| Y | 66 | 82 | intro |
| Z | 36 | 52 | Freeze Man |
| 0 (digit) | 103 | 104 | intro |
| 2 (digit) | 105 | 106 | intro |
| X | 64 (single tile, not a pair) | — | intro |

`BossNameText.cpp` also carries `G = 6`, `P = 15` (inferred from the
sequential alphabet) and `S = 33` (from the BURST MAN capture).

**Main result**: in the port there is **no `+256` offset** between intro and
boss name — same table, same resource (A, E, N, R are identical in both
contexts; D is identical between the intro and Cloud Man). The ROM's `+256`
was only a quirk of how SNES VRAM holds two physical copies of the font.

**Alphabetical order**: the top values in increasing order (A=0, B=1, C=2,
D=3, E=4, F=5, H=7, I=8, J=9, K=10, L=11, M=12, N=13, O=14) show that the
first row of the tile sheet is **literally the alphabet in order** (16 letters
= 16 tiles per row; G should be 6 and P 15). R=32, T=34, U=35, Y=66, Z=36 do
not follow the straight sequence (R would be 17 if it continued) — the R-Z
region is organized differently and is not fully mapped.

**Unresolved anomaly — T and S in "BURST MAN"**: decoding "BURST MAN" gave
`T: top=49 bottom=50` and `S: top=33 bottom=34`. The top/bottom difference is
only **1**, not **16** as for every other letter, and T's top does not match
the intro's T (`top=34 bottom=50`: the bottom matches, the top does not). The
adjacent T and S do not follow the simple 2-tiles-each pattern; the data shows
4 tiles with interleaved Y (2 X positions × 2 Y). Given what was found for the
A/W pair in "WATCH" (section 5: a non-standard array *order*, not real
kerning), this is probably the same kind of phenomenon, but it has not been
confirmed with the same rigor (it would need the ROM bytes for that stretch).
It may also be tied to the `+0x38` field of `charEntry` (sub-glyph count, `1`
vs `4`).

### Spacing / per-letter width (big font)

The big font is **not fixed width** — it is proportional. Per-letter offsets
live in the `+0x3c` (X) and `+0x40` (Y) fields of each `charEntry`, confirmed
live (`iVar2 = param_5 + *(int*)(lVar8+0x3c)` in `FUN_14005dd80`).

Widths confirmed (pixels the cursor advances after each letter), cross-checked
between FREEZE MAN, JUNK MAN, CLOUD MAN and BURST MAN:

| Letter | Width |
|---|---|
| A | 9 |
| O | 7 |
| All others tested (D, E, K, L, N, R, U, Z, M) | 8 |
| Space | 7 (inferred: M=8 + space=7 = 15, matches every time "M " appears) |

**8px is assumed as the default** for letters not yet tested individually
(B, C, F, G, H, I, J, P, Q, S, T, V, W, X, Y). It may be 1-2px off until each
one is confirmed, but is enough to generate text of arbitrary length legibly.

## 5. Small font (single 8x8 tile) — the stage-select "flavor text"

Besides the big boss-name banner, the stage select screen has a **second line
of text** (a short phrase such as "DON'T SLIP!") that only appears on the
**Japanese** build of the game — even though the text itself is already in
English. It looks like content the US localization disabled rather than
removed, which is exactly the kind of "lost content" the project wants to
recover.

This phrase uses a small font (8x8 per letter, no top/bottom pair) with a tile
index table that is **different from the big font's, and also different from
the original ROM's** (only the big font kept the same numbers between ROM and
port).

### The 8 phrases (one per boss)

| Boss | Phrase |
|---|---|
| Freeze Man | DON'T SLIP! |
| Junk Man | FORGOTTEN FACTORY |
| Burst Man | BOMB BOMBER BOMBEST |
| Cloud Man | WATCH YOUR STEP |
| Spring Man | BOYOYON PARADISE |
| Slash Man | JURASSIC JUNGLE |
| Shade Man | MYSTERY! THE HORROR |
| Turbo Man | CHAMP OF THE ROADS |

To unlock all 8 bosses at once (without beating the first 4), use the known
password **`8735 2587 4486 8362`** on the main menu's Password screen.

### Letter → tile table (small font) — the table is FIXED

**History of the confusion**: the first live captures in the port seemed to
show different numbers for the same letter across sessions (e.g. D=296 once,
D=40 another time), which led to the assumption for a long time that this font
"renumbers itself every session", unlike the big font. That motivated a whole
"live learning" system in the code.

**Final finding**: a teammate built the complete table by hand, running the
original SNES ROM **multiple times** to confirm it never changes there. They
found **two copies** of the small font in the ROM, exactly **256 tiles
apart** — the very same `+256` relationship already confirmed for the BIG font.
Cross-checking that table against every stable value captured from the port
across very different sessions, **21 of 22 letters matched exactly** with "ROM
table minus 256", with no session-to-session exception.

Conclusion: **the numbering never changed per session**. The "different"
values seen at first were captures taken **during the fade-in effect** (before
the animation settled), which produce temporary garbage. Once captures wait
for stabilization, they always match this fixed table.

| Letter | Tile (ROM) | Tile (port, = ROM − 256) |
|---|---|---|
| A | 293 | 37 |
| B | 294 | 38 |
| C | 295 | 39 |
| D | 296 | 40 |
| E | 297 | 41 |
| F | 298 | 42 |
| G | 299 | 43 |
| H | 300 | 44 |
| I | 301 | 45 |
| J | 302 | 46 |
| L | 304 | 48 |
| M | 310 | 54 |
| N | 311 | 55 |
| O | 312 | 56 |
| P | 313 | 57 |
| R | 315 | 59 |
| S | 316 | 60 |
| T | 317 | 61 |
| U | 318 | 62 |
| W | 352 | 96 |
| ! | 353 | 97 |
| Y | 354 | 98 |
| ' | 355 | 99 |

**Confirmed not to exist in this font**: K, Q, V, X, Z (none of the 8 original
phrases use them, and they are not on the ROM's tile sheet).

**Note on the ROM numbering**: A-J are perfectly sequential (293-302, no
gaps). Then there is a jump: L=304. The same kind of "break" the big font has
after a certain point of the alphabet — the sheet is not a plain continuous
A-Z.

**Small pending item**: the port value for `L` captured at some earlier point
was `53`, not `48` as the new table says — most likely a transcription slip in
an intermediate summary (the other 21 letters matched perfectly). Worth
re-checking; it does not block anything, since the code uses `48`.

### Spacing / per-letter width (small font)

Like the big font, this one is proportional. Confirmed from a stable capture
(after waiting out the fade-in) of "DON'T SLIP!" and "WATCH YOUR STEP":

| Letter | Width |
|---|---|
| ! | 6 |
| P | 6 |
| I | 6 |
| L | 8 |
| ' | 8 |
| N | 8 |
| O | 8 |
| Default for everything not measured (including T) | 8 |
| Space | 7 |

**About `T`**: it was measured at 4px in one capture (T glued to an
apostrophe, in "DON'T"). But every tile is 8x8, so any width under 8 causes a
visible overlap with the next letter, and T has a wide top bar (unlike
`!`/`P`/`I`, which are naturally thin glyphs where a smaller advance leaves
blank space in the cell). Tested live: using 8 (the default) instead of 4 gave
correct spacing in "WATCH OUT!!" (T appears twice). The "4" was probably an
artifact of that specific T+apostrophe pair, not T's real standalone width.

**Space**: inferred as **7px** (same as the big font; S + space = 15, assuming
S ≈ 8).

### Decoding technique (reference)

1. Pause the game (bsnes-plus on the Japanese ROM, or the `DrawChars` hook /
   `GlyphDrawDebug.cpp` on the port) with the phrase visible.
2. In the Sprite Viewer: each letter is a single 8x8 tile (no pair). There are
   two Y rows when the phrase wraps to 2 lines.
3. Sort by X **descending** = reversed normal reading order (the first table
   entry is the LAST letter of the phrase) — **except** in cases like "WATCH"
   (below).
4. **Real fade-in effect**: this phrase has a genuine animation (starts with a
   white palette and "cools" to the normal color in a few frames), and the
   tile values ALSO change during that window, not just the color. Any capture
   must wait for the animation to settle before its data can be trusted.

### About the "scrambled" order in some phrases (history — resolved)

While investigating, the sprite list of "WATCH YOUR STEP" seemed to have an
odd order in its last two entries (W and A), which first looked like kerning
(width depending on the letter pair). A teammate compared it with the raw ROM
bytes (SNES OAM sprite format: X, Y, Tile, Attribute) and confirmed the W→A
spacing is normal: only the ORDER of those two entries in the list was off
(the last two are in direct order, not reversed like the rest). The real order
is "H,C,T,W,A", not "H,C,T,A,W". There is **no known general rule** to predict
this (the original 65816 code can build the sprite order however it likes);
each string has to be checked individually against the ROM.

This no longer matters in practice: the code uses the FIXED table and no
longer learns by reading array order, so the problem cannot affect it. It only
matters to someone trying to *read* a phrase off the screen by array order.

## 6. Intro cutscene text (big font)

The opening cutscene text uses the same big font as the boss banner. It has
three screens:

| Screen | English build |
|---|---|
| 0 | `IN THE YEAR 20XXAD` |
| 1 | `BUT...` |
| 2 | `BEGIN SEARCHING FOR` / `THEIR MASTER...` (native 2-line) |

Glyphs that do NOT follow the "2 stacked tiles, bottom = top + 16" rule:

- **Digits**: bottom = top + 1 (not +16). Only `0` (top 103) and `2` (top 105)
  are confirmed — the only digits in any original big-font text. Other digits
  are treated as unsupported until someone captures a screen that uses them.
  `0` is notably narrow: 4px advance (measured `X.x − 0.x = 4`); `2` matches
  the 8px default.
- **`X`**: a SINGLE 16x16 sprite (tile 64), occupying one array slot at the top
  row's Y only (its sub-glyph count `+0x38` is 1, versus 0 for paired letters
  and digits). Measured advance: 9px for X→X and 13px for X→letter — a
  kerning-pair-style value, with no other neighbor combination measured.
- **Ellipsis `...`**: each dot is its own single 8x8 tile (100), drawn at
  that line's bottom-row Y minus 2 (bottom = 16 → dot Y = 14), spaced 6px
  apart. Inferred from ONE captured example (the "..." after "THEIR
  MASTER"), so the Y rule has not been cross-checked.

**Array-order quirk**: one intro line — the 2-line `BEGIN SEARCHING FOR /
THEIR MASTER...` screen — has its first glyph in a swapped position in the
array (same class of per-case anomaly as the "WATCH" one in section 5). Lines
0 and 1 were captured and confirmed working WITHOUT the swap. It is a per-line
flag in the code (`firstGlyphOrderSwapped`), applied only where there is
direct evidence.

### Japanese build

Screen 0 reuses the English `IN THE YEAR 20XXAD` verbatim. Screens 1 and 2
appear in kana/kanji:

| Screen | Japanese text |
|---|---|
| 1 | `しかし・・・` |
| 2 | `そして、数ヶ月後・・・` |

Their tile numbers were read off gameplay captures plus the raw `BOSSNAME.bin`
texture (4bpp linear, 16 tiles per row, 8x8 each), then corrected against
real captures where they disagreed:

- Most kana turned out to be **single-tile**, not top/bottom paired like Latin
  letters, even though the texture looks paired. Only `し` pairs (top 67 /
  bottom 83).
- Single-tile glyphs: `。` = 100 (the same tile as the ellipsis dot), `、` =
  101, `か` = 68, `そ` = 70, `て` = 72, `数` = 74, `ヶ` = 102, `月` = 76,
  `後` = 78.
- A stabilized capture of screen 2 has exactly **12 tiles**, which matches
  only if そ, て, 月 and 後 are single-tile too: dots (3) + 後 (1) + 月 (1) + ヶ
  (1) + 数 (1) + 、(1) + て (1) + し (2, paired) + そ (1) = 12. That match also
  confirmed `数` = 74 and `ヶ` = 102.
- The comma after て was confirmed by a community member who reads Japanese;
  an earlier guess without it was wrong.
- No width has been measured for any kana: they all use the 8px default.

Screen 1 only needs し and か, both confirmed. The code identifies both screens
by matching the raw tile sequence; they are mapped, but the code notes do not
record an in-game test of a **replacement** on the Japanese build.

## 7. Integration in the mod (`BossNameText.cpp`)

`BossNameText.cpp` is a production module: it hooks the draw function and
substitutes text; it does not go through the `StringEntry` dialogue table.

### How it works

- **Hook**: `FUN_14005dd80` (RVA `0x5dd80`), `Detour_DrawChars` in the code.
- **Data layout** (see section 2):
  - `param_1 + 0x40` → string object (`strObj`).
  - `strObj + 0x40` → character count (int32).
  - `strObj + 0x48` → pointer to the array of pointers (`arrPtr`), one per
    character.
  - Each `charEntry` (0x48 bytes): `+0x30` tile index, `+0x34`
    flip/orientation, `+0x38` sub-glyph count, `+0x3c` X position, `+0x40` Y
    position.
- **Identification** — which original string is being drawn:
  - *Big font (boss names)*: decode the array with the static table; if it
    matches one of the 8 names, that boss index is remembered
    (`g_lastKnownBossIndex`).
  - *Small font (flavor text)*: the remembered boss index, cross-checked with
    the phrase's character count, gives which of the 8 phrases is on screen.
    The small-font content no longer has to be decoded to identify it, since
    the table is known in advance.
  - *Intro text*: matched by the raw tile-VALUE sequence against the known EN
    and JP intro lines. This is independent of the "even entry count" check
    used for names, because intro lines can have an ODD count (each `...` dot,
    `X` or kana-punctuation takes 1 array slot instead of 2).
- **Replacement**: variable-length. New per-letter positions are computed from
  the width tables, and the replacement string object and its array are built
  inside a single `VirtualAlloc` block placed near the module (the same
  approach as `EnglishText.cpp`).
- **Cache**: built replacements are cached (`g_flavorTextPatchedCache`)
  instead of being rebuilt every frame — important for the small font, which
  "shakes" if rebuilt constantly.

### `BossNameText.json`

Embedded as an `RCDATA` resource (`IDR_BOSSNAMETEXT`), in the same pattern as
`GameTextUS.json`:

| Key | Slots | Meaning |
|---|---|---|
| `boss_names` | 0-7 | Big-font boss name banner, in boss order |
| `flavor_text` | 0-7 | Small-font phrase under it |
| `intro_en` | 0-2 | English-build intro screens |
| `intro_jp` | 1-2 | Japanese-build intro screens (screen 0 is shared) |

A slot left empty means "leave the original untouched". A `\n` inside a value
marks a manual line break (e.g. `"BEGIN SEARCHING FOR\nTHEIR MASTER..."`).

### What can be written

| Font | Supported characters |
|---|---|
| Big font | A-P, R-U, Y, Z; digits `0` and `2`; `.` and `X` (intro path); the kana/punctuation listed in section 6 |
| Small font | A-J, L-P, R-U, W, Y, `!`, `'` |

Not available: in the big font, Q, V, W, other digits and other punctuation;
in the small font, K, Q, V, X, Z (they do not exist in that font) and any
digit or punctuation beyond `!` and `'`. A character with no mapping cannot be
drawn until someone captures a screen that uses it.

### Two-line flavor text

A `\n` in a `flavor_text` value starts a second line, 8px below the first (the
same distance measured between "WATCH" and "YOUR STEP" in the original data).
Two-line text is **left-anchored** and built left to right, following a native
capture of the 2-line intro screen, where both lines share the same leftmost
X. Single-line text keeps the original, already-proven right-anchored path, so
that case is untouched.

An earlier right-anchored two-line attempt rendered "shifted to the right".
The left-anchored replacement is the fix for it, but the notes do not record a
new in-game confirmation of it for flavor text.

### Build modes (production vs diagnostic)

The project has two mutually exclusive modes, and it is easy to forget to
switch back:

- **Production**: `BossNameText.cpp` is part of the project and
  `GlyphDrawDebug.cpp` is excluded. In `dllmain.cpp`, `InstallBossNameHook`
  is active and `InstallGlyphDrawHook` is commented out.
  `LoadBossNameLocalization()` must also be called (forgetting it has
  happened before).
- **Diagnostic** (to capture new data, such as the width of a new letter): the
  opposite. `GlyphDrawDebug.cpp` is included, `BossNameText.cpp` is excluded,
  and the hooks are swapped. It writes `CHARDUMP` lines to the log with every
  field of every `charEntry`, including each entry's memory pointer, which is
  handy with Cheat Engine.

The two modes cannot be combined because both hook the **same function**
(`FUN_14005dd80`) and MinHook allows only one hook per address.

The built DLL is deployed as an `.asi` file, copied manually (there is no
post-build step). Rebuild, copy, and only then open the game.

## 8. Open items

1. **T/S anomaly in "BURST MAN"** (big font) — see section 4. It only affects
   *reading* those two glyphs in that word; writing new text is unaffected.
2. **Untested widths** — several letters in both fonts still use the 8px
   default for lack of real data. Big font: B, C, F, G, H, I, J, P, Q, S, V,
   W, X, Y. Small font: only what appeared in "DON'T SLIP!" and "WATCH YOUR
   STEP" was measured (T is settled at 8).
3. **Small-font `L`**: confirm 48 versus the stray 53 seen once.
4. **Four bosses not yet captured** (Spring Man, Shade Man, Slash Man, Turbo
   Man) — would complete the big-font alphabet and reveal more two-line
   phrases.
5. **Two-line flavor text**: confirm the left-anchored layout in game.
6. **Line-break rule of the native two-line phrases** (e.g. "FORGOTTEN
   FACTORY"): not decoded. Each character appears to carry its Y "baked in",
   with no dynamic calculation; replacements use a manual `\n`.
7. **Japanese build**: boss names and flavor text show in English even there,
   and no hidden Japanese content was found in this system. The Japanese
   intro screens are mapped but a replacement has not been recorded as tested.
8. **Missing glyphs**: punctuation in the big font (beyond `.` and the kana
   punctuation), and digits other than 0 and 2.
