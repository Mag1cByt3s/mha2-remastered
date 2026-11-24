# Input Record / Replay & Regression Testing

`mha2_linux` can record keyboard/mouse input to a text script and drive the game
from such a script. `tools/regress.sh` builds on this for headless, sandboxed
smoke/regression runs.

```bash
./mha2_linux --record session.rec                 # play normally, input is recorded
./mha2_linux --replay session.rec                 # replay it (real input is ignored)
./mha2_linux --replay a.rec --record b.rec        # re-record injected input (script -> recording)
./mha2_linux --replay a.rec --replay-out out/     # screenshots go to out/ (default screenshots/replay)
nix-shell --run tools/regress.sh                  # run tests/replay/*.rec headless
```

## Timing model

Event times are **milliseconds since the last sync point**. Sync points are
`wait` lines; a replay blocks on them until the game reaches that state, then
restarts its clock. This keeps scripts aligned no matter how long the intro,
loading screens or room transitions take on a given machine. Recordings emit
sync points automatically (`wait video`, `wait menu`, `wait room ...`).

Mouse coordinates are **guest 800×600 coordinates**, injected after the window
mapping, so scripts are independent of window size, fullscreen and the
4:3 / 16:10 / 16:9 layout. Keys and text are pushed through SDL and take exactly
the same path as real input (including F-keys and QoL shortcuts).

## Script syntax

One command per line; `#` starts a comment. `<t>` is absolute ms within the
current segment, `+<t>` is relative to the previous event.

| Command | Meaning |
|---|---|
| `wait video [timeout_s]` | Block until an intro/cutscene video plays |
| `wait menu [timeout_s]` | Block until the main menu is shown |
| `wait room <prefix> [timeout_s]` | Block until the active room is `<prefix>` (e.g. `l/005`) |
| `wait leave <prefix> [timeout_s]` | Block until a room other than `<prefix>` is active |
| `wait interactive [timeout_s]` | Block until the room accepts input (hotspots collectable; ends monologues/cutscenes) |
| `<t> key down\|up <SDL key name>` | Key event (`Escape`, `Space`, `Left Ctrl`, `F1`, `A`, …) |
| `<t> tap <SDL key name>` | Key down, release 80 ms later |
| `<t> text <utf-8 text>` | Text input (e.g. savegame names) |
| `<t> move <x> <y>` | Move the mouse (travels in ≤48 px steps like a real mouse) |
| `<t> pad <button> down\|up` / `<t> padtap <button>` | Controller button (`a`, `x`, `back`, `dpup`, …) |
| `<t> quicksave` / `<t> quickload` | Run the quick save/load macro (as Ctrl+S / Ctrl+O would) |
| `<t> savecheck <slot>` | Fail unless `mha<slot>.sav` exists and is non-empty |
| `<t> toast <text>` | Show a toast message |
| `<t> button left\|right\|middle down\|up [<x> <y>]` | Mouse button, optionally at a position |
| `<t> click <x> <y> [left\|right\|middle]` | Hover 250 ms, press, release 120 ms later |
| `<t> clickspot <label>` | Click the first room hotspot whose label contains `<label>` |
| `<t> hotspots` | Log all room hotspots (kind, anchor, box, label) — for authoring |
| `<t> shot <name>` | Clean screenshot (game image) to `<out>/<name>.png` |
| `<t> shotfull <name>` | Final frame incl. overlays/HUD/menus |
| `<t> hash <name>` | Log FNV-1a hash of the last presented 800×600 guest frame |
| `<t> log <text>` | Log text with current room/menu state |
| `<t> expect room <prefix>` | Fail unless the active room matches |
| `<t> expect menu` | Fail unless the main menu is shown |
| `<t> expect hotspots <min>` | Fail unless at least `<min>` hotspots are collectable |
| `<t> quit` | Print `[replay] PASS`, close the game (forced exit after 20 s) |

A failed `wait`/`expect` prints `[replay] FAIL (script line N): …` and exits
with code 3. When a script ends without `quit`, real input is re-enabled.

`#! threshold <shot> <rmse>` lines override the screenshot comparison threshold of
`tools/regress.sh` for one shot (e.g. pulsing hotspot markers).

Notes:

- An event that fires late (blocked frame, absolute time already passed) shifts the
  rest of its segment by the same amount, so relative spacing is preserved — a
  `click` always keeps its hover delay.
- Quick save/load macros (hotkeys, controller or `quicksave`/`quickload`) pause the
  script and run atomically; don't `wait` for the transient save screen around them.

- The game only accepts clicks on a target that is already hovered; `click` and
  `clickspot` hover for 250 ms first. Raw `button` events do not.
- Room prefixes come from the HD background index, so room sync requires the
  HD asset pack (`hd_assets/`).
- Prefer `clickspot` over coordinates for in-room objects; labels come from the
  game's own string table.

## tools/regress.sh

Each script runs in a fresh sandbox under `work/regress/<timestamp>/<script>/`:
`--game-dir` points at symlinks to the read-only game data plus an **empty**
`mha2.sav/`, and the working directory is the sandbox (default settings, crash
log inside). Real savegames and `mha2_settings.ini` are never touched.

A script passes when the game exits 0, the log contains `[replay] PASS` and no
crash/fatal markers, and — if a local baseline exists — every screenshot is
within the RMSE threshold. Baselines contain copyrighted game imagery and are
kept in `work/regress_baseline/` (gitignored) only.

```bash
tools/regress.sh                       # headless (SDL offscreen driver), dummy audio
tools/regress.sh --visible             # show the window
tools/regress.sh --strict-mem          # additionally run every script with --strict-mem
tools/regress.sh --update-baseline     # accept current screenshots as the baseline
tools/regress.sh --threshold 0.03 tests/replay/boot_menu.rec
```

Typical run-to-run RMSE: static screens < 0.007, animated rooms (torch flicker,
dust motes, idle animations) ≈ 0.02; the default threshold is 0.05.

Included scripts (`tests/replay/`):

- `boot_menu.rec` — intro → skip → main menu → chapter select (~12 s)
- `smoke_newgame.rec` — new game → story intro → first room `l/001`
  (monologue, hotspot highlighter) → exit to `l/005` (~55 s)
- `overlays.rec` — F1 menu tabs, dialog log, F2 split, hotspot markers (~50 s)
- `quicksave.rec` — quick save guards, save, walk, load back, overwrite (~115 s)
- `controls.rec` — rebind the dialog log in `F1 → 4. CONTROLS` (~20 s)
- `controller.rec` — controller bindings incl. D-pad quick save/load (~110 s)
- `overlay_text.rec` — Overlay Text Size 175% (dialog log, labels) (~60 s)
