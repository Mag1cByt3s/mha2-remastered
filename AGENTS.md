# AGENTS.md — Contributor & Agent Guide

Guidance for coding agents working on the **Moorhuhn Adventure 2: Der Fluch des Goldes — Native Linux x86_64 Remastered Engine**.

> `CLAUDE.md` is a symlink to this file so all agent configurations read one source of truth.

---

## 1. Project Overview

- **What this is:** A standalone static x86 → C recompilation of the original 2005 Windows game `mha2.exe` (`recomp/lift.py`) paired with a clean-room Win32/DirectX/OpenGL 3.3 Core Remastered v5.3 runtime (`runtime/`).
- **Game assets:** Loaded dynamically at runtime from user-supplied `game_files/` (`mha2.dat`, `mha2.exe`, `*.avi`) and 4K WebP/MP4 packs in `hd_assets/`.
- **Public repository & branch:** `main` (`LICENSE` is MIT and covers **only** original clean-room code, recompiler tooling, and scripts — never original game assets or generated code).

---

## 2. Environment & Build Commands

### NixOS Host Notes
- `/` and `/var/tmp` are mounted on a small **1 GB `tmpfs`**, while `/home` has large disk capacity (`zroot/home`) and `/tmp` is 16 GB.
- Always keep temporary container/build staging under `work/` (e.g., `TMPDIR="$ROOT/work/tmp"` as done in `tools/package_portable_linux.sh`), never `/var/tmp`.

### Native Build & Re-Lifting (`nix-shell`)
- **Build native binary (`./mha2_linux`):**
  ```bash
  nix-shell --run "make -j$(nproc)"
  ```
- **Re-lift `game_files/mha2.exe` into `recomp/gen/` and rebuild** (required after editing `recomp/lift.py`):
  ```bash
  nix-shell --run "make lift && make -j$(nproc)"
  ```
- **Run locally:**
  ```bash
  ./play.sh
  ```

### Standard Linux Build (Ubuntu / Debian / Fedora / Arch)
- **Required packages:** `gcc`, `make`, `pkg-config`, `python3`, `python3-capstone`, and development headers for `SDL2` (`sdl2`), `SDL2_ttf`, `FFmpeg` (`libavformat`, `libavcodec`, `libswscale`, `libavutil`), and `OpenGL` (`gl`).
- **Build & run:**
  ```bash
  make lift && make -j"$(nproc)"
  ./mha2_linux
  ```

### `Makefile` Mechanics & Overrides
- **Overridable variables (`?=`):** `CC`, `PKG_CFG`, `BUILD_DIR` (default `build`), `TARGET` (default `mha2_linux`), `SDL_CFLAGS`, and `SDL_LIBS` (queried for `PKG_DEPS` = `sdl2 SDL2_ttf libavformat libavcodec libswscale libavutil gl`).
- **Header dependency tracking:** `CFLAGS_COMMON` enables `-MMD -MP` (`build/*.d` included via `-include $(RT_DEPS) $(GEN_DEPS)`), and `$(BUILD_DIR)/rt_%.o` includes `recomp/gen/funcs.h`.
- **Fail-fast checks (skipped for `clean` and `lift`):** Build aborts explicitly if `recomp/gen/dispatch.c` is missing (run `make lift`) or `SDL_LIBS` resolves empty.
- **No implicit `make lift` trigger:** `Makefile` intentionally does **not** auto-run `recomp/lift.py` on timestamp changes because the Ubuntu 22.04 portable container (`tools/Containerfile.portable`) compiles pre-lifted `recomp/gen/*.c` without installing `python3-capstone`. Always run `make lift` manually when changing `recomp/lift.py`.

