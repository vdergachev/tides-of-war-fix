# Tides of War fix

**English** | [Русский](README.ru.md)

Unofficial fix for **Tides of War** (Devil's Thumb Entertainment, 1998) on modern Windows.
Not affiliated with the original developers or publisher. The fix contains only its own code; you need your own copy of the game.

## What it fixes

- **Ships are drawn with textures** instead of solid red (Direct3D RGB renderer instead of Ramp, white vertex diffuse).
- **Integer scaling ×2/×3/×4** of the 640×480 game, sharp pixels, the window is centred; mouse, dialogs, buttons, fonts and cursor follow the scale.
- **Borderless fullscreen** with black bars (the in-game fullscreen option); the desktop resolution is never changed.
- **Starts from its own folder** by double-click — no `SUBST` drive and no compatibility settings needed.
- **Saves and settings next to the game** (`saves\`), nothing is written to the registry or to Program Files.
- **Smooth motion** of other ships (each game step is drawn right away) and a configurable **game pace**.
- **Smooth motion at the monitor refresh rate** in mission scenes (Direct3D 11 with vsync; Windows 7 SP1+, falling back
  to GDI).
- **No more 100% CPU core load:** the game sleeps while it waits for messages instead of busy-polling.
- The "designed for Windows 95" warning is skipped.

Every fix can be switched off in `tow_fix.ini`.

## Requirements

- Windows 10 or 11 (Windows 7 SP1 is a target but not tested yet).
- Tides of War, **English 1998 release** — the patcher accepts only these `TOW.exe` files (SHA-256):
  - `4cd1a3ddad2007e5c09fd55b02922ff59fdd20434d95e433e6e85ce7cb327967` — original;
  - `9dcf3f69bf505208e9826e6b9a56bb2356e2a7794debdc3ac589c1fbfc0a8289` — original with a no-CD patch.
  Other builds (localized versions, other patches) are not supported yet — please report their hash.
- **The game's ActiveX controls `Anibtn32.ocx` and `Threed32.ocx` must be installed and registered.**
  The original CD installer does this. If your copy of the game was not installed from the CD, use the
  ["Windows XP fix" from old-games.ru](https://www.old-games.ru/game/download/get.php?fileid=2796&modal=1):
  copy both `.ocx` files into the game folder and run its `setup.bat` once **as administrator**.
  Without them the game crashes when you open the port screen (Trade).
- The game still checks for its CD.

## Install

1. Download `tow_fix-<version>.zip` from [Releases](../../releases) and unpack it into the game folder (next to `TOW.exe`).
2. Double-click `tow_patch.exe` and confirm. It checks the game version, keeps a backup `TOW.exe.bak`
   and adds `tow_fix.dll` to the import table of `TOW.exe`.
3. Start the game with `TOW.exe` as usual.

To remove the fix, double-click `tow_patch.exe` again and choose to restore `TOW.exe.bak`.
From a command prompt: `tow_patch.exe [--check | --restore] [path\to\TOW.exe]`.

## Settings

**Nothing has to be configured — everything is already enabled and tested.**
Do not edit `tow_fix.ini` unless you know what you are doing: a wrong value can bring back red ships or break the
interface. If something breaks, take `tow_fix.ini` from the release archive again.

Safe to change: `[game_speed] percent` (game pace) and `[scaling] factor` (zoom).

For reference, the main keys (`1` = on, `0` = off):

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[scaling]` | `factor` | `auto` | `0` off, `2`..`4`, or `auto` (largest that fits the screen) |
| `[scaling]` | `fullscreen` | `borderless` | in-game fullscreen option: `borderless` or `off` (always windowed) |
| `[game_speed]` | `percent` | `80` | game pace, 100 = original 62 ms step |
| `[rgb_device]`, `[white_diffuse]` | `enabled` | `1` | textured ships |
| `[idle_wait]` | `enabled` | `1` | sleep instead of busy-polling (CPU load) |
| `[save_redirect]`, `[registry_guard]` | `enabled` | `1` | saves and settings in `saves\`, no registry writes |
| `[d3d11]` | `enabled`, `smooth` | `1` | set either to `0` to switch off the D3D11 path if you see problems |
| `[wake]` | `enabled` | `1` | wake behind every ship |
| `[timer_guard]` | `enabled` | `1` | no frozen world after fast clicks on ships |
| `[hd_ships]` | render_scale | 16 (clamped to N) | ship detail: 0 off, 1 original look, 2/3 coarser pixels, N full sharpness; D3D11 smooth scene only |
| `[hd_ships]` | dump_step | 0 | developer capture of one mission step; 0 off |
| `[cargo_pause]` | enabled | 1 | hide Cargo and its tabs during single-player pause |
| `[gun_smoke]` | every_shot | 1 | smoke on every successful shot; requires the in-game smoke option |

A log is written to `tow_fix.log` next to `TOW.exe`; please attach it to bug reports.

## Building from source

Visual Studio 2022 Build Tools (x86 C compiler, Windows SDK). Run `build.cmd`; the result is in `build\`.
`format.cmd` and `lint.cmd` use clang-format/clang-tidy shipped with Visual Studio. See [docs/building.md](docs/building.md).

## License

MIT — see [LICENSE](LICENSE). Tides of War and its files belong to their respective owners.
