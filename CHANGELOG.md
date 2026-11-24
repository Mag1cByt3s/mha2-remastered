# Changelog

All notable changes to the **Moorhuhn Adventure 2: Der Fluch des Goldes — Native Linux x86_64 Static Recompilation & Remaster Engine** are documented in this file.

---

## [v5.4.1 — Picture-Difference Minigame Performance] (2026-11-24)

### Fixes
- **Lag & 100% CPU in the Dining-Room Picture Minigame (`m/012`)**: the game blits from two different background surfaces every frame (`objBg1`/`objBg2`), but the renderer kept only one HD background texture — every switch re-decoded a 3200×1800 WebP and rebuilt its mipmaps on the render thread (4 fps in the close-up, zoom animation crawling). HD backgrounds now live in a 4-entry GPU texture cache (`runtime/gl_renderer.c`, LRU, includes `*_wide` siblings); switching between cached backgrounds is a texture bind (≈2 decodes per visit instead of ~3 per second).

### Testing
- Replay `log` lines now include the presented frame rate (`fps=`) for performance checks (`docs/REPLAY.md`).

---

## [v5.4 — Correctness Hardening, Replay Test Harness, Quick Save, Rebindable Controls & Split Release] (2026-11-24)

### Original 2005 Game Bugfixes (`recomp/lift.py`, `runtime/game_patches.c`, `runtime/guest_sync.c`)
- **Inlined Non-Atomic Spinlock Fixed Everywhere**: the v5.2 fix only patched `MetaSpinLock::Lock` (`0x004443f0`); MSVC had inlined the same `mov/inc/mov` lock + forced 200 ms lock-steal into six more `ObjMutex` methods. New lifter peephole `ATOMIC_COUNTER_GLOBALS` turns all 52 counter read-modify-writes into atomic RMWs and the 9 forced steals into a blocking acquire; `TryLock` again holds the meta-lock around its check-and-set (`tests/test_guest_sync.c`).
- **Ambient Variant Pick Crash (`0x0042a820`)**: picking a random variant before any of its resources finished loading indexed `arr[-1]` (virtual call through the heap header + weight decrement inside it). New `GUEST_BUGFIX_INSERTS` lifter hook takes the function's own empty-pool exit. Found by the new replay harness under `--strict-mem`.
- **Hotspot Highlighter (`Space`/`H`) Dead in Most Rooms**: rooms/minigames with custom logic are subclasses with their own vtables (11 Location + 5 Minigame) and were rejected by an exact vtable match; the `(scene+0x6c)->+0x80` "dialog" guard read `2` in ordinary idle rooms. Now inheritance-based, with markers suppressed only while dialog choices are shown.

### Static Recompiler & x86 Semantics (`recomp/lift.py`, `runtime/cpu.h`)
- **Dead Flag Elimination**: per-function flag liveness removes 27,698 of 68,740 flag computations (−40%, `.text` −6%); validated by a poison-check lift mode (`MHA2_LIFT_FLAGCHECK=1`) over the full replay suite plus a negative control.
- **Function Discovery Fix**: data-pointer scan missed functions placed after `ret imm16`; recovers `exception::what()` (`0x004a6749`) and an SEH filter (8,134 → 8,136 functions).
- **x87 & Integer Exactness**: `FIST`/`FISTP` m16/m32 store x87 "integer indefinite" instead of wrapped values (bit-exact vs host x87 over 800k cases); `div`/`idiv` raise a reported `#DE` on zero divisor / quotient overflow instead of a host trap or silent truncation; `pushfd` sets IF.

### Runtime Robustness & Diagnostics (`runtime/guest_mem.c`, `runtime/crash_handler.c`)
- **Guarded 4 GiB Guest Window**: whole 32-bit window reserved `PROT_NONE`, only the used 1.5 GiB committed — wild guest pointers fault instead of corrupting host memory.
- **Crash Reports**: `SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT` on an alternate stack print the guest fault address & region, guest registers and a symbolized guest call stack (`sub_<VA>` frames via `-rdynamic`) to stderr and `mha2_crash.log`; guest C++ exceptions report type, `what()` and throw site.
- **`--strict-mem`** debug flag protects the guest NULL page; explicit 32 MiB host stacks for guest threads.