### Portable Linux Release Build (`work/dist/`)
- Built inside an **Ubuntu 22.04** (`glibc 2.35`) builder container defined in `tools/Containerfile.portable` using `podman` (or `docker`), outputting object files to `work/build_portable/` and overriding `BUILD_DIR`, `TARGET`, `SDL_CFLAGS`, and `SDL_LIBS` so local NixOS `./build/` and `./mha2_linux` are untouched:
  ```bash
  make lift                                         # prerequisite if recomp/gen/ isn't generated yet
  bash tools/package_portable_linux.sh              # stages bundle + builds .tar.gz (~5.5 GB)
  bash tools/package_portable_linux.sh --no-archive # stages bundle + runs ABI checks without tarballing
  ```
- **Never bundle NixOS `.so` libraries, `libSDL3.so.0`, or `SDL_DYNAMIC_API` in portable releases:** Nixpkgs binaries/libs pull `GLIBC_2.42`, `GLIBCXX_3.4.32`, `CXXABI_1.3.15`, unversioned `DT_NEEDED: libvulkan.so`, Nixpkgs `libstdc++`, and hardcoded `/nix/store` paths, breaking Ubuntu/Mint/Fedora/SteamOS.
- Keep minimal static FFmpeg (`--prefix=/opt/ffmpeg-min` built with `nasm` SIMD) and verified ABI/font/FFmpeg image-codec checks (`ff_webp_decoder`, `ff_png_decoder`, `ff_bmp_decoder`, `ff_png_encoder`, `ff_image2_demuxer`) inside `tools/package_portable_linux.sh` (max `GLIBC_2.35`, zero `/nix/store` strings, zero hard `DT_NEEDED` on `libvulkan.so`/`libpipewire`/`libpulse`/`libwayland`/`libEGL`/`libgbm`/`libdecor`, bundled `fonts/DejaVu*.ttf`).

---

## 3. Repository & Architecture Map

### Root, Build & Documentation
- `Makefile`, `shell.nix`: Build rules (`-MMD -MP`, `?= ` overrides) and NixOS shell environment (`MHA2_FONT_PATH`, `MHA2_GAME_FONT_PATH`, `python3` + `capstone`, SDL2, FFmpeg, GL, `dejavu_fonts`).
- `play.sh`: Local NixOS launcher forwarding CLI arguments safely (`$@`).
- `LICENSE`, `README.md`, `CHANGELOG.md`, `docs/images/`: MIT License (clean-room code/tooling only), bilingual README (**German `#deutsch` first, clickable English `#english` below**), `v1`–`v5.4` changelog, `1280×640` repo banner (`docs/images/banner.png`), and showcase screenshots (`remaster_showcase_*.png`).

### Static Recompiler (`recomp/`)
- `recomp/lift.py`: Standalone x86-32 PE → C static lifter (uses `capstone` + `pefile`-free PE parser; discovers 8,136 functions & 121 switch tables, emits `recomp/gen/chunk_*.c`, `dispatch.c`, `funcs.h`, `imports.h`). Routes `HOOKABLE_FUNCS` (`call`, `jmp`, `Jcc`, fallthrough) via `g_code_table[]`.
- `recomp/gen/`: Generated C translation units (gitignored; regenerated by `make lift`).

