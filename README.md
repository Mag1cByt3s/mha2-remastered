![Moorhuhn Adventure 2: Der Fluch des Goldes — Remastered](docs/images/banner.png)

# Moorhuhn Adventure 2: Der Fluch des Goldes — Native Linux Remastered Engine (v5)

A full **standalone x86-32 to native Linux x86_64 static recompiler** and **modernized OpenGL 3.3 Core remaster engine** for *Moorhuhn Adventure 2: Der Fluch des Goldes* (`mha2.exe`), ported natively to Linux with **OpenGL 3.3 Core**, **SDL2**, **SDL2_ttf**, and **FFmpeg** — with **no Wine, no emulator, no Windows DLLs, and no Ghidra required at build time**.

> **Legal / IP Notice:** *Moorhuhn* / *Crazy Chicken* is a registered trademark of ak tronic Software & Services GmbH. This repository contains **only** original clean-room recompiler tooling, asset extractors, and a Linux runtime compatibility layer. No original game binaries, copyrighted assets, or lifted C code are included — you must supply your own legally owned copy of the game.
>
> See [CHANGELOG.md](CHANGELOG.md) for the full version history from `v1` static recompilation through `v5.3`.

## Showcase (Original 2005 vs 4K Remaster v5)

### Split-Screen Comparison — Castle Hallway (2005 `800×450` vs Live 4K Remaster)
![Original 2005 vs 4K Remaster v5 Split-Screen](docs/images/remaster_showcase_split.png)

### Side-by-Side & 1:1 Native 4K Detail Zooms
![Original 2005 vs 4K Remaster v5 Detail Comparison](docs/images/remaster_showcase_comparison.png)

### In-Game 4K Close-Up & Interactive Hotspot Highlighter (`Space` / `H`)
![4K Remaster v5 Clock Close-Up with Hotspot Highlighter](docs/images/remaster_showcase_clock_hotspots.png)

## Modernized Engine Features (v5)

- **100% Standalone Static Recompiler** ([recomp/lift.py](recomp/lift.py)):
  - Automatically discovers all 8,100+ functions, 121 MSVC switch tables (including negative-indexed and 1-based `memcpy`/`memmove` tables), and all MSVC C++ EH `FuncInfo` unwind/catch handlers directly from a user-supplied `mha2.exe` with **zero lift errors** and zero Ghidra dependency.
  - Includes a standalone legal-copy disc/installer extractor ([tools/extract_game.py](tools/extract_game.py)) supporting both Wise `Setup.EXE` and raw Mode 1/2352 `.bin`/`.iso` CD-ROM images with full CRC-32 verification.
- **Zero-Stretch True Widescreen (`16:10` & `16:9`) + Ambient Blur 4:3 Sidebars (`F12`)**:
  - Automatically detects native `800×450` (16:9) gameplay rooms/minigames vs `800×600` (4:3) menus.
  - **16:10 Smart (`800×500` → exact `3.2×` on `2560×1600`, 0% stretch, 0px black bars)**: full `800×450` room + dedicated `50px` bottom inventory dock with soft drop-shadow top HUD overlays.
  - **16:9 Cinema (`800×450`)**: edge-to-edge 16:9 gameplay with transparent on-demand top & bottom overlays.
  - **4:3 Menus**: aspect-correct with optional blurred ambient sidebars.
- **Interactive 3-Tab In-Game Graphics & Remaster FX Menu (`F1`) + Auto-Save (`mha2_settings.ini`)**:
  - Mouse + keyboard 3-tab menu (`1. Display & Scaling`, `2. Remaster FX & AA`, `3. Assets, HUD & Audio`) controlling resolution presets (`800×600`–`2560×1600`), aspect mode, shaders, bloom, color grading, silhouette AA, vignette, transitions, HD assets, and volume/reverb/EQ — automatically saved to `mha2_settings.ini`.
- **Multi-Pass OpenGL 3.3 FBO Post-Processing & Smooth Scene Crossfades**:
  - **Emissive Bloom** (`Off / Subtle / Medium / Rich`): quarter-res `GL_RGBA16F` warm-weighted bright-pass + dual separable 9-tap Gaussian blur for candles, torches, chandeliers, and gold artifacts.
  - **Color Grading & Vibrance** (`Original / Remastered Rich / Cinematic Warm / Crisp Daylight`).
  - **Sprite Silhouette AA & Anti-Banding Dithering**: lightweight FXAA edge smoothing + triangular-noise output dithering.
  - **Smooth 210 ms Cubic Ease-Out Room Crossfades**: zero-copy dual-FBO transitions between rooms and menus.
- **AI 4K Neural Backgrounds + Native GPU 4× RGBA Sprites (`F6`)** ([runtime/hd_assets.c](runtime/hd_assets.c), [tools/upscale_ai.py](tools/upscale_ai.py)):
  - Composites 4K (`3200×1800` / `3200×2400`) Real-ESRGAN backgrounds for both menus (`800×600`) and gameplay rooms (`800×450`), plus an O(1) dimension-bucketed LRU GPU renderer for 4× RGBA foreground characters, props, inventory icons, buttons, and cursors (`hd_assets/sprites/`), all cached to disk for instant startup.
- **Native Monitor-DPI Vector Text & Subtitles (`F5`)** & **High-Refresh Decoupled Cursor (`F10`)**:
  - Crisp native monitor-DPI TrueType text rendered post-FX, plus a 4× HD decoupled cursor rendered at full monitor refresh rate (120/144/240Hz).