### New Features
- **Quick Save / Quick Load (`Ctrl+S` / `Ctrl+O`, D-pad Up/Down)**: macros drive the game's own save screen into slot 9 (original save code & thumbnails), never overwrite a manual save in that slot, and are refused during cutscenes, dialogs and menus.
- **Rebindable Controls (`F1` → `4. CONTROLS`)**: 18 actions with two key chords each, controller buttons via `pad_*` in `mha2_settings.ini`, conflict handling, reset to defaults; UI hints follow the live bindings. Steam Deck guide in `docs/STEAM_DECK.md`.
- **Overlay Text Size (`F1` → `3. ASSETS & TEXT`)**: 100–200% for the dialog log (now word-wrapped), hotspot labels and toasts.
- **Split Linux Release (`tools/package_portable_linux.sh --split`)**: 12 MB runtime archive without game files + separate 5 GB HD asset tar + `SHA256SUMS`; shared data directory `~/.local/share/mha2-remastered/` (`docs/INSTALL.md`).

### Testing & Code Structure
- **Input Record/Replay (`--record` / `--replay`) & `tools/regress.sh` (`make regress`)**: sync-point-based scripts (`wait video|menu|room|interactive`), guest-coordinate mouse injection, `clickspot <label>`, controller injection, screenshots/frame hashes/assertions; headless (SDL offscreen) sandboxed runs with local screenshot baselines. Scripts: `boot_menu`, `smoke_newgame`, `overlays`, `quicksave`, `controls`, `controller`, `overlay_text` (`docs/REPLAY.md`).
- **Unit Tests (`make test`)**: guest sync, guest memory & crash reports, x86 semantics (div/idiv, x87 vs hardware, flag-free helpers), input bindings, text wrap.
- **`gl_renderer.c` Split**: 5,205 → 3,242 lines (`gl_shaders.c`, `gl_menu.c`, `gl_dialog_log.c`, private `gl_internal.h`); guest memory/heap moved from `cpu.c` to `guest_mem.c`.

---

## [v5.3 — Engine Accuracy Overhaul, Split-Room & Filmstrip Fixes, Audit Hardening & Portable Builder] (2026-11-23)

### Core Engine, Widescreen UX & 4K Scene Accuracy (Post-`v5.2`)
- **24-Bin Segregated Free Lists & 1.42 GB Guest Heap (`runtime/cpu.c`, `runtime/cpu.h`)**: Upgraded guest heap allocator to 24 size-class bins with O(1) bitmask jump + 1.42 GB virtual address space (`#10` also extends exact-bin scans past 64 blocks on first-fit fallback).
- **Exact Read-Only Scene-Graph Hotspot Collector (`runtime/game_patches.c`, `runtime/gl_renderer.c`)**: Replaced sprite-name heuristics for `Space`/`H` hotspot highlights with exact read-only traversal of live room object hitboxes (`game_collect_room_hotspots()`).
- **Widescreen Dialog Bar Detection & Left-Aligned 4K Choices (`runtime/gl_renderer.c`)**: Added pixel-based detection of bottom dialog choices spanning `y=500..599` (fixing clipped `y=525..536` lines in 16:9/16:10) and left-aligned 4K vector dialog choices to match original layout.
- **Animated JPEG Filmstrip Seam Fix (`tools/upscale_transformer.py`, `runtime/hd_assets.c`, `runtime/win32_ddraw.c`)**: Split and 4×-upscaled all 50 multi-frame JPEG strips (462 frames) individually with 16px edge-replicate padding (`--apply-jpeg-strips`), eliminating blinking dark horizontal seams, and added `hd_sprites_find_strip_cell()` for clipped-first-blit auto-discovery.
- **Split-Background Outdoor Rooms Fix (`runtime/hd_assets.c`, `runtime/win32_ddraw.c`)**: Fixed 1× blurry fallback across all 8 split-background rooms (`l/011`, `l/014`, `l/022`, `l/023`, `l/026`, `l/027`, `l/028`, `l/036`) by prioritizing `/objBgO`/`/objBgU`/`/mskBg.`/`/objWolken` allowlisting, adding `maybe_lock_split_room_from_sprite()`, and raising `lookup_rle_both_sprite()` max cell width/height to 2048 (`l/023/img/mskBg` `1600×212`).
- **Clean-Room Procedural 3D Gold `"REMASTERED"` Main Menu Title (`runtime/gl_renderer.c`, `runtime/game_patches.c`)**: Added `game_is_main_menu_active()` and a pure-C 2D EDT + 3D beveled brushed champagne-gold `"REMASTERED"` overlay rendered procedurally via `SDL2_ttf` in both 4K HD canvas and classic fallback modes with full Bloom, Post-FX, and `F2` split-screen support (zero copyrighted assets in git).