### Native Runtime (`runtime/`)
- `runtime/guest_mem.c`: 4 GiB `PROT_NONE` guest window with 1.5 GiB committed, 24-bin segregated free-list `guest_alloc()` (64-step best-fit + same-bin first-fit fallback), `--strict-mem` NULL page (`runtime_mem_protect_null_page()`).
- `runtime/guest_sync.c`: atomic replacement protocol for the game's MetaSpinLock (`guest_spin_acquire()`, `guest_owner_lock_enter/leave()`).
- `runtime/crash_handler.c`: fatal-signal reporter (guest fault region, registers, `sub_<VA>` call stack, `mha2_crash.log`), `guest_divide_error()`.
- `runtime/input_replay.c`, `runtime/input_replay.h`: `--record`/`--replay` engine (docs/REPLAY.md), quick save/load macros (slot 9, ownership hash `<game dir>/mha2_quicksave.id`), shared guest input entry points (`win32_input_mouse_move/button()`, `win32_input_text()`).
- `runtime/input_bindings.c`, `runtime/input_bindings.h`: rebindable hotkeys/controller buttons (`bind_*`/`pad_*` ini keys, `F1 → 4. CONTROLS`).
- `runtime/text_wrap.c`: SDL_ttf word wrap for the dialog log.
- `runtime/main.c`: CLI argument parsing, `mha2_settings.ini` load ordering, guest Core 0 affinity pinning (`runtime_unpin_current_thread()`), entry point.
- `runtime/cpu.c`, `runtime/cpu.h`: Win32 stdcall/cdecl & COM dispatch, PE loading, thread slots, ALU/`nf_*` flag-free helpers, `div`/`idiv` `#DE` checks, x87 integer conversion (`fpu_to_int()`, `x87_ld_to_int()`), atomic `xchg`/`lock` ops, rotate flags (`alu_rol8/16/32`, `alu_ror8/16/32`), x87 FPU stack emulation (`fpu_save()` FPU re-init).
- `runtime/runtime.h`: Shared `EngineConfig g_cfg`, guest/runtime state declarations, cross-module hooks.
- `runtime/win32_kernel.c`: Win32 threads, events/semaphores/mutexes, handles (locked `ThreadObj`/`SyncObj` refcounting), GL-thread-guarded `ExitProcess`/`TerminateProcess`, case-insensitive VFS/save paths, file I/O, timers.
- `runtime/win32_ddraw.c`: DirectDraw 7 surfaces, hardware-accelerated/emulated blits, RLE sprite hooks (`hook_rle_both`, `hook_rle_common`), `surface_blt()`, palette/color-key conversion, decoupled cursor tracking (1-frame, 2-frame horizontal `mc*`, 2-frame vertical `cuHelp`).
- `runtime/hd_assets.c`, `runtime/hd_assets.h`: `HDS5` 24×24 signature matching (`is_small_prop`, low-opacity fallback), 4K WebP background & sprite index caches (`.hd_index.cache`, `.hd_sprite_index.cache`), strip cell auto-discovery (`hd_sprites_find_strip_cell()`), background preloader thread, sprite room allowlisting (`sprite_allowed_in_active_room()`).
- `runtime/gl_renderer.c`, `runtime/gl_renderer.h`: OpenGL 3.3 Core renderer, 4K HD canvas FBO, procedural 3D brushed-gold Main Menu `"REMASTERED"` badge overlay, shaders (Bicubic CAS, Lanczos, Edge Smooth, FSR1, CRT, Bloom, Color Grading), widescreen `16:10`/`16:9` HUD/dialog split composition (`uDialogBar`, `g_top_bar_has_buttons`, `gl_renderer_clamp_gamepad_cursor()`), settings persistence (`gl_config_load/save()`), vector typography (`SDL2_ttf`), GPU post-FX & compare split-screen (`F2`), decoupled cursor, hotspot overlay, `gl_renderer_is_gl_thread()`.
- `runtime/game_patches.c`: Original 2005 game bugfixes (`game_patches_install()` via `install_patch()`: spinlock CPU burn, savegame EOF hang, loading sleep throttle, null derefs, handle leaks), hot-path native replacements, read-only Main Menu detector (`game_is_main_menu_active()`), and exact read-only scene-graph hotspot collector (`game_collect_room_hotspots()`).
- `runtime/win32_user_gdi.c`: SDL2 window/event loop, focus-loss QoL reset (`clear_held_qol_keys()`), input state, atomic audio hotkey updates (`F7`/`F8`/`F9`), GDI font/TextOutA hooking (Windows-1252 → UTF-8, `find_system_ttf()`), gamepad cursor emulation.
- `runtime/win32_dinput.c`: DirectInput keyboard/mouse COM stubs (`com_dinput_dispatch`).
- `runtime/win32_dsound.c`: DirectSound mixing + SDL2 audio callback + reverb/EQ DSP (reads shared `g_cfg` audio fields atomically).
- `runtime/gl_shaders.c` (GLSL sources), `runtime/gl_menu.c` (`F1` menu, 4 tabs incl. `4. CONTROLS` key capture), `runtime/gl_dialog_log.c` (`Ctrl+L` log, `gl_dialog_log_shutdown()`), `runtime/gl_internal.h` (private renderer interface — never include outside `runtime/gl_*.c`).
- `runtime/win32_avi.c`: FFmpeg-backed DirectShow (`IGraphBuilder`/`IMediaControl`/`IMediaEventEx`/`IVideoWindow`/`IBasicVideo`) & `AVIFIL32`/`DrawDibDraw` video playback with `Pause`/`Run` elapsed-clock preservation (`paused_elapsed`), bounded codec error/flush loops (`ff_decode_next_frame()`), guest bounds checks, and automatic `hd_assets/videos/*.mp4` 4K override.