- **True 32-bit Color (`Mode 888`), 7 Upscaling Shaders (incl. AMD FidelityFX FSR 1.0), & 48 kHz Cubic Audio Mixer**:
  - Bicubic+CAS, Lanczos3+CAS, Edge-Smooth, Bilinear, Nearest, Subtle CRT, and AMD FidelityFX FSR 1.0 (`EASU + RCAS`); Float32 48 kHz Catmull-Rom audio mixer with reverb and mastering EQ (`F7`/`F8`/`F9`).
- **Remastered FMV Cutscenes** ([runtime/win32_avi.c](runtime/win32_avi.c), [tools/upscale_videos.py](tools/upscale_videos.py)):
  - Real-time GPU deblocking + anti-ringing + sharpening for 2005 Cinepak AVIs, plus automatic drop-in HD MP4/MKV cutscene playback from `hd_assets/videos/` in full audio sync.

## Building From Your Own Legal Copy

### NixOS (`nix-shell`)

```bash
# 1. Extract your legally owned CD image (.bin/.iso) or Wise Setup.EXE into game_files/:
nix-shell --run "python3 tools/extract_game.py '/path/to/Setup.EXE' -o game_files"
# (or copy your existing installed game directory containing mha2.exe, mha2.dat, *.avi into game_files/)

# 2. Lift mha2.exe to C (100% standalone, no Ghidra required) and compile native x86_64 ELF:
nix-shell --run "make lift && make -j$(nproc)"

# 3. Play!
./play.sh
```

### Standard Linux (Ubuntu / Debian / Fedora / Arch)

Install the build dependencies:
- **Compilers & build tools:** `gcc`, `make`, `pkg-config`, `python3`, `python3-capstone`
- **Development headers:** `SDL2` (`libsdl2-dev`), `SDL2_ttf` (`libsdl2-ttf-dev`), `FFmpeg` (`libavformat-dev`, `libavcodec-dev`, `libswscale-dev`, `libavutil-dev`), and `OpenGL` (`libgl-dev`)

```bash
# 1. Supply your legally owned game files in game_files/ (or extract them with tools/extract_game.py):
python3 tools/extract_game.py '/path/to/Setup.EXE' -o game_files

# 2. Lift mha2.exe to C and build the native x86_64 executable:
make lift && make -j"$(nproc)"

# 3. Launch:
./mha2_linux
```

### Portable Linux Release Bundle

To package a self-contained portable x86_64 `.tar.gz` bundle using the Ubuntu 22.04 (`glibc 2.35`) builder container (`podman` or `docker`):

```bash
make lift
bash tools/package_portable_linux.sh
```

## Building the 4K AI Neural HD Pack (`hd_assets/`)

```bash
# Build 4K detail-preserving backgrounds (4x_NMKD-Siax_200k) + 4x RGBA sprites (ultrasharp-4x) on your Vulkan GPU:
nix-shell --run "python3 tools/upscale_ai.py --all"

# Remaster the FMV cutscenes (mha2il/ol/sl.avi) to 1600x1200 H.264 MP4 in hd_assets/videos/:
nix-shell --run "python3 tools/upscale_videos.py --scale 2"

# Or extract raw 1x assets manually:
nix-shell --run "python3 tools/extract_assets.py --list"
nix-shell --run "python3 tools/extract_assets.py --images -o extracted_assets"
```

## Launching & CLI Options

```bash
./play.sh                                       # Default Remastered v5 (loads mha2_settings.ini)
./play.sh --fullscreen --hud                    # Borderless fullscreen + live HUD
./play.sh --aspect 16:10 --grade remastered     # Force 16:10 Smart widescreen & color grading
./play.sh --16bit --no-hd-assets --no-hd-sprites --no-hd-text --no-bloom --grade original  # Pure 2005 look
```

### In-Game Hotkeys
| Key | Action |
|---|---|
| **F1** | Open/Close Interactive 3-Tab Display, Post-FX & Audio Settings Menu |
| **F2** | Toggle Live Split-Screen Before / After Comparison (2005 vs 4K Remaster) |
| **F3** | Toggle live GPU stats HUD |
| **F4** | Cycle VSync / High-FPS mode |
| **F5** | Toggle Native Monitor-DPI Vector Text & Subtitles |
| **F6** | Toggle HD Asset Pack Override (`hd_assets/`) |
| **F7 / F8 / F9** | Master volume down / up / mute |
| **F10** | Toggle High-Refresh Decoupled Cursor |
| **F11** or **Alt+Enter** | Toggle borderless fullscreen |
| **F12** | Cycle Widescreen Aspect (`Auto` / `16:10` / `16:9` / `4:3`) |
| **Space** or **H** | Hold to highlight interactive hotspots in the room |
| **Ctrl+L** | Open/Close Dialog & Clue History Log |
| **Shift** | Hold for 2× fast-walk speed |
| **Ctrl + P** or **PrintScreen** | Save clean 4K PNG screenshot (`screenshots/`) |
| **ESC** | Skip cutscenes / open game menu / close F1 menu |

## License

The clean-room static recompiler, native Linux runtime, and asset-processing tooling in this repository are released under the **MIT License** — see [LICENSE](LICENSE) for details. Original game binaries, artwork, sound, and video assets remain the property of their respective copyright holders and are excluded from this repository.
