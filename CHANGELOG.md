# Changelog

## 0.13.2 — first public version

- Patcher `tow_patch.exe`: version check by SHA-256, backup `TOW.exe.bak`, import of `tow_fix.dll`,
  restore; message boxes when started by double-click.
- Start from the game folder without a `SUBST` drive.
- Saves and settings in `saves\` next to the game; no registry writes; one-time copy of old saves and settings.
- "Designed for Windows 95" warning skipped.
- Game pace (`[game_speed] percent`, default 80) and drawing right after each game step (no ship stutter).
- Textured ships: Direct3D RGB device instead of Ramp, white vertex diffuse.
- Integer scaling ×2..×4 (`auto` by default) with mouse, child windows, dialogs, Anibtn32 buttons, fonts and cursor.
- Borderless fullscreen for the in-game fullscreen option; display mode changes are temporary (restored on exit).