### Static Recompiler & x87 FPU (`recomp/lift.py`, `runtime/cpu.h`, `runtime/cpu.c`)
- **Fixed `fxch st(2..7)` Lifting Bug**: Decoded Capstone's 2-operand `fxch D9 C8+i` encoding (`[ST(0), ST(i)]`) accurately instead of defaulting to `st(1)` across 15 live instructions in `mha2.exe`.
- **Completed x87 FPU & Instruction Semantics**: Added FPU state re-initialization in `fpu_save()` (`fnsave`), cleared/set `C2` with `2^63` range guards in `fsin`/`fcos`/`fsincos`/`fptan`, separated `fprem` (`fmodl`) from IEEE 754 `fprem1` (`remquol`) with `C0`/`C3`/`C1` quotient bits, fixed 8/16-bit `alu_rol`/`alu_ror` count masking and flag updates, fixed 16-bit and immediate `bt`/`bts`/`btr`/`btc`, fixed 16-bit `pop [esp+disp]` ordering, made memory `xchg`/`lock xchg` and 8/16/32-bit `lock inc`/`dec`/`add` atomic via `__atomic_*`, and routed `Jcc` and fallthrough transitions through `g_code_table` for `HOOKABLE_FUNCS`.

### Runtime Stability, Threading & Playback (`runtime/`)
- **DirectShow COM Vtable & Video Robustness (`runtime/win32_avi.c`)**: Fixed `IVideoWindow::SetWindowPosition` (`slot 39`, `+0x9C`) / `GetWindowPosition` (`slot 40`, `+0xA0`) stack corruption (`stdcall` arg counts `37..43`), added missing `IBasicVideo` getters (`slot 15/19` and neighbors), preserved playback elapsed ticks across `Pause` → `Run`, bounded `ff_decode_next_frame()` against codec/flush loops, and validated `DrawDibDraw` fallback dimensions and guest memory bounds.
- **Win32 Handle & GL Shutdown Thread Safety (`runtime/win32_kernel.c`, `runtime/gl_renderer.c`, `runtime/win32_user_gdi.c`)**: Added `ThreadObj` refcounting under `g_hdl_mtx` in `WaitForSingleObject`, `ResumeThread`, and `GetExitCodeThread`; guarded GL/SDL teardown to the GL context thread in `gl_renderer_shutdown()` and `ExitProcess`/`TerminateProcess`; freed `g_dialog_log_tex` on shutdown; and switched live audio config writes (`master_volume`, `muted`, `audio_reverb`, `audio_eq`) to atomic stores.
- **100% HD Sprite Coverage (`runtime/hd_assets.c`, `runtime/win32_ddraw.c`)**: Expanded `HDS5` `24×24` matching (`is_small_prop` via `sw <= 64 || sh <= 64`, `max_cell_da` tuning, and low-opacity probe handling) recovering ~140 previously missed IT3 props/characters; fixed runtime `F6` toggle and `--no-hd-sprites` gating across all blitters & `g_hd_canvas`; and added 2-frame vertical cursor strip support (`cuHelp`) for decoupled rendering.
- **Input, Widescreen & Tooling UX (`runtime/gl_renderer.c`, `runtime/win32_user_gdi.c`, `runtime/main.c`)**: Fixed stuck `Shift` / `Space` / `H` states on window focus loss, aligned `uDialogBar` in `F2` widescreen split-screen comparison, gated widescreen top-corner routing via `g_top_bar_has_buttons`, clamped gamepad cursor bounds to the visible widescreen playfield, and updated CLI `--help` docs.