### Tooling (`tools/`)
- `tools/Containerfile.portable`, `tools/package_portable_linux.sh`: Self-contained Ubuntu 22.04 portable Linux builder & packager.
- `tools/extract_game.py`, `tools/extract_assets.py`: Legal-copy installer/disc extractor (`Setup.EXE`, Mode-1/2352 `.bin`/`.iso`) and `mha2.dat` VDrv/`_img`/IT3 unpacker.
- `tools/upscale_ai.py`, `tools/upscale_transformer.py`, `tools/upscale_alpha_sprites.py`: Real-ESRGAN Vulkan batch 4× builder, faithful Transformer/SPAN (`4x-ClearRealityV1`) 4× upscaler (`--apply-backgrounds`, `--apply-sprites`, `--apply-jpeg-strips`), and composited IT3 `tbl1+tbl2` RGBA upscaler.
- `tools/remaster_bg_diffusion.py`: Experimental SDXL + ControlNet-Tile 4K background remaster tool.
- `tools/upscale_videos.py`, `tools/remaster_videos_4k.py`: Cutscene upscalers (`game_files/*.avi` → `hd_assets/videos/*.mp4` via Real-ESRGAN Video or streaming RTX `4x-ClearRealityV1` pipeline).
- `tools/polish_cursors.py`, `tools/compress_hd_lossless.py`: SDF vector-clean cursor polisher (`cu*` & `mc*`, reading `.bak_raw` backups for idempotency) and parallel bit-exact lossless WebP recompressor.
- `tools/install_desktop.sh`: Local NixOS launcher icon (`walkS_f000.webp`) & `.desktop` installer.
- `tools/regress.sh` (`make regress`): headless sandboxed replay regression runner (fresh `--game-dir` sandbox per script, local RMSE baselines in `work/regress_baseline/`, `--strict-mem`, `#! threshold` per-shot overrides, `MHA2_BIN` override).

### Tests & Docs (`tests/`, `docs/`)
- `tests/test_*.c` (`make test`): guest sync, guest memory & crash reports, x86 semantics (div/idiv vs `__int128`, `FIST` vs host x87, `nf_*` vs `alu_*`), input bindings, text wrap.
- `tests/replay/*.rec`: replay scripts (boot, new game, overlays, quick save, controls, controller, overlay text).
- `docs/REPLAY.md` (script syntax), `docs/INSTALL.md` (split release), `docs/STEAM_DECK.md`, `docs/PLAN.md` (engineering plan/status).

---

## 4. Key Technical Invariants & Pitfalls (Do Not Break)

1. **Animated JPEG Filmstrip Sprites (`img_type == 0`, `nf > 1`):**
   - Never upscale multi-frame JPEG vertical filmstrips as a single image (`objKerzen`, `gsNormal`, `objBrunnen`, etc.) — neural models bleed adjacent frames into dark horizontal trenches that blink on animation boundaries.
   - Always split and upscale per-frame with edge-replicate padding into `_f000.webp`..`_f{N-1}.webp` (`tools/upscale_transformer.py --apply-jpeg-strips`), and keep `--apply-backgrounds` skipping `nf > 1`.

