# Engineering Plan — Post-v5.3 Hardening & QoL

Status legend: `[ ]` open · `[~]` in progress · `[x]` done (verified)

Each task is executed and verified before moving to the next:
clean `-Wall -Wextra` rebuild (and `make lift` when `recomp/lift.py` changes),
inspection of regenerated `recomp/gen/`, targeted unit/stress tests, and a live
smoke run of `./mha2_linux` (boot → main menu → load save → room transitions).

---

## Phase A — Correctness & Robustness

### A1. Unify the meta-spinlock (`0x0051b3b8` / owner `0x00527824`)
- [x] Inventory every inlined acquire/release sequence of the non-atomic
      meta-spinlock (`0x00444470`, `0x004446b0`, `0x004447a0`, `0x00444880`,
      `0x004449c0`, `0x00444b10`, unlock `0x00444660`, EH funclet `0x004f53f0`).
- [x] `recomp/lift.py` `ATOMIC_COUNTER_GLOBALS` peephole: every
      `mov r,[X]; inc/dec r; mov [X],r` → one atomic RMW (52 sites); every other
      store to X (forced lock-steal) → `guest_spin_acquire()` (9 sites); unknown
      access patterns abort the lift. (Chosen over per-site splicing: ~40 sites
      with varying registers.)
- [x] `runtime/guest_sync.c`: `guest_spin_acquire()` + re-entrant
      `guest_owner_lock_enter/leave()`; native `Lock`/`TryLock` patches rewritten on
      the same protocol (TryLock again holds the meta-lock around its
      check-and-set, like the original).
- [x] Verified: 0 raw `0x51b3b8u` accesses left, gen diff limited to the 226
      lock lines; `make test` (`tests/test_guest_sync.c`) 0 violations / 0 lost
      updates over 3×32k mixed acquisitions (legacy 2005 protocol: ~3k
      violations); gdb hit counts show all 6 inlined-lock functions executing
      during boot/menu with no hang.

### A2. Guest memory guard + crash diagnostics
- [x] `runtime/guest_mem.c` (split from `cpu.c`): full 4 GiB + 64 KiB guest window
      reserved `PROT_NONE`, only `GUEST_MEM_SIZE` committed `RW`.
- [x] `runtime/crash_handler.c`: `SIGSEGV`/`SIGBUS`/`SIGFPE`/`SIGILL`/`SIGABRT` on a
      per-thread alt stack; guest fault VA + region, guest register dump, symbolized
      backtrace (`-rdynamic` → `sub_<VA>` frames = guest call stack), copy in
      `mha2_crash.log`, re-raise for core dumps; externally sent signals labelled.
- [x] `--strict-mem` protects guest `0x0–0xFFFF`.
- [x] Verified: `tests/test_guest_mem.c` (heap integrity/coalescing + 7 fault
      scenarios incl. 4 GiB straddle, NULL page, host stack overflow); boot/menu
      clean with and without `--strict-mem`; live `kill -SEGV` shows guest stack
      `sub_004a7227 → … → sub_0048c6b0`. Gameplay `--strict-mem` coverage → B1.

### A3. C++ exception & arithmetic fault diagnostics
- [x] `RaiseException` decodes MSVC `0xE06D7363` (type + catchable chain via
      ThrowInfo, guest `what()`, throw site) before `abort()`. Only throwers in
      `mha2.exe`: STL `_Xlen`/`_Xran` (`length_error`/`out_of_range`/`logic_error`).
- [x] Lifter discovery fix: data-pointer scan rejected functions placed right after
      `ret imm16` (`C2 xx 00`); recovered `exception::what()` (`0x004a6749`) and an
      SEH filter (`0x004a7d56`) — 8134 → 8136 functions, none lost.