### Portable Linux Packaging, Licensing & Build System (`tools/`, `Makefile`, `shell.nix`, `docs/`)
- **Cross-Distro Portable Linux Builder (`tools/Containerfile.portable`, `tools/package_portable_linux.sh`)**: Added a reproducible Ubuntu 22.04 (`glibc 2.35`) builder container compiling minimal static SIMD FFmpeg (including `image2` demuxer, `webp`/`png`/`bmp` image decoders, and `png` screenshot encoder) and portable SDL2 (`libSDL2-2.0.so.0`), removing `GLIBC_2.42`/`GLIBCXX_3.4.32`/unversioned `libvulkan.so`/`SDL_DYNAMIC_API` NixOS leaks, bundling `fonts/DejaVu*.ttf` (with relative `./fonts/` + Fedora font fallback), and enforcing automated ABI and FFmpeg codec symbol verification.
- **Build, Tooling & Release Docs**: Added `-MMD -MP` header dependency tracking, explicit `pkg-config`/`recomp/gen/dispatch.c` checks, and `?= ` overrides in `Makefile`; exported `MHA2_GAME_FONT_PATH` in `shell.nix`; fixed argument quoting, cursor batching (`mc*`), extension cleanup, temp file error cleanup, and CPU/subprocess safety across `play.sh` and `tools/*.py`/`*.sh`; added `docs/images/banner.png` (`1280×640` Remastered banner) and MIT [LICENSE](LICENSE).

---

## [v5.2 — Original Game Bugfixes, Multi-Core Threading & Runtime Hardening] (2026-11-23)

### Original 2005 Game Bugfixes & Optimizations (`runtime/game_patches.c`, `runtime/win32_kernel.c`, `recomp/lift.py`)
- **Fixed Non-Atomic Spinlock & Forced Lock-Steal (`0x004443f0`, `0x00444230`)**: Replaced the original game's non-atomic `DAT_0051b3b8++` spinlock (which forcibly stole the lock after 200ms while another thread was still inside the critical section) with true atomic CAS (`__atomic_compare_exchange_n`) and zero lock-steal.
- **Fixed Infinite Loop on Truncated/Corrupt Savegames (`0x0044a200`)**: Guarded `ReadFile` EOF (`bytesRead == 0`) and `GetFileSize` overflow so corrupted/truncated save files fail cleanly instead of hanging forever or wrapping `operator_new(size + 0x20)`.
- **Eliminated Loading Screen 50–250ms Artificial Stall (`0x00453cd0`)**: Clamped the original loading loop's `Sleep(250 - p1 - p2)` stall to `<= 4ms` for noticeably snappier room transitions.
- **Native Hardware `ceil()` & `__ftol()` for Scaled RLE Blitters (`0x004a7a5e`, `0x004a71cc`)**: Replaced the original CRT's slow emulated x87 FPU control-word-flipping `ceil`/`__ftol` routines called per-span inside all 8 scaled RLE blitters with direct native hardware instructions.
- **Fixed Room Global Object Loader Null Dereference (`0x00474fd0`)**: Guarded the two `ESI` writes (`0x0047530e`, `0x00475317`) that occurred before `test esi, esi; je`.
- **Fixed Startup `FindFirstFileA` Handle Leak (`0x00447820`)**: Closed the `data\g` directory probe `DIR*` immediately when matched instead of leaking it for the lifetime of the process.

