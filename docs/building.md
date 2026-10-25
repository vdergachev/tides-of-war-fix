# Building and code layout

## Tools

- Visual Studio 2022 (Build Tools are enough) with the x86 C compiler and a Windows SDK.
- `vsenv.cmd` finds Visual Studio through `vswhere` and sets up the x86 environment
  (prefers SDK 10.0.26100, falls back to the default one).

| Command | Result |
|---|---|
| `build.cmd` | `build\tow_fix.dll`, `build\tow_patch.exe`, `build\tow_fix.ini` (+ `.pdb`) |
| `format.cmd` | clang-format over `src\` (`.clang-format`) |
| `lint.cmd` | format check + clang-tidy (`.clang-tidy`); non-zero exit on findings |

Target: Windows 7 SP1+ (`_WIN32_WINNT=0x0601`, `/SUBSYSTEM:…,6.01`), static CRT. Newer APIs are loaded at run time.

## How the fix is loaded

1. `tow_patch.exe` appends a section `.towimp` to `TOW.exe` with a copy of the import descriptors plus one for
   `tow_fix.dll!TowFixImport`, updates the headers and the checksum. The game code is not changed on disk.
2. `DllMain` only verifies the PE and the entry-point bytes and places a jump at the entry point.
3. The entry stub calls `tow_init` on the main thread before the game's CRT starts (outside the loader lock),
   restores the entry bytes and continues into the game.
4. `tow_init` writes `tow_fix.log`, reads `tow_fix.ini`, identifies the game by the SHA-256 of its `.text`
   section and applies the enabled fixes. Every code patch first verifies the exact expected bytes; an unknown
   build is left untouched.

## Source files

| File | Purpose |
|---|---|
| `src/dll/main.c` | entry hook, settings, version check, order of fixes |
| `src/dll/patch.c`, `iat.c` | verified code patches, import-slot replacement |
| `src/dll/game_folder.c` | game folder from the EXE path (no SUBST) |
| `src/dll/saves.c`, `registry.c` | saves in `saves\`, MFC profile in `saves\settings.ini`, no registry writes |
| `src/dll/startup_warning.c`, `game_speed.c`, `draw_sync.c` | Windows 95 warning, game pace, draw after each step |
| `src/dll/idle_wait.c` | wait for messages instead of busy-polling an empty queue |
| `src/dll/d3d_fix.c` | Direct3D RGB device and white LVERTEX diffuse (textured ships) |
| `src/dll/scaling.c`, `display.c` | integer scaling, mouse/dialog/font/cursor mapping, borderless fullscreen |
| `src/patcher/tow_patch.c` | patcher (console and double-click dialog modes) |

## Code style

One statement per line, logical blocks separated by blank lines, short functions, game addresses and expected
bytes as named constants with a comment, `// clang-format off/on` around `__asm` functions with a comment on the
stack state. Run `format.cmd`, `lint.cmd` and `build.cmd` before a commit.