2. **Split-Background Outdoor Rooms (`l/011`, `l/014`, `l/022`, `l/023`, `l/026`, `l/027`, `l/028`, `l/036`):**
   - In `sprite_allowed_in_active_room()` (`runtime/hd_assets.c`), the allowlist for `/objBgO`, `/objBgU`, `/mskBg.`, and `/objWolken` **must stay before** `if (g_spr_room_prefix_len > 0)`.
   - `maybe_lock_split_room_from_sprite()` (`runtime/win32_ddraw.c`) may lock room state only from `/objBgO`, `/objBgU`, or `/mskBg.` — **never** from shared desert clouds `/objWolken`.
   - Keep `lookup_rle_both_sprite()` max width `>= 2048` (`l/023/img/mskBg` is `1600×212`).

3. **HD Asset Caches, `HDS5` Matching & Runtime Toggles:**
   - Adding, replacing, or deleting files in `hd_assets/` requires invalidating/rebuilding `.hd_index.cache` and/or `.hd_sprite_index.cache`.
   - Both `g_cfg.hd_assets_enabled` (`F6`) and `g_cfg.hd_sprites_enabled` (`--no-hd-sprites`) must gate HD sprite lookups (`surface_blt`, `hook_rle_both`, `hook_rle_common`, `upload_cursor_sprite`) and `use_hd_canvas` (`gl_draw_frame_common`).
   - In `HDS5` (`runtime/hd_assets.c`), keep `is_small_prop = (sw <= 64 || sh <= 64)` (`||`, not `&&`) and low-opacity probe handling (`shared_cells >= 1` when opaque cells `< 2`).
   - Decoupled cursor detection (`runtime/win32_ddraw.c`) must support 1-frame, 2-frame horizontal (`mc*`), and 2-frame vertical (`cuHelp`) cursor strips (`frame_sy0` offset handling + `256×256` strip surface bound while capping frame dims `<= 128×128`).

4. **Guest Core 0 Pinning & Win32 Handle Refcounting:**
   - The original 2005 binary uses non-atomic refcounts across main, loading, and processing threads; guest threads must stay pinned to Core 0 while native worker threads (audio callback, HD pre-decoder) call `runtime_unpin_current_thread()`.
   - When accessing `WinHandle` payloads (`SyncObj` or `ThreadObj`) in `runtime/win32_kernel.c`, always lock `g_hdl_mtx`, extract the pointer strictly matching `htype`, atomically increment `refcount` **before** unlocking `g_hdl_mtx`, and unref afterward.

5. **COM Vtable Slot Accuracy & Lifter Quirks:**
   - Every DirectShow / DirectDraw / DirectSound / DirectInput COM slot must match its exact Windows vtable index (`byte_offset / 4`) and stdcall parameter count (including `this`); wrong counts corrupt guest `ESP`. Check lifted call sites in `recomp/gen/` when implementing slots.
   - Capstone returns 2 operands `[ST(0), ST(i)]` for `fxch D9 C8+i` even though Intel syntax displays `fxch st(i)`; verify any new `recomp/lift.py` mnemonic handler directly against Capstone's `ins.operands` output and inspect regenerated `recomp/gen/`.
   - Keep `fpu_save()` re-initializing FPU state (`CW=0x037F`, `SW=0`, `TOP=0`), `fsin`/`fcos`/`fsincos`/`fptan` updating `C2`, `fprem` (`fmodl`) vs `fprem1` (`remquol`), 16-bit `pop [esp+disp]` ESP-first ordering, and `HOOKABLE_FUNCS` routing across `call`, `jmp`, `Jcc`, and fallthrough edges.

6. **Hotspot Highlights & Widescreen Dialogs:**
   - `Space`/`H` hotspot highlights must come from `game_collect_room_hotspots()` in `runtime/game_patches.c`, not sprite-name guesses.
   - In `16:10`/`16:9` widescreen modes, bottom dialog choices reach `y=525..536` (`update_hud_bar_masks()`, `uDialogBar`) — keep dialog bar detection, shader UV remapping (including `FS_POST` `F2` compare split-screen), `g_top_bar_has_buttons` corner routing, and pointer/gamepad coordinate mapping synchronized.