### Native Linux Runtime Stability & Performance (`runtime/*.c`)
- **Multi-Core Unpinning for Native Worker Threads (`runtime/main.c`, `runtime/hd_assets.c`, `runtime/win32_dsound.c`)**: Saved the host's multi-core affinity mask before pinning guest threads to Core 0, then unpinned the background HD WebP pre-decoder thread and SDL audio DSP thread across all CPU cores (`0..N-1`) for zero main-thread contention.
- **Explicit Free-List Guest Heap Allocator (`runtime/cpu.c`)**: Upgraded `guest_alloc` from an `O(N_all)` full-heap scan to an explicit doubly-linked free list (`O(N_free)`), added mutex locking to `guest_alloc_size`, added size-overflow/OOM guards, and enforced `guest_alloc_static` bounds.
- **Fixed DirectSound Use-After-Free & Audio Thread Races (`runtime/win32_dsound.c`)**: Added a refcounted `DSoundData` PCM pool so releasing an original sound buffer never frees `data_va` out from under a playing `DuplicateSoundBuffer` clone; added atomic loads for `g_cfg` audio settings and fixed `IDirectSound::Release` COM leak.
- **Thread Slot Recycling & Handle Exhaustion Safety (`runtime/cpu.c`, `runtime/win32_kernel.c`)**: Refcounted `ThreadObj` and recycled guest thread slots via a 36-slot bitmask (eliminating stack OOB after 40 threads and `ThreadObj` leaks); guarded all `alloc_handle() == 0` call sites and fixed `DeleteCriticalSection`/`FindClose` mutex races.
- **DirectDraw & DirectShow Safety (`runtime/win32_ddraw.c`, `runtime/win32_avi.c`)**: Fixed primary surface backbuffer leak on `Release`, locked `QueryInterface`/`AddRef` refcounts, guarded `alloc_surface`/`CreateSurface`/`ff_open` OOM paths, freed DirectShow COM wrapper objects on `Release`, and eliminated the 25Hz black-frame intro/cutscene video flicker (`ddraw_present_surface` no longer overwrites active `AVIFile`/`DrawDibDraw` frames with the empty DirectDraw backbuffer) while making 4K `DrawDibDraw` zero-copy.
- **4K SDF Cursor Matching & `cuExit`/`cuHelp` Polish (`runtime/hd_assets.c`, `tools/polish_cursors.py`)**: Relaxed single-cell 2×2 peak outlier caps (`max_cell_da`, `max_cell_drgb`) for `<=64×64` sprites in `HDS5` so all 4K SDF cursors (`mcArrow`, `mcLook`, `mcTake`, `mcTalk`, `mcUse`, `cuExit`, `cuHelp`) match reliably instead of falling back to blurry 1× sprites, and polished `cuExit` and `cuHelp` with vector-clean SDF contours.
- **Native 16:9 & 16:10 Widescreen Main Menu (`runtime/gl_renderer.c`, `runtime/hd_assets.c`, `hd_assets/x/001/img/objBg_wide.webp`)**: Added seamless 4K `4268×2400` widescreen extended wings for the Main Menu (`x/001`), keeping the exact untouched 4K `3200×2400` center aligned 1:1 with all interactive menu buttons while filling 16:9 and 16:10 monitors edge-to-edge with matching color grading (and respecting `F12` 4:3 / 16:10 / 16:9 modes).
- **Input & UX Polish (`runtime/win32_user_gdi.c`, `runtime/gl_renderer.c`)**: Coalesced consecutive `WM_MOUSEMOVE` events in the Win32 message queue (eliminating lag on 500–1000Hz mice against the original game's 9-msg/frame cap), and rebound Dialog & Clue History Log from bare `L` to **`Ctrl+L`** so players can freely type `'l'` in savegame names.

---

## [v5.1 — 4K ClearRealityV1 Overhaul, Audio Remastering & Portable Build] (2026-11-22)

### Neural 4K Asset Pipeline (`4x-ClearRealityV1` Full Remaster)
- **Complete 100% Faithful Super-Resolution Overhaul** (`tools/upscale_transformer.py`):
  - Replaced legacy 2021 ESRGAN (`4x_NMKD-Siax_200k` / `ultrasharp-4x`) weights with modern **`4x-ClearRealityV1`** (SPAN) neural super-resolution — eliminating 2005 JPEG crackle, carpet moiré, and jagged staircases while preserving 100% of original art, crests, cobwebs, and shading.
  - Remastered **all 348** JPEG-backed backgrounds, scrolling panoramas, menus, and screens (`hd_assets/`).
  - Remastered **all 8,098** IT3 sprites (`≥16px`) using RGB-dilation + separate neural alpha upscaling + 1.0px 4K Gaussian silhouette AA for non-`tbl2` props.
- **True 4K (`3200×2400`) FMV Cutscenes** (`tools/remaster_videos_4k.py`):
  - Added streaming FP16 GPU cutscene remastering (`ffmpeg` → RTX GPU `4x-ClearRealityV1` → `libx264` CRF 17 MP4 with zero temporary disk frames).
  - Remastered all 4,721 frames of `mha2il`, `mha2ol`, and `mha2sl` to native `3200×2400` MP4 (with `mha2ih`/`mha2oh`/`mha2sh` symlinked to 4K masters).
- **Vector-Clean SDF Cursors** (`tools/polish_cursors.py`):
  - Replaced bumpy 2005 `tbl2` outer cursor fringes across all 10 cursor frames (`mcArrow`, `mcLook`, `mcTake`, `mcTalk`, `mcUse`) with smooth signed-distance-field (SDF) anti-aliased contours and soft drop shadows.
- **100% Bit-Exact Lossless WebP Optimization** (`tools/compress_hd_lossless.py`, `runtime/hd_assets.c`):
  - Converted all 8,644 HD PNGs to bit-exact lossless WebP (`lossless=True, exact=True`, `max_pixel_diff=0`, preserving dilated RGB inside transparent pixels) — saving **2.16 GB (38.2%)** with zero quality loss.
- **Zero-Stutter Preloading & QoL Suite** (`runtime/hd_assets.c`, `runtime/gl_renderer.c`, `runtime/win32_ddraw.c`):
  - Added a background room-sprite pre-decoder worker thread + expanded GPU sprite LRU cache (`256 → 512` slots).
  - Added **Dialog & Clue History Log (`L` key)** capturing recent multi-word dialog/clue lines into a scrollable overlay.
  - Added **Rolling Auto-Save Backup** (`game_files/mha2.sav/autosave_backup/`, throttled every 3 min on room changes).
  - Added **Multi-Monitor Target Display Picker** in `F1` → `1. DISPLAY`.

### Graphics, Shaders & Audio Remastering
- **Audio Mastering EQ + M/S Stereo Widener** (`runtime/win32_dsound.c`):
  - Added real-time 2nd-order RBJ stereo biquad Low-Shelf + High-Shelf EQ and Mid/Side stereo widening (`OFF` / `Warm Remaster` / `Studio Crisp`) in `F1` → `3. ASSETS & TEXT`.
- **Upgraded Wide-Radius Debanding** (`runtime/gl_renderer.c`):
  - Replaced the 1.8px debander with an mpv-style 2-ring wide-radius (`7–14px` & `18–32px`) jittered edge-preserving gradient debander + triangular dither.
- **Fixed AMD FidelityFX FSR 1.0 on 4K Canvas** (`runtime/gl_renderer.c`):
  - Enabled true 12-tap directional EASU directly on the `3200×2400` 4K canvas and replaced double-RCAS with authentic per-channel RGB 2-scale RCAS with soft overshoot clamping.
- **Engine-Side Edge Polish** (`runtime/gl_renderer.c`):
  - Added `SMAA + 3x Edge AA` mode (structure-tensor tangent AA targeting baked-in 1× staircases) and `FS_OSD` HD sprite edge feathering & dark-halo defringe.

### Bug Fixes & Stability
- **Safe (`Tresor`) Rotary Wheel & Puzzle Digit Fix (`HDS5`)** (`runtime/hd_assets.c`):
  - Upgraded the sprite matcher from `8×8` to **`24×24`** thumbnails (9× higher spatial detail) with un-truncated fixed-point SAD + peak-cell penalty (`max_drgb * 4`), fixing all 10/40 safe wheel digit mismatches (`40/40` exact matches).
- **Death's Office (`l/006`) Black Rectangle Fix** (`runtime/gl_renderer.c`, `hd_assets/`):
  - Downscaled 13 oversized `>16384px` multi-frame vertical filmstrips (`objTodIdle/Talk/Action`, `objGolem`, `objExplosion`, `objMischen`) to crisp `2×` and added a `GL_MAX_TEXTURE_SIZE` guard before `glTexImage2D`.
- **Gaussian-AA Room Prop Matching Fix (`HDS4`)** (`runtime/hd_assets.c`):
  - Lowered core-alpha threshold from `245` → `160` and relaxed tiny-prop `max_cell_da` to `230`, restoring 100% 4K matching on small room props (`invToteFledermaus`, `invAbacus`, `xobjKerze`, `objVase`, portraits) and SDF cursors.
- **Reliable Edge Back-Cursor Default** (`runtime/gl_renderer.c`):
  - Defaulted `decoupled_cursor` to `OFF` so edge-clipped back cursors (e.g., Clock close-up) always render reliably (`F10` toggle still available).

### Packaging & Documentation
- **Portable Linux x86_64 Packager** (`tools/package_portable_linux.sh`):
  - Added `./fonts/` lookup fallback in `gl_renderer.c` and a packager script that bundles `mha2_linux` (patched to `/lib64/ld-linux-x86-64.so.2`, `RUNPATH=$ORIGIN/lib`), all 86 non-GPU shared libraries (including `libSDL3.so.0`), DejaVu fonts, `game_files/`, and full 4K `hd_assets/` into a distro-agnostic `.tar.gz`.
- **README Showcase** (`README.md`, `docs/images/`):
  - Added labeled 4K split-screen, 1:1 macro zoom, and Clock hotspot-highlighter showcase screenshots plus updated v5 hotkey reference.

---

## [v5.0 — Neural Remaster & Quality-of-Life Suite] (`v5-neural-remaster`, 2026-11-20 – 2026-11-22)

### Rendering & Typography
- **Persistent 4K GPU DirectDraw Canvas (`3200×2400`)** (`runtime/gl_renderer.c`, `runtime/win32_ddraw.c`):
  - Mirrored all DirectDraw blits in exact painter's Z-order on dual 4K FBOs, eliminating cursor erase artifacts, flicker, and IT3 cell-size mismatches.
  - Added 16-tap Catmull-Rom supersampling + 2-scale (`1px` + `2px`) screen-pixel FidelityFX CAS sharpening.
- **Full IT3 Dual-Layer (`tbl1` + `tbl2`) 4K Compositing** (`runtime/win32_ddraw.c`, `tools/upscale_alpha_sprites.py`):
  - Hooked all 8 software IT3 RLE blitters and `Blitter::BlitBoth` (`0x00494b80`) to match & render `tbl1+tbl2` in a single 4K pass and suppress blurry 1× `tbl2` edge rings.
- **Native 4K Vector Outlined Typography** (`runtime/win32_ddraw.c`, `runtime/gl_renderer.c`):
  - Hooked `Font::DrawText` (`0x00486c40`) & `Font::MeasureText` (`0x00486ad0`) for invariant-bounds 4K TrueType text (`Clean Sans` / `Adventure Serif`) with optional soft readability backing, eliminating dirty-rect text morphing.

### Post-Processing & Shaders
- **3-Tab Interactive F1 Settings Menu** (`1. DISPLAY`, `2. POST-FX`, `3. ASSETS & TEXT`) with auto-persistence to `mha2_settings.ini`.
- **Multi-Pass OpenGL 3.3 Post-FX Pipeline**:
  - **AMD FidelityFX FSR 1.0** (`EASU` + `RCAS`), **Bicubic + CAS**, **Lanczos3 + CAS**, **Edge-Smooth**, **Bilinear**, **Nearest**, **Subtle CRT**.
  - **Anti-Aliasing**: `OFF` / `FXAA 3.11` (28px span) / `SMAA 1x (Crisp)` + live `AA Strength` (`0–100%`).
  - **Emissive Bloom Glow**: warm-weighted `0.72/0.14` bright-pass extract + 3-pass wide Gaussian halo (torch/candle/gold glow with zero midtone haze).
  - **Filmic HDR Tone-Map** (adjustable `0–100%`), **Local Contrast / Clarity** (adjustable `0–100%`), **Color Grading** (`Original` / `Remastered Rich` / `Cinematic Warm` / `Crisp Daylight`), **Living Atmosphere FX** (torch flicker + drifting dust motes), **Cinematic Vignette**, **210 ms Smooth Room Crossfades**, and **8-tap 2-radius Cutscene Deblock + Sharpen**.

### Quality-of-Life, Audio & Desktop Integration
- **Hotspot Highlighter**: Hold `Space` / `H` (or Gamepad `X`) to display pulsing golden markers on interactive room objects.
- **Fast-Walk / Speed-Up**: Hold `Shift` for `2×` speed or toggle `1.0x` / `1.5x` / `2.0x` in F1 menu via virtual clock scaling (`runtime/win32_kernel.c`).
- **Audio Remastering** (`runtime/win32_dsound.c`): Anti-pop master gain ramping + Schroeder stereo castle reverb (`OFF` / `Subtle Castle` / `Rich Hall`).
- **Native Gamepad Support**: Full `SDL_GameController` support (left stick cursor, A/B click/right-click, X hotspots, Start F1 menu).
- **Live Split-Screen Before/After (`F2`)** & **Clean 4K PNG Screenshots (`PrintScreen` / `Ctrl+P` → `screenshots/`)**.
- **Linux Desktop Integration** (`tools/install_desktop.sh`): Installs `~/.local/share/applications/mha2-remastered.desktop` and icon.

---

## [v4.0 — Zero-Stretch Widescreen & In-Game Menu] (`v4-widescreen-remaster`, 2026-11-19)
- Added automatic **16:10 Smart** (`800×500` → 0% stretch on `2560×1600`), **16:9 Cinema** (`800×450`), and **4:3** with blurred ambient sidebars (`F12`).
- Built the interactive mouse + keyboard **F1 Graphics Settings Menu** and persistent `.hd_index.cache` for instant HD asset startup.

---

## [v3.0 — Standalone Recompiler & First HD Pipeline] (`v3-full-remaster`, 2026-11-19)
- Completed **100% standalone x86-32 → C static recompiler** (`recomp/lift.py`) with automatic discovery of 8,100+ functions, 121 switch tables, and MSVC C++ EH `FuncInfo` handlers (zero Ghidra dependency).
- Added legal-copy disc/installer extractor (`tools/extract_game.py`) and `mha2.dat` asset extractor (`tools/extract_assets.py`).
- Introduced initial HD background replacement, native vector text overlay, and high-refresh decoupled cursor.

---

## [v2.0 — Modernized OpenGL 3.3 Engine] (`v2-remastered-engine`, 2026-11-18)
- Upgraded renderer from software SDL2 to **OpenGL 3.3 Core** with 32-bit TrueColor (`Mode 888`), GPU upscaling shaders, high-refresh frame pacing, and 48 kHz Float32 Catmull-Rom cubic audio mixing.

---

## [v1.0 — Initial Native Linux Static Recompilation] (`v1-working-recomp`, 2026-11-17)
- First fully playable native Linux x86_64 static recompilation of `mha2.exe` with custom Win32 / DirectDraw / DirectInput / DirectSound / AVI compatibility runtime (no Wine, no emulator).
