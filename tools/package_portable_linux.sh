#!/usr/bin/env bash
# Builds a self-contained portable Linux x86_64 bundle for any modern Linux distro
# (Ubuntu 22.04+, Fedora 39+, Debian 12+, Arch, Linux Mint 21+, SteamOS, NixOS, etc.)
set -euo pipefail

MAKE_ARCHIVE=1
SPLIT=0
for arg in "$@"; do
    case "$arg" in
        --no-archive) MAKE_ARCHIVE=0 ;;
        --split) SPLIT=1 ;;
        *)
            echo "Usage: $0 [--no-archive] [--split]" >&2
            echo "  --split  small runtime archive (no game files, no HD assets) + separate HD asset" >&2
            echo "           tar + SHA256SUMS; players supply their own game files" >&2
            exit 2
            ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_NAME="Moorhuhn_Adventure2_Remastered_Linux_x86_64"
STAGE="$ROOT/work/dist/$DIST_NAME"
ARCHIVE="$ROOT/work/dist/${DIST_NAME}.tar.gz"
CONTAINERFILE="$ROOT/tools/Containerfile.portable"

if [[ ! -f "$ROOT/recomp/gen/dispatch.c" ]]; then
    echo "Error: $ROOT/recomp/gen/dispatch.c is missing; run 'make lift' first." >&2
    exit 1
fi

if command -v podman >/dev/null 2>&1; then
    CTR=podman
    CTR_RUN_ARGS=()
elif command -v docker >/dev/null 2>&1; then
    CTR=docker
    CTR_RUN_ARGS=(--user "$(id -u):$(id -g)")
else
    echo "Error: podman or docker is required to build the portable Linux bundle." >&2
    exit 1
fi

CF_HASH="$(sha256sum "$CONTAINERFILE" | awk '{print $1}')"
IMAGE_TAGGED="localhost/mha2-portable-builder:ubuntu2204-${CF_HASH:0:12}"
IMAGE_LATEST="localhost/mha2-portable-builder:ubuntu2204"

# NixOS `/` (/var/tmp) may be a small tmpfs; keep container image staging under work/tmp
mkdir -p "$ROOT/work/tmp"
export TMPDIR="$ROOT/work/tmp"

if ! "$CTR" image exists "$IMAGE_TAGGED" >/dev/null 2>&1; then
    echo "[pack] Building portable builder image $IMAGE_TAGGED ..."
    "$CTR" build -f "$CONTAINERFILE" -t "$IMAGE_TAGGED" -t "$IMAGE_LATEST" "$ROOT/tools"
else
    echo "[pack] Reusing cached builder image $IMAGE_TAGGED"
fi

# The builder container has no git: resolve the engine version (release tag) on the host.
MHA2_VERSION="$(git -C "$ROOT" describe --tags --match 'v[0-9]*.[0-9]*' --always --dirty 2>/dev/null || echo unknown)"
echo "[pack] Engine version $MHA2_VERSION"

echo "[pack] Staging portable Linux x86_64 bundle in $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib" "$STAGE/fonts" "$STAGE/game_files/mha2.sav" "$ROOT/work/build_portable"

