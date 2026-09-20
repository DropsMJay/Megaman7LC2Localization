# Research tools

These scripts were used to reverse-engineer *Mega Man 7* in *Mega Man Legacy
Collection 2* and to extract its dialogue into the JSON files that the mod
embeds. **You do not need them to use or build the mod.**

Requirements: Python 3. Standard library only, no `pip install` needed.

## Extracting text

Both `.bat` files take the path to `MMLC2.exe` (default: `MMLC2.exe` in this
folder):

| Script | Output | Notes |
|---|---|---|
| `ExtractEnglishText.bat` | `GameTextUS.extracted.json` | runs `decode_en_text.py` |
| `ExtractJapaneseText.bat` | `GameTextJP.extracted.json` | runs `build_japanese_strings_json.py` |

The outputs are named `*.extracted.json` on purpose, so they never overwrite
the translation files (`GameTextUS.json` / `GameTextJP.json`).

## Known limitation: `decode_en_text.py` does not reproduce `GameTextUS.json`

English dialogue is stored as arrays of line pointers with no terminator, so
the extractor can keep reading into the next array in memory. The included
version of `decode_en_text.py` only stops at other known entries, so it still
produces leaked lines (usually credits text) after the real dialogue.

The `GameTextUS.json` in this repository was **cleaned by hand**. The entries
that were corrected are: 3, 39, 90, 98-106, 113-122, 131, 144-151, 157 and 160.
If you regenerate the file, compare against these entries before using it.

## Files

| File | Purpose |
|---|---|
| `decode_en_text.py` | Extracts English dialogue |
| `build_japanese_strings_json.py` | Extracts Japanese dialogue; also `--validate` for translated files |
| `decode_jp_text.py` | Decodes Japanese text from given RVAs |
| `mm7_custom_strings.py` | `strings`-like scanner for the game's own 1-byte font codepage |
| `known_array_starts_ordered.h`, `known_ctx_starts_ordered.h` | Addresses of the 171 English table entries, used by `decode_en_text.py` |
| `known_array_starts.h` | Older, superseded address list |
| `ghidra_pairs_clean.txt` | Japanese table addresses, used by `build_japanese_strings_json.py` |
| `find_literal_ptrs.py` | Scans the Japanese table builder function (`UndefinedFunction_1400017b0`, RVA `0x17b0`-`0x2bba`) for pointers to plain ASCII strings (credits names, etc.). Prints the results to the console |

For what is known about the game's text system, see
`docs/MM7Loc_INVESTIGATION_SUMMARY.md`.