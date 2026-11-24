# Steam Deck & Controllers

The engine speaks SDL GameController natively, so any XInput-style pad — and the
Steam Deck's built-in controls — work without extra drivers.

## Setup on the Steam Deck

1. Unpack the portable Linux release (`tools/package_portable_linux.sh`) in Desktop
   Mode, e.g. to `~/Games/mha2`, together with your `game_files/` and `hd_assets/`.
2. Steam → *Add a Non-Steam Game* → select `mha2_linux`.
3. Properties → *Launch Options*: `-f` (borderless fullscreen).
4. Properties → *Controller*: **Gamepad with Mouse Trackpad** (the default for
   non-Steam games). The right trackpad then acts as a precise mouse in addition to
   the left-stick cursor.

The Deck's 1280×800 panel is 16:10; the default aspect mode **Auto** picks the
zero-stretch 16:10 layout (`F1 → 1. DISPLAY → Aspect`).

## Default controller mapping

| Control | Action |
|---|---|
| Left stick | Move cursor (clamped to the visible playfield) |
| Right trackpad | Mouse (Steam Input template) |
| **A** | Left click (fixed) |
| **B** | Right click / cycle action (fixed) |
| **Start** | Escape: skip cutscene, chapter/save screen (fixed) |
| **Back / View** | F1 settings menu |
| **X** (hold) | Highlight hotspots |
| **Y** / **LB** (hold) | Fast walk 2× |
| **RB** | Before/after split-screen |
| **D-pad Up** | Quick save (slot 9) |
| **D-pad Down** | Quick load (slot 9) |

Inside the F1 menu: D-pad navigates, A changes a value, LB/RB switch tabs, B/Start
close it.

## Remapping controller buttons

Keyboard keys are rebound in-game (`F1 → 4. CONTROLS`). Controller buttons are set in
`mha2_settings.ini` (two slots per action; empty = unbound):

```ini
pad_quick_save=dpup
pad_quick_load=dpdown
pad_hotspots=x
pad_fast_walk=y
pad_fast_walk_2=leftshoulder
pad_compare_split=rightshoulder
pad_settings_menu=back
pad_perf_hud=dpleft          # example: add an FPS HUD toggle on D-pad Left
```

Button names: `a b x y back guide start leftstick rightstick leftshoulder
rightshoulder dpup dpdown dpleft dpright misc1 paddle1 paddle2 paddle3 paddle4
touchpad`. A, B and Start are reserved for click / right click / Escape. Assigning a
button to one action removes it from any other action.

## Quick save notes

Quick save writes only slot 9 (`mha9.sav`) and never overwrites a save there that it
did not create itself; it is available while exploring a room (not during cutscenes,
dialogs or menus). Quick load needs an existing quick save.
