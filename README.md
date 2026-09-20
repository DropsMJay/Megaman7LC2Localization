# MM7Loc

A text-patching mod for **Mega Man 7** as shipped in *Mega Man Legacy Collection 2*
(`MMLC2.exe`). It is a small DLL, built on [MinHook](https://github.com/TsudaKageyu/minhook),
that replaces the game's text in memory at runtime. Nothing on disk is modified.

The original US release has scenes the localization team never got around to
translating; those spots reuse generic placeholder text (for example "YOU GET")
instead. MM7Loc can patch both the English and the Japanese text, so that
content can be recovered and translated from the Japanese source.

> **Unofficial fan project.** Not affiliated with or endorsed by Capcom. You need
> your own copy of the game. *Mega Man* and all related names are trademarks of
> Capcom.

## What it patches

| System | Text file | What it covers |
|---|---|---|
| English dialogue | `GameTextUS.json` | The 171-entry dialogue table |
| Japanese dialogue | `GameTextJP.json` | The same 171 entries, Japanese build |
| Boss names, flavor text, intro | `BossNameText.json` | The boss-name banner and the short phrase under it on the stage select screen, plus the intro cutscene text (a separate system with its own fonts) |
| System labels | `MiscText.json` | Sound Test, Versus Mode / Player Select, "THANK YOU FOR PLAYING!" and the opening history/copyright screen |

All four files are embedded into the DLL as resources when it is built, so the
mod does not read any external file at runtime.

## Requirements

- *Mega Man Legacy Collection 2* (Steam). version / Steam build you tested.**
- An ASI loader, because the built DLL is used as an `.asi` plugin. (d3d11.dll recommended)

**Important:** the mod hooks fixed addresses inside `MMLC2.exe`. It only works
with the game build it was made for, and a game update can break it.

## Installing

1. Install an ASI loader in the game folder (follow the loader's own
   instructions).
2. Copy `MM7Loc.asi` (the built `MM7Loc.dll`, renamed) to a "scripts" folder where the MMLC2.exe is
3. Start the game.

To uninstall, delete `MM7Loc.asi`.

## Editing the text

The four JSON files are the scripts, and they are what you edit to translate.
Because they are embedded, **rebuild the DLL after any change**.

- **`GameTextUS.json`** — edit only the `text` field of an entry.
- **`GameTextJP.json`** — edit only the `text` field of an entry. `header_bytes`
  is what actually gets patched and is preserved byte for byte;
  `header_decoded` is a readable copy for humans and is ignored by the mod.
- **`BossNameText.json`** — `boss_names`, `flavor_text`, `intro_en`, `intro_jp`.
  A `\n` inside a value marks a line break; an empty value leaves the original
  text untouched. Only some characters exist in the game's fonts; see
  [`Docs/BOSSNAME_Font_Investigation.md`](Docs/BOSSNAME_Font_Investigation.md).
- **`MiscText.json`** — lists of lines per "system label" slot. Table `A` is the
  Japanese build and `B` is the US build.

Text uses bracketed control codes such as `[NewLine]`, `[WaitForConfirm]` and
`[SetTypeSpeed][XX]`. The full list, with what each one does, is in
[`Docs/JAPANESE_CONTROL_CODES.md`](Docs/JAPANESE_CONTROL_CODES.md).

One rule that catches people out: in the Japanese text, a `[DrawClosingMark]`
must be followed right away by `[NewLine]`, or the next box renders shifted.
This checks a translated file for it:

```
python Scripts/build_japanese_strings_json.py --validate GameTextJP.json
```

## Building

You need Visual Studio with the C++ desktop workload and the **v145** platform
toolset (the project uses C++20).

1. Open `MM7Loc.slnx`.
2. Restore the NuGet packages (`packages.config`): `minhook` 1.3.3 and
   `nlohmann.json` 3.12.0. Visual Studio does this automatically on build.
3. Select **Release | x64** and build. The output is `MM7Loc.dll`.
4. Rename or copy it to `MM7Loc.asi`.

### Debug mode

`IsDebugModeOn` in `Logging.h` is the master switch for every diagnostic. Leave
it `false` for a normal build: the mod then writes nothing to disk and installs
no diagnostic hooks. Set it to `true` to get `mm7loc_log.txt` next to the DLL,
plus extra output that you can read with
[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview).

## Repository layout

| Path | Contents |
|---|---|
| `*.cpp`, `*.h` | The mod's source |
| `GameTextUS.json`, `GameTextJP.json`, `BossNameText.json`, `MiscText.json` | The embedded text files |
| `MM7Loc.rc`, `resource.h` | Embeds the JSON files into the DLL |
| `Docs/` | Reverse-engineering notes; start with `MM7Loc_INVESTIGATION_SUMMARY.md` |
| `Scripts/` | Python scripts used to extract the text. Not needed to build or use the mod; see `Scripts/README.md` |

## Documentation

- [`Docs/MM7Loc_INVESTIGATION_SUMMARY.md`](Docs/MM7Loc_INVESTIGATION_SUMMARY.md) — how the text system works, the bugs found, and an address reference
- [`Docs/JAPANESE_CONTROL_CODES.md`](Docs/JAPANESE_CONTROL_CODES.md) — every control code
- [`Docs/TEXT_FINDING_GUIDE.md`](Docs/TEXT_FINDING_GUIDE.md) — how to find text in the game
- [`Docs/BOSSNAME_Font_Investigation.md`](Docs/BOSSNAME_Font_Investigation.md) — the boss-name, flavor-text and intro font system

## License

The source code is released under the [MIT License](LICENSE). It uses
[MinHook](https://github.com/TsudaKageyu/minhook) and
[nlohmann/json](https://github.com/nlohmann/json); their licenses are in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

The game's own text, contained in the JSON files, remains the property of
Capcom and is not covered by that license.
