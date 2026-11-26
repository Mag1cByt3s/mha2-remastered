# Installing the Linux Release

The engine is kept apart from everything that belongs to the game, so engine updates stay small:

| Part | Size | Where it comes from |
|---|---|---|
| `…_Runtime.tar.gz` | small | `mha2_linux`, bundled libraries, fonts, `PLAY.sh`, docs |
| your game files | ~480 MB | `mha2.dat`, `mha2.exe`, `*.avi` from your own copy of the game |
| `hd_assets/` (optional) | ~5 GB | **built by you** from your game files (see below) |

The HD pack (4K backgrounds, sprites, cursors, cutscenes and the page texts of the book
reader) is derived from the game's copyrighted art, so it is never published. Without it
the game runs in its original 2005 graphics with all engine features; with it you get the
4K remaster. `SHA256SUMS` covers the runtime archive (`sha256sum -c SHA256SUMS`).

## Layout A — everything in one folder

```bash
tar -xzf Moorhuhn_Adventure2_Remastered_Linux_x86_64_Runtime.tar.gz
cp /path/to/your/game/{mha2.dat,mha2.exe,*.avi} Moorhuhn_Adventure2_Remastered_Linux_x86_64/game_files/
# optional: your self-built HD pack
cp -a /path/to/your/hd_assets Moorhuhn_Adventure2_Remastered_Linux_x86_64/
./Moorhuhn_Adventure2_Remastered_Linux_x86_64/PLAY.sh
```

To update the engine later, extract a newer runtime archive over the folder; the HD
assets, game files, settings and saves stay in place.

## Layout B — shared data directory

Game files and HD assets can live once per user in
`$XDG_DATA_HOME/mha2-remastered/` (default `~/.local/share/mha2-remastered/`):

```
~/.local/share/mha2-remastered/
├── game_files/    mha2.dat, mha2.exe, *.avi, mha2.sav/ (saves)
└── hd_assets/
```

The engine uses them whenever `game_files/` / `hd_assets/` are not present next to its
working directory, so several runtime versions can share one data set. Command-line
overrides: `--game-dir <dir>`, `--hd-dir <dir>`.

## Updates

The tarball releases do not update themselves. To update one, extract a newer
`…_Runtime.tar.gz` over the existing folder (saves, settings, game files and HD assets
are not in the archive and stay untouched; `--strip-components=1` makes it independent of
the folder name):

```bash
cd /path/to/the/game/folder
tar -xzf ~/Downloads/Moorhuhn_Adventure2_Remastered_Linux_x86_64_Runtime.tar.gz --strip-components=1 -C .
```

The installed version is in the `VERSION` file and the `F1` menu title. Installs made with
`git clone https://github.com/Mag1cByt3s/mha2-remastered.git` and started with `./play.sh`
update automatically to every new release tag (saves and settings are kept and backed
up; see the README section "Automatic Updates").

## Getting the game files

Use `tools/extract_game.py` from the source repository to extract the files from the
original installer (`Setup.EXE`) or CD image (`.bin`/`.iso`). The engine never ships or
downloads original game data.

## Building the HD pack

The HD pack is built from a `git clone` of the source repository with your game files in
`game_files/`; the complete command list is in the README section "Building the 4K AI
Neural HD Pack". You need an NVIDIA GPU (CUDA) and about 17 GB of free disk space: the
finished pack is ~5 GB, the upscaling models ~0.7 GB and the text-recognition models ~11 GB
(in `~/.cache/huggingface`, deletable afterwards). Copy the resulting `hd_assets/` next to the
runtime (Layout A) or into the shared data directory (Layout B).

One step creates the texts for the book reader (`Space` on a book page, letter or notice):
`tools/extract_page_text.py` reads every page with three independent text-recognition
models (PaddleOCR-VL, Qwen3-VL-4B, GLM-OCR; downloaded on first use). The
three readings are compared word by word, and a page only gets text if every word is
backed by at least two of the three. Pages where they disagree (a word hidden behind a
finger, handwriting, the pseudo-Arabic notice board) get no text; `Space` then opens the
magnifier, an enlarged view of the 4K page. Nothing is ever guessed: in tests against
hand-made transcriptions every accepted page was letter-perfect. You can correct or add
texts by hand in `hd_assets/…/*.txt` (one paragraph per line, `---` between pages); the
tool never overwrites files that contain text.

## Building the release

```bash
make lift                                        # once, generates recomp/gen/
bash tools/package_portable_linux.sh --split     # runtime archive + SHA256SUMS (+ a tar of your own hd_assets/)
bash tools/package_portable_linux.sh             # all-in-one bundle with your game files (personal use)
```

AppImage / Flatpak packaging is not provided: `appimagetool` is not in nixpkgs, and
Flatpak needs multi-GB SDK runtimes. The split runtime already gives small engine
updates, and Layout B gives the same "data outside the program" separation.

Only the runtime archive may be shared. The HD tar from `--split` and the all-in-one bundle
contain game-derived data and are for your own machines only.