- [x] `div`/`idiv` 8/16/32: zero divisor and quotient overflow → `guest_divide_error()`
      (#DE report + abort) instead of host trap / silent truncation; `INT_MIN / -1`
      no longer traps in host code.
- [x] Verified: gdb-injected guest `std::_Xran()` → full report incl.
      `what(): "invalid string position"`; 563k randomized div/idiv results vs
      `__int128` reference + 41 #DE cases; boot/menu clean.

### A4. Small semantic fixes
- [x] FIST/FISTP m16/m32 stored wrapped values for out-of-range input; now x87
      "integer indefinite" via `fpu_to_int()` (7 sites); `__ftol` patch shares
      `x87_ld_to_int()`. Verified bit-exact vs host x87 `FISTP` (800k cases, all RC modes).
- [x] `pushfd` pushes IF (`0x202` baseline) — no live sites in `mha2.exe`.
- [x] Guest threads get an explicit 32 MiB host stack; main thread soft
      `RLIMIT_STACK` raised to 32 MiB.

## Phase B — Testing Infrastructure

### B1. Deterministic input record/replay regression harness
_Pulled forward: KWin/XWayland drops synthetic xdotool button presses, so in-game
smoke tests (load save, walk rooms) need runtime-level input injection._
- [x] `runtime/input_replay.c`: `--record` / `--replay` / `--replay-out`; times relative
      to sync points (`wait video|menu|room`), guest-coordinate mouse injection via
      shared `win32_input_mouse_move/button()`, keys/text through SDL, real input
      suppressed while replaying; `click`/`clickspot` hover first (game ignores
      un-hovered clicks); `shot`/`shotfull`/`hash`/`hotspots`/`expect ...`/`quit`.
- [x] `tools/regress.sh` (+ `make regress`): per-script sandbox (`--game-dir` symlinks +
      empty `mha2.sav/`, sandbox cwd), headless SDL `offscreen` GL, log checks, local
      RMSE screenshot baseline (`work/`, never committed), `--strict-mem` variants.
- [x] `tests/replay/boot_menu.rec`, `tests/replay/smoke_newgame.rec`; docs in
      `docs/REPLAY.md`.
- [x] Verified: 3× full suite incl. `--strict-mem` 12/12 PASS; static-screen guest
      hashes identical across runs/windowed/headless; record→replay round trip;
      user savegames byte-identical after all runs.

### Bugs found by the harness (fixed)
- [x] **Original-game crash race** `0x0042a820` (weighted ambient-variant pick): with no
      item loaded yet the selection stays -1 → `arr[-1]->vfn()` + weight decrement in
      the heap header. Fix via new `GUEST_BUGFIX_INSERTS` lifter hook at `0x0042a8e4`
      (take the function's empty-pool exit). Verified by gdb-forcing the condition
      under `--strict-mem`.
- [x] **Space/H hotspot highlighter dead in most rooms**: (1) exact vtable match
      rejected 11 Location + 5 Minigame subclasses → inheritance probe
      (`hs_vtable_derives()`); (2) `(scene+0x6c)->+0x80` "dialog" guard reads 2 in
      ordinary idle rooms → removed, renderer suppresses markers on detected dialog
      choices (`g_bot_bar_dialog_mode`). Verified `l/001` 24, `l/005` 12, `l/009` 13,
      `l/016` 6, `l/017` 11 hotspots with markers drawn.

## Phase C — Performance & Code Hygiene

### C1. Lifter flag-liveness elimination
- [x] Per-function backward flag liveness in `recomp/lift.py` (`flag_effects()` kill sets
      mirror exactly what each `alu_*` helper writes; CL/zero-count shifts and rep
      cmps/scas are may-defs; call/ret/tail/indirect/HOOKABLE/bugfix-insert edges are
      all-live). Dead results use `nf_*` helpers; dead `cmp`/`test` are dropped.
      `MHA2_LIFT_NOFLAGELIM=1` disables it.
- [x] 27,698 of 68,740 flag computations eliminated (−40%); `.text` 8.24 → 7.75 MB (−6%).
- [x] Verified: `nf_*` vs `alu_*` 5.2M randomized results identical; poison-check lift
      (`MHA2_LIFT_FLAGCHECK=1`: dead flags := 2, 28,514 read sites checked) passes the full
      replay suite incl. `--strict-mem`; negative control (liveness deliberately broken
      for `jne`) aborts within 2 s. Whole-game CPU unchanged within noise (GPU window
      16.8 s → 16.2 s user over the smoke replay) — the game is not guest-CPU bound.

### C2. Split `runtime/gl_renderer.c`
- [x] 5,205 → 3,242 lines: `gl_shaders.c` (GLSL sources, 1,008), `gl_menu.c` (F1 menu,
      853), `gl_dialog_log.c` (Ctrl+L log, 143); private `gl_internal.h` for the 11 shared
      helpers/2 globals. Blocks cut by marker, not hand-edited.
- [x] Verified: zero warnings; new `tests/replay/overlays.rec` (F1 tabs, dialog log, F2
      split, hotspots) baselined *before* the split — post-split RMSE identical to
      run-to-run noise (menus 0.00088 vs 0.00087, dialog log 0.00146 vs 0.00145); full
      suite 3/3.

## Phase D — Features the Original Lacks

### D1. Quick save / quick load
- [x] **Ctrl+S / Ctrl+O** run built-in macros on the replay engine that drive the game's own
      *Kapitelauswahl* screen (Escape → slot 9 → Speichern + name "Quicksave" / Laden) — original
      save code, original thumbnails, no state hacks. Macros nest inside `--replay` scripts
      (script paused), restore the cursor afterwards, and recover (toast + Escape) on failure.
- [x] Guards: only while exploring a room (not menus, videos, cutscenes, dialogs, F1/log);
      quick load requires an existing quick save; slot 9 is only overwritten if it is empty
      or hash-identical to the last quick save (`<game dir>/mha2_quicksave.id`), so a manual
      save there is never destroyed.
- [x] Engine fixes found on the way: SDL_TEXTINPUT injection crashed inside sdl2-compat
      (text now injected via `win32_input_text()`); large single mouse jumps are dropped by
      the game (moves now travel in ≤48 px steps); late events shift the segment so a
      click keeps its hover delay; per-shot RMSE thresholds (`#! threshold`).
- [x] Verified: `tests/replay/quicksave.rec` (guards at menu / monologue / no save, save in
      `l/001`, walk to `l/005`, load back to `l/001` with 24 hotspots, overwrite → label
      "Quicksave"), foreign slot-9 save left byte-identical; full suite incl. `--strict-mem`.

### D2. Rebindable keys + Steam Deck controller profile
- [x] `runtime/input_bindings.c`: 18 actions × 2 keyboard chords + 2 controller buttons,
      persisted as `bind_*` / `pad_*` in `mha2_settings.ini`; conflict displacement;
      Escape / Alt+F4 / Alt+Enter and pad A/B/Start reserved; strict Ctrl/Alt matching so
      plain letters typed into savegame names never trigger hotkeys.
- [x] All keyboard/controller hotkeys dispatch through one `perform_action()`; UI hints
      (dialog log title, menu close row, toasts) follow the live bindings.
- [x] F1 → **4. CONTROLS** tab: Enter/click rebinds slot 1, Left/right-click slot 2,
      Backspace unbinds, Esc cancels, reset-to-defaults row; raw keys (incl. F1) go to the
      capture first; controller-synthesized menu keys are never captured.
- [x] Controller: D-pad Up/Down = quick save/load by default; `docs/STEAM_DECK.md`.
- [x] Verified: `tests/test_input_bindings.c` (matching, conflicts, reserved chords, 36
      chord round trips, 72-entry ini round trip); `tests/replay/controls.rec` (rebind
      Ctrl+L → Ctrl+K in the UI, Ctrl+K opens the log, Ctrl+L no longer does, persisted
      `bind_dialog_log=Ctrl+K`); `tests/replay/controller.rec` (new `pad`/`padtap`
      replay commands: X hotspots, Back menu, D-pad quick save/load); full suite 6/6.

### D3. Lighter distribution (runtime split from asset pack)
- [x] `tools/package_portable_linux.sh --split`: 12 MB runtime `.tar.gz` (binary, 12 libs,
      fonts, launcher, docs, **no game files**), 5.1 GB uncompressed HD asset `.tar` rooted at
      the same folder, `SHA256SUMS`; legacy all-in-one bundle unchanged without `--split`.
- [x] Runtime falls back to `$XDG_DATA_HOME/mha2-remastered/{game_files,hd_assets}` when they
      are not next to the working directory (shared data across runtime versions).
- [x] `docs/INSTALL.md` (layouts, updates, checksums, game file extraction).
- [x] Verified: container rebuild with all new code (symbols checked), ABI checks passed,
      checksums OK, clean extraction of both archives + own game files → bundled binary
      passes `boot_menu.rec`; XDG fallback run from an empty cwd passes `boot_menu.rec`.
- [ ] AppImage / Flatpak not built: `appimagetool` is not in nixpkgs (would need an
      unvetted binary download) and Flatpak needs multi-GB SDK runtimes. Documented.

### D4. Accessibility: text size, dialog pacing
- [x] Measured: the original game already **advances dialog lines on click** (opening
      monologue 38.9 s untouched vs ~11 s with clicks) — documented, no patch needed.
- [x] **Overlay Text Size** (F1 → 3. ASSETS & TEXT, 100–200 %, `overlay_text_scale`): dialog
      log (now word-wrapped via tested `runtime/text_wrap.c`; title splits into heading +
      hint when too wide), hotspot labels, toasts/HUD.
- [x] New replay sync `wait interactive`; `tests/replay/overlay_text.rec`; `tests/test_text_wrap.c`.
- [ ] Follow-up: scaling the game's own in-scene subtitles needs enlarged font metrics behind
      `Font::MeasureText` (0x00486ad0) so layout, wrapping and dirty-rect erase all grow
      together; the vector text is deliberately confined to the measured box today.
- [ ] Follow-up: "hold each line until clicked" requires reverse engineering the dialog timer.

_Translation layer intentionally deferred._
