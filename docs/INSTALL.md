# Installing the Linux Release

The release is split into three parts so engine updates stay small:

| Part | Size | Contents |
|---|---|---|
| `…_Runtime.tar.gz` | small | `mha2_linux`, bundled libraries, fonts, `PLAY.sh`, docs |
| `…_HD_Assets.tar` | ~5 GB | 4K backgrounds, sprites, cursors and cutscenes (`hd_assets/`) |
| your game files | ~480 MB | `mha2.dat`, `mha2.exe`, `*.avi` from your own copy of the game |

`SHA256SUMS` covers both archives (`sha256sum -c SHA256SUMS`).

## Layout A — everything in one folder

```bash
tar -xzf Moorhuhn_Adventure2_Remastered_Linux_x86_64_Runtime.tar.gz
tar -xf  Moorhuhn_Adventure2_Remastered_HD_Assets.tar      # same top-level folder
cp /path/to/your/game/{mha2.dat,mha2.exe,*.avi} Moorhuhn_Adventure2_Remastered_Linux_x86_64/game_files/
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

## Getting the game files

Use `tools/extract_game.py` from the source repository to extract the files from the
original installer (`Setup.EXE`) or CD image (`.bin`/`.iso`). The engine never ships or
downloads original game data.

## Building the release

```bash
make lift                                        # once, generates recomp/gen/
bash tools/package_portable_linux.sh --split     # runtime + HD asset pack + SHA256SUMS
bash tools/package_portable_linux.sh             # legacy all-in-one bundle (personal use)
```

AppImage / Flatpak packaging is not provided: `appimagetool` is not in nixpkgs, and
Flatpak needs multi-GB SDK runtimes. The split runtime already gives small engine
updates, and Layout B gives the same "data outside the program" separation.
