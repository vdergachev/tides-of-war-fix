# Changelog

## 0.17.0

- Sharp high-resolution ships drawn from the game's own 3D models.
- Choose ship detail with `[hd_ships]` render_scale: 0 turns it off, 1 keeps the original look,
  2/3 use coarser pixels, and N (the output scale) gives full sharpness.
- Optional one-step ship dump for developers: `[hd_ships]` dump_step selects the mission step to capture;
  0 disables it. The offline ship_replay tool can render the captured geometry.
- Moving the mouse over the Cargo window no longer makes the scene jump.
- Cargo and its tabs hide while the game is paused and return on resume (`[cargo_pause]`).
- Gun smoke on every successful shot (`[gun_smoke]` every_shot); the in-game smoke option must be enabled.
- Ship rendering fixes, with the original ship image used when model data or GPU rendering is unavailable.

## 0.16.0

- Wake behind every ship: the game's own ripple algorithm plus bubbles, drawn in the smooth scene on the sea under
  hulls and clouds, kept off the land (`[wake]`, `enabled=0` turns it off).
- No more frozen world after fast clicks on ships: the game created a new draw timer on every selection without
  stopping the old one, ran out of system timers and lost its step timer. Only one draw timer is kept now, and a lost
  step is restarted in single-player games (`[timer_guard]`).
- The screen keeps updating if the game stops stepping for a moment.
- Ships near other ships no longer carry a shifted piece of a neighbour's sails: each ship's image is captured
  separately.
- No cursor jump when grabbing the sail or crew controls (`[scaling] soft_cursor_clip`); restoring a saved cursor
  clip no longer enlarges it twice.
- No main-menu flash behind the panels on fast clicks (`[d3d11] gdi_refresh_ms`).
- Optional diagnostics for bug reports (`[diag]`, all off by default).
- The render code is now C++17 (no exceptions, no RTTI) split into classes.

## 0.15.0

- Direct3D 11 output in a child window with vsync, with GDI fallback.
- Smooth mission scenes at the monitor refresh rate: sea, coasts, hulls, masts and sails, effects, and clouds are
  interpolated between game steps.
- Clouds are drawn from the game's own art; masts and booms keep their colour.
- `[d3d11] enabled`, `smooth`, and `warp` settings can switch the path off or control its fallback.

## 0.14.0

- No more 100% load of one CPU core: the game's own message loop busy-polled an empty queue;
  it now sleeps until the next message or timer (`[idle_wait]`, at most `max_wait_ms` = 10 ms per wait).

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
