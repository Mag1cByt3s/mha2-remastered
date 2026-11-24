#!/usr/bin/env bash
# Replay-driven regression runner for ./mha2_linux (see docs/REPLAY.md).
#
# Every script runs in a fresh sandbox: --game-dir points at symlinks to the
# read-only game data plus an EMPTY mha2.sav/, and the working directory is the
# sandbox (default settings, crash log stays inside). Real savegames and
# mha2_settings.ini are never touched.
#
# Checks per script: exit code 0, "[replay] PASS", no crash/fatal markers, and
# (if a local baseline exists) screenshot RMSE <= threshold. Baselines contain
# copyrighted game imagery and live under work/ (gitignored) only.
#
# Usage: tools/regress.sh [options] [script.rec ...]   (default: tests/replay/*.rec)
#   --visible           show the game window (default: headless SDL offscreen driver)
#   --strict-mem        also run with --strict-mem (guest NULL page protected)
#   --update-baseline   store this run's screenshots as the new local baseline
#   --threshold <f>     max normalized RMSE per screenshot (default 0.05)
#   --timeout <s>       per-script timeout in seconds (default 300)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VISIBLE=0 STRICT=0 UPDATE=0 THRESH=0.05 TMO=300
SCRIPTS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --visible) VISIBLE=1 ;;
    --strict-mem) STRICT=1 ;;
    --update-baseline) UPDATE=1 ;;
    --threshold) THRESH="$2"; shift ;;
    --timeout) TMO="$2"; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) SCRIPTS+=("$1") ;;
  esac
  shift
done
[ ${#SCRIPTS[@]} -eq 0 ] && SCRIPTS=(tests/replay/*.rec)

BIN="${MHA2_BIN:-$ROOT/mha2_linux}"  # MHA2_BIN: test an alternate build (e.g. flag-check lift)
GAME_SRC="$ROOT/game_files"
BASELINE="$ROOT/work/regress_baseline"
RUN="$ROOT/work/regress/$(date +%Y%m%d_%H%M%S)"
[ -x "$BIN" ] || { echo "error: $BIN not built (make -j\$(nproc))"; exit 2; }
[ -f "$GAME_SRC/mha2.exe" ] || { echo "error: $GAME_SRC/mha2.exe missing"; exit 2; }
HAVE_MAGICK=0; command -v magick >/dev/null 2>&1 && HAVE_MAGICK=1
mkdir -p "$RUN"

make_sandbox() {  # $1 = sandbox dir
  mkdir -p "$1/game/mha2.sav"
  for f in "$GAME_SRC"/*; do
    [ "$(basename "$f")" = "mha2.sav" ] && continue
    ln -s "$f" "$1/game/$(basename "$f")"
  done
}

compare_shots() {  # $1 = shots dir, $2 = baseline dir, $3 = script; prints failures, returns count
  local fails=0 png base out val thr
  for png in "$1"/*.png; do
    [ -e "$png" ] || continue
    base="$2/$(basename "$png")"
    [ -f "$base" ] || { echo "    (no baseline for $(basename "$png"))"; continue; }
    out=$(magick compare -metric RMSE "$png" "$base" null: 2>&1)
    val=$(printf '%s' "$out" | sed -n 's/.*(\([0-9.e+-]*\)).*/\1/p')
    [ -n "$val" ] || { echo "    $(basename "$png"): compare failed: $out"; fails=$((fails + 1)); continue; }
    # per-shot override in the script: "#! threshold <shot> <rmse>" (animated overlays)
    thr=$(sed -n "s/^#! threshold $(basename "$png" .png) \([0-9.]*\).*/\1/p" "$3" | head -1)
    thr=${thr:-$THRESH}
    if awk -v v="$val" -v t="$thr" 'BEGIN { exit !(v > t) }'; then
      echo "    $(basename "$png"): RMSE $val > $thr  REGRESSION"
      magick compare "$png" "$base" "${png%.png}.diff.png" 2>/dev/null
      fails=$((fails + 1))
    else
      echo "    $(basename "$png"): RMSE $val ok (<= $thr)"
    fi
  done
  return $fails
}

TOTAL=0 FAILED=0
VARIANTS=("")
[ $STRICT -eq 1 ] && VARIANTS+=("--strict-mem")
for script in "${SCRIPTS[@]}"; do
  for variant in "${VARIANTS[@]}"; do
    name="$(basename "$script" .rec)${variant:+_strict}"
    dir="$RUN/$name"
    make_sandbox "$dir"
    TOTAL=$((TOTAL + 1))
    echo "== $name"
    start=$(date +%s)
    (
      cd "$dir" || exit 2
      [ $VISIBLE -eq 1 ] || export SDL_VIDEODRIVER=offscreen
      export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
      timeout "$TMO" "$BIN" --game-dir "$dir/game" --hd-dir "$ROOT/hd_assets" --windowed \
        --width 1280 --height 960 --replay "$ROOT/$script" --replay-out "$dir/shots" $variant
    ) > "$dir/game.log" 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    problems=()
    [ $rc -eq 0 ] || problems+=("exit code $rc")
    grep -q '^\[replay\] PASS' "$dir/game.log" || problems+=("no PASS line")
    bad=$(grep -E 'MHA2 CRASH|FATAL|#DE|LIFT ERROR|UNIMPLEMENTED|guest spinlock .* contended' "$dir/game.log" | head -3)
    [ -z "$bad" ] || problems+=("log: $bad")
    grep '^\[replay\] FAIL' "$dir/game.log" | head -1 | sed 's/^/    /'
    if [ $UPDATE -eq 1 ]; then
      mkdir -p "$BASELINE/$name"
      cp "$dir"/shots/*.png "$BASELINE/$name/" 2>/dev/null && echo "    baseline updated ($BASELINE/$name)"
    elif [ $HAVE_MAGICK -eq 1 ] && [ -d "$BASELINE/$name" ]; then
      compare_shots "$dir/shots" "$BASELINE/$name" "$ROOT/$script" || problems+=("screenshot regression")
    fi
    grep '^\[replay\] hash' "$dir/game.log" | sed 's/^\[replay\] /    /'
    if [ ${#problems[@]} -eq 0 ]; then
      echo "   PASS (${secs}s)"
    else
      FAILED=$((FAILED + 1))
      printf '   FAIL (%ss): %s\n' "$secs" "${problems[*]}"
      echo "   log: $dir/game.log"
    fi
  done
done

echo
echo "regress: $((TOTAL - FAILED))/$TOTAL passed  (artifacts: $RUN)"
[ $FAILED -eq 0 ]