7. **GL Thread Ownership & Resource Teardown:**
   - `gl_renderer_shutdown()` and `SDL_Quit()` (including from `ExitProcess`/`TerminateProcess` in `runtime/win32_kernel.c`) must only run on the GL thread (`gl_renderer_is_gl_thread()`).
   - Any new GL texture/FBO/program added in `runtime/gl_renderer.c` (such as `g_dialog_log_tex` via `gl_dialog_log_shutdown()` and `g_remastered_badge_tex`) must be deleted inside `gl_renderer_shutdown()`.
   - HD backgrounds live in the `g_bg_cache[]` GPU texture cache (`bg_cache_acquire()`); `g_bg_hd_tex`/`g_bg_wide_tex` only alias the active entry. Never go back to a single re-uploaded texture: screens such as the `m/012` picture minigame blit from two backgrounds per frame.

8. **Lock-Free Audio Config Atomics:**
   - `runtime/win32_dsound.c` reads `g_cfg.master_volume`, `g_cfg.muted`, `g_cfg.audio_reverb`, and `g_cfg.audio_eq` on the SDL audio callback thread using `__atomic_load` / `__atomic_load_n`.
   - Any runtime writer (`menu_apply_delta` in `runtime/gl_renderer.c`, `F7`/`F8`/`F9` in `runtime/win32_user_gdi.c`) must update `int` fields with `__atomic_store_n` and `float master_volume` with `__atomic_store`.

9. **Input Focus Loss & Gamepad Bounds:**
   - `SDL_WINDOWEVENT_FOCUS_LOST` (`runtime/win32_user_gdi.c`) must call `clear_held_qol_keys()` so Shift speed-up or Space/`H` hotspot highlights never stick across Alt-Tab.
   - Gamepad cursor clamping must route through `gl_renderer_clamp_gamepad_cursor()` so the cursor cannot get lost in cropped widescreen bands.

10. **Font Resolution & Environment Variables:**
    - `MHA2_FONT_PATH` overrides the UI/GDI font (`DejaVuSans.ttf`); `MHA2_GAME_FONT_PATH` overrides the bold in-game vector font (`DejaVuSans-Bold.ttf`).
    - Both fallback to relative `fonts/DejaVu*.ttf` (used in portable releases) and system NixOS / Debian / Ubuntu / Fedora (`dejavu-sans-fonts`) / Arch paths (`init_osd_font()` and `find_system_ttf()`).

---

11. **Lifter Rewrites & Flag Liveness (`recomp/lift.py`):**
    - `ATOMIC_COUNTER_GLOBALS` (`0x0051b3b8`) must keep raising on unrecognized accesses; every lifted `mov/inc|dec/mov` is one atomic RMW and every other store a `guest_spin_acquire()`. Native `Lock`/`TryLock` patches must use `guest_owner_lock_enter/leave()` (same protocol).
    - `flag_effects()` kill sets must mirror exactly what each `alu_*` helper writes *unconditionally* (shift/rotate by CL or 0 and `rep cmps/scas` are may-defs; `inc/dec` keep CF; shifts write OF only for count 1). After touching ALU helpers or flag handling, re-validate with `MHA2_LIFT_FLAGCHECK=1 make lift` + `MHA2_BIN=… tools/regress.sh --strict-mem`; `MHA2_LIFT_NOFLAGELIM=1` disables elimination.
    - `GUEST_BUGFIX_INSERTS` code runs before the instruction (after its label) and counts as an all-flags-live point.

12. **Input Injection (`runtime/input_replay.c`):**
    - Never `SDL_PushEvent` an `SDL_TEXTINPUT` (sdl2-compat crashes); use `win32_input_text()`. Injected key/pad events carry the real window ID and are recognized via the signature FIFO.
    - Injected mouse moves must travel in ≤48 px steps (`inject_move_to()`): the game drops large single DirectInput deltas. The game only accepts clicks on an already-hovered target (`click`/`clickspot` hover 250 ms).
    - Macros (quick save/load) must never `_exit`; failures go through `macro_end()`. Quick save may only write slot 9 if it is empty or hash-matches `mha2_quicksave.id`.

