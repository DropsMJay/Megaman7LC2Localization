# MM7Loc v0.1.0

First public release of MM7Loc, a runtime text patch for **Mega Man 7** in
*Mega Man Legacy Collection 2*.

## What's included

- English dialogue patching (all 171 entries)
- Japanese dialogue patching (all 171 entries)
- Boss-name banner, stage-select flavor text and intro cutscene text
- System labels: Sound Test, Versus Mode / Player Select, credits screen and
  the opening history/copyright screen

## Compatibility

- **Game:** *Mega Man Legacy Collection 2* (Steam), **TODO: the version / build
  you tested.**
- **System:** Windows, 64-bit
- The mod hooks fixed addresses in `MMLC2.exe`, so a game update can break it.

## Installation

**TODO: same steps as the README.** Copy `MM7Loc.asi` to your ASI loader's
plugin folder and start the game. To uninstall, delete the file.

## Download

`MM7Loc.asi` — SHA-256: **TODO: paste the hash**

(On Windows PowerShell: `Get-FileHash MM7Loc.asi -Algorithm SHA256`)

## Known limitations

- A few characters are not available in the boss-name and flavor-text fonts
  (for example K, Q, V, X and Z in the small font), so text using them can't be
  drawn there yet.
- The "····BASS····?" dots in the English text are not stored as text. A
  translated line has to add the dots itself.
- Two-line flavor text uses a left-aligned layout that has not been confirmed
  in game.
- The Japanese intro screens are mapped, but a replacement has not been tested
  on the Japanese build.

See `Docs/BOSSNAME_Font_Investigation.md` and
`Docs/MM7Loc_INVESTIGATION_SUMMARY.md` for details.

## Notes

- Unofficial fan project, not affiliated with Capcom. You need your own copy of
  the game.
- Source code: MIT License. See `THIRD_PARTY_NOTICES.md` for the libraries used.