# Build binary, collect transitive non-core deps, patch ELF headers, bundle fonts & verify ABI
"$CTR" run --rm "${CTR_RUN_ARGS[@]}" -v "$ROOT:/src" -w /src "$IMAGE_TAGGED" bash -euo pipefail -c '
    DIST_NAME="'"$DIST_NAME"'"
    MHA2_VERSION="'"$MHA2_VERSION"'"
    STAGE="/src/work/dist/$DIST_NAME"
    export PKG_CONFIG_PATH=/opt/sdl2-portable/lib/pkgconfig:/opt/ffmpeg-min/lib/pkgconfig

    # Force recompile of runtime objects against /opt/sdl2-portable headers while preserving gen_*.o cache
    rm -f work/build_portable/rt_*.o work/build_portable/mha2_linux

    SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_ttf gl) $(pkg-config --cflags libavformat libavcodec libswscale libavutil)"
    SDL_LIBS="-Wl,-rpath,/opt/sdl2-portable/lib $(pkg-config --libs sdl2 SDL2_ttf gl) $(pkg-config --static --libs libavformat libavcodec libswscale libavutil)"

    make -j"$(nproc)" \
        BUILD_DIR=work/build_portable \
        TARGET=work/build_portable/mha2_linux \
        MHA2_VERSION="$MHA2_VERSION" \
        SDL_CFLAGS="$SDL_CFLAGS" \
        SDL_LIBS="$SDL_LIBS"

    cp -f work/build_portable/mha2_linux "$STAGE/mha2_linux"
    chmod 755 "$STAGE/mha2_linux"

    sdl2_resolved="$(ldd "$STAGE/mha2_linux" | awk "/libSDL2-2\.0\.so\.0 =>/ {print \$3}")"
    if [[ "$sdl2_resolved" != "/opt/sdl2-portable/lib/libSDL2-2.0.so.0" ]]; then
        echo "ERROR: Expected libSDL2-2.0.so.0 from /opt/sdl2-portable/lib, got '\''$sdl2_resolved'\''" >&2
        exit 1
    fi

    EXCLUDE_REGEX="^(linux-vdso|ld-linux|libc\.so|libm\.so|libmvec\.so|libpthread\.so|libdl\.so|librt\.so|libresolv\.so|libnss_|libz\.so|libGL\.so|libGLX\.so|libGLdispatch\.so|libEGL\.so|libOpenGL\.so|libvulkan\.so|libdrm\.so|libgbm\.so|libX11|libXext|libXau|libXdmcp|libxcb|libwayland|libasound|libpulse|libpipewire|libdbus|libsystemd|libudev|libstdc\+\+\.so|libgcc_s\.so)"

    ldd "$STAGE/mha2_linux" 2>/dev/null | awk "/=> \// {print \$3}" | while read -r so; do
        base="$(basename "$so")"
        if [[ "$base" =~ $EXCLUDE_REGEX ]]; then
            continue
        fi
        if [[ ! -f "$STAGE/lib/$base" ]]; then
            cp -L "$so" "$STAGE/lib/$base"
            chmod 755 "$STAGE/lib/$base"
        fi
    done

    patchelf --set-interpreter /lib64/ld-linux-x86-64.so.2 --set-rpath "\$ORIGIN/lib" "$STAGE/mha2_linux"
    for so in "$STAGE"/lib/*.so*; do
        [[ -f "$so" ]] || continue
        patchelf --set-rpath "\$ORIGIN" "$so"
    done

    for fn in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSerif-Bold.ttf; do
        src="/usr/share/fonts/truetype/dejavu/$fn"
        if [[ ! -s "$src" ]]; then
            echo "ERROR: Missing font $src inside container!" >&2
            exit 1
        fi
        cp -L "$src" "$STAGE/fonts/$fn"
    done

    echo "[pack] Patched binary + $(ls -1 "$STAGE/lib" | wc -l) bundled libraries"

    # Strict portability & ABI verification (must resolve purely via ELF RUNPATH without LD_LIBRARY_PATH)
    if ldd "$STAGE/mha2_linux" | grep -q "not found"; then
        echo "ERROR: Unresolved shared library dependency without LD_LIBRARY_PATH:" >&2
        ldd "$STAGE/mha2_linux" >&2
        exit 1
    fi
    for req_lib in libSDL2-2.0.so.0 libSDL2_ttf-2.0.so.0; do
        resolved="$(ldd "$STAGE/mha2_linux" | awk -v lib="$req_lib" "\$1 == lib {print \$3}")"
        if [[ "$resolved" != "$STAGE/lib/$req_lib" ]]; then
            echo "ERROR: Expected $req_lib to resolve from $STAGE/lib/$req_lib via RUNPATH, got '\''$resolved'\''" >&2
            exit 1
        fi
    done

    for elf in "$STAGE/mha2_linux" "$STAGE"/lib/*.so*; do
        [[ -f "$elf" ]] || continue
        if grep -qa "/nix/store" "$elf"; then
            echo "ERROR: Found /nix/store reference in $elf" >&2
            exit 1
        fi
        bad_ver="$(readelf -V "$elf" 2>/dev/null | grep -oE "GLIBC_2\.(3[6-9]|[4-9][0-9])|GLIBCXX_3\.4\.(3[1-9]|[4-9][0-9])|CXXABI_1\.3\.(1[4-9]|[2-9][0-9])" || true)"
        if [[ -n "$bad_ver" ]]; then
            echo "ERROR: Incompatible ABI symbol version in $(basename "$elf"): $bad_ver" >&2
            exit 1
        fi
        bad_need="$(readelf -d "$elf" 2>/dev/null | grep "NEEDED" | grep -E "libvulkan\.so|libpipewire|libpulse|libwayland|libEGL|libgbm|libdecor" || true)"
        if [[ -n "$bad_need" ]]; then
            echo "ERROR: Forbidden hard DT_NEEDED in $(basename "$elf"): $bad_need" >&2
            exit 1
        fi
    done

    codec_syms="$(nm --defined-only /opt/ffmpeg-min/lib/libavcodec.a 2>/dev/null)"
    for sym in ff_webp_decoder ff_png_decoder ff_bmp_decoder ff_png_encoder; do
        if ! grep -qE " [DT] ${sym}$" <<< "$codec_syms"; then
            echo "ERROR: Required FFmpeg codec symbol $sym missing from /opt/ffmpeg-min/lib/libavcodec.a" >&2
            exit 1
        fi
    done
    fmt_syms="$(nm --defined-only /opt/ffmpeg-min/lib/libavformat.a 2>/dev/null)"
    if ! grep -qE " [DT] ff_image2_demuxer$" <<< "$fmt_syms"; then
        echo "ERROR: Required FFmpeg image2 demuxer missing from /opt/ffmpeg-min/lib/libavformat.a" >&2
        exit 1
    fi

    echo "[pack] ABI & portability verification passed!"
'

if [[ "$SPLIT" -eq 1 ]]; then
    # Runtime-only: players supply their own game files (tools/extract_game.py) and the
    # HD asset pack ships separately, so engine updates stay small.
    cat > "$STAGE/game_files/PUT_GAME_FILES_HERE.txt" << 'GAMEFILES_EOF'
Copy mha2.dat, mha2.exe and the six *.avi files of your own copy of
"Moorhuhn Adventure 2: Der Fluch des Goldes" into this folder
(or into ~/.local/share/mha2-remastered/game_files/).
Use tools/extract_game.py from the source repository to extract them from the
original installer or CD image.
GAMEFILES_EOF
    mkdir -p "$STAGE/docs"
    cp "$ROOT/docs/STEAM_DECK.md" "$ROOT/docs/INSTALL.md" "$STAGE/docs/"
else
    # Copy original game files (without personal saves)
    cp "$ROOT/game_files/mha2.dat" "$ROOT/game_files/mha2.exe" "$STAGE/game_files/"
    cp "$ROOT/game_files/"*.avi "$STAGE/game_files/"

    # Copy complete 4K hd_assets (backgrounds, sprites, cursors, 4K MP4 videos, caches)
    cp -a --reflink=auto "$ROOT/hd_assets" "$STAGE/hd_assets"
fi

# Write portable launcher script (rely on ELF $ORIGIN/lib RUNPATH so host GPU/audio libs never conflict)
cat > "$STAGE/PLAY.sh" << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"
export MHA2_FONT_PATH="${MHA2_FONT_PATH:-$DIR/fonts/DejaVuSans.ttf}"
export MHA2_GAME_FONT_PATH="${MHA2_GAME_FONT_PATH:-$DIR/fonts/DejaVuSans-Bold.ttf}"
if [[ ! -f "$DIR/mha2_settings.ini" && $# -eq 0 ]]; then
    exec "$DIR/mha2_linux" --fullscreen
fi
exec "$DIR/mha2_linux" "$@"
EOF
chmod 755 "$STAGE/PLAY.sh"

echo "$MHA2_VERSION" > "$STAGE/VERSION"

# Write quick-start guide
cat > "$STAGE/README_PLAY.txt" << 'EOF'
Moorhuhn Adventure 2: Der Fluch des Goldes — Remastered (Linux x86_64)
=====================================================================
Installed version: see the file VERSION (also shown in the F1 menu title).

HOW TO PLAY:
  Double-click PLAY.sh or run in a terminal:
    ./PLAY.sh

  Windowed mode:
    ./PLAY.sh --windowed

CONTROLS & SHORTCUTS (defaults; rebind in F1 -> 4. CONTROLS):
  Left Click     Interact / Walk
  Right Click    Cycle action cursor (Look / Take / Talk / Use)
  F1             Settings menu (Display, Post-FX, Assets & Text, Controls)
  F2             Live Split-Screen Before / After (2005 vs 4K Remaster)
  Space or H     Hold to highlight interactive hotspots in the room
  Shift          Hold for 2x fast-walk speed
  Ctrl + S / O   Quick Save / Quick Load (slot 9)
  Ctrl + L       Dialog & clue history
  Ctrl + P       Save a clean 4K PNG screenshot (saved to screenshots/)
  Alt + Enter    Toggle Fullscreen / Windowed
  F3             Toggle FPS / Remaster HUD
  F7 / F8 / F9   Volume Down / Up / Mute
  Gamepad        Left Stick = Cursor, A = Click, B = Right-Click, X = Hotspots,
                 Back = F1 Menu, Start = Esc, D-pad Up/Down = Quick Save/Load

REQUIREMENTS:
  64-bit Linux (Ubuntu 22.04+, Fedora 39+, Debian 12+, Arch, Mint 21+, SteamOS, etc.)
  with OpenGL 3.3+ graphics drivers.

UPDATING (savegames and settings are kept):
  1. Close the game. Optional safety copy of saves + settings:
       cp -a game_files/mha2.sav ~/mha2_saves_backup; cp mha2_settings.ini ~/ 2>/dev/null
  2. Extract the new ..._Runtime.tar.gz OVER this folder (works even if you renamed it):
       tar -xzf ~/Downloads/Moorhuhn_Adventure2_Remastered_Linux_x86_64_Runtime.tar.gz \
           --strip-components=1 -C /path/to/this/folder
  3. Start ./PLAY.sh as usual; the F1 menu title shows the new version.
  The runtime archive only contains the program (mha2_linux, lib/, fonts/, PLAY.sh,
  docs). Your game_files/ (incl. saves in game_files/mha2.sav/), mha2_settings.ini,
  screenshots/ and hd_assets/ are left untouched. Only re-extract the HD asset pack
  (Moorhuhn_Adventure2_Remastered_HD_Assets.tar, same command with -xf) if you were
  told that the HD assets changed.
EOF

if [[ "$MAKE_ARCHIVE" -eq 1 && "$SPLIT" -eq 1 ]]; then
    RT_ARCHIVE="$ROOT/work/dist/${DIST_NAME}_Runtime.tar.gz"
    HD_ARCHIVE="$ROOT/work/dist/Moorhuhn_Adventure2_Remastered_HD_Assets.tar"
    echo "[pack] Creating runtime archive $RT_ARCHIVE ..."
    tar -czf "$RT_ARCHIVE" -C "$ROOT/work/dist" "$DIST_NAME"
    # WebP/MP4 are already compressed: plain tar, rooted at the same top-level folder so
    # both archives extract into one directory.
    echo "[pack] Creating HD asset pack $HD_ARCHIVE ..."
    tar -cf "$HD_ARCHIVE" -C "$ROOT" --transform "s,^hd_assets,$DIST_NAME/hd_assets," hd_assets
    (cd "$ROOT/work/dist" && sha256sum "$(basename "$RT_ARCHIVE")" "$(basename "$HD_ARCHIVE")" > SHA256SUMS)
    ls -lh "$RT_ARCHIVE" "$HD_ARCHIVE" "$ROOT/work/dist/SHA256SUMS"
elif [[ "$MAKE_ARCHIVE" -eq 1 ]]; then
    echo "[pack] Creating archive $ARCHIVE ..."
    GZIP=-1 tar -czf "$ARCHIVE" -C "$ROOT/work/dist" "$DIST_NAME"
    ls -lh "$ARCHIVE"
else
    echo "[pack] Skipping .tar.gz creation (--no-archive)"
fi
echo "[pack] Done!"