13. **Hotkeys (`runtime/input_bindings.c`):** all remaster hotkeys go through `bindings_match_key()`/`perform_action()`; Escape, Alt+F4, Alt+Enter and pad A/B/Start stay fixed. Press chords require exact Ctrl/Alt so plain letters typed into savegame names never trigger actions. UI hints use `bindings_describe()`.

## 5. Common Development Workflows

### Adding a New Game Patch / Hook
1. Identify the target guest `.text` VA and add it (plus a concise comment) to `HOOKABLE_FUNCS` in `recomp/lift.py`.
2. Regenerate `recomp/gen/` so `call`, `jmp`, `Jcc`, and fallthrough sites dispatch through `g_code_table[]`:
   ```bash
   nix-shell --run "make lift"
   ```
3. Implement the clean-room handler in `runtime/game_patches.c` (ensuring exact `stdcall`/`__fastcall`/cdecl stack cleanup) and register it via `install_patch(va, handler)` inside `game_patches_install()`.
4. Rebuild (`nix-shell --run "make -j$(nproc)"`) and verify zero warnings.

### Testing
```bash
nix-shell --run "make test"                    # unit & stress tests (no game files needed)
nix-shell --run "tools/regress.sh --strict-mem" # replay suite, headless & sandboxed (needs game_files/ + hd_assets/)
nix-shell --run "tools/regress.sh --update-baseline"  # after an intentional visual change
```
Never point replays at the real `game_files/` save folder; `tools/regress.sh` sandboxes automatically.

### Refreshing HD Caches or Portable Release Bundles
- If modifying files under `hd_assets/`, delete `.hd_index.cache` and/or `.hd_sprite_index.cache` so the runtime rebuilds the indexes cleanly.
- To ship or test a cross-distro release after code edits, run `bash tools/package_portable_linux.sh --split` (runtime + HD asset pack, no game files; `--no-archive` for fast ABI/font checks).

---

## 6. Clean-Room IP Policy, Coding, Docs & Git Conventions

- **Strict clean-room boundary (public GitHub repo):**
  - Never commit copyrighted game binaries/assets (`game_files/`, `hd_assets/`, `extracted_assets/`, `*.exe`, `*.dat`, `*.avi`, `*.sav`, disc images), Ghidra project databases/journals, raw disassembly dumps, or generated `recomp/gen/*.c` files/snippets.
  - Keep everything committed strictly clean-room: generic x86 lifter logic, Linux compatibility runtime (`runtime/`), extractor/upscaler/packaging scripts (`tools/`), original docs/screenshots, and minimal hook VAs in `HOOKABLE_FUNCS` / `game_patches.c`.
- **No generated/local commits:** Never commit files matching `.gitignore` (`work/`, `recomp/gen/`, `build/`, `mha2_linux`, `mha2_settings.ini`, `tools/models/`, `backups/`, `screenshots/`).
- **Compiler strictness:** All `runtime/*.c` files must compile cleanly with **zero warnings** under `-Wall -Wextra`.
- **Code hygiene:** Avoid duplicating logic; keep helpers small and single-purpose; preserve existing docstrings and unrelated comments.
- **Docs synchronization:** Keep `README.md` bilingual (German `#deutsch` first, clickable English `#english` second) and update `CHANGELOG.md` when adding user-facing features or fixes.
- **Git commit identity:** Use your configured Git identity (no hardcoded local names or placeholder emails):
  ```bash
  git commit -m "..."
  ```
- **Verification before claiming done:** Rebuild after edits (`nix-shell --run "make -j$(nproc)"`, plus `make lift` if `recomp/lift.py` changed) and inspect generated output, runtime logs, or portable ABI checks directly.
