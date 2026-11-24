#!/usr/bin/env bash
# Tests for tools/update.sh on throwaway git repositories (bare "origin" + player clone)
# with a stub build command - no network, no game files, a few seconds.
# Usage: tests/test_update.sh   (make test-update)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/work/test_update.$$"
FAILS=0 PASSES=0
# isolate from the developer's git config (signing, hooks, default branch, ...)
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1

ok()   { PASSES=$((PASSES + 1)); echo "  ok   $*"; }
fail() { FAILS=$((FAILS + 1));  echo "  FAIL $*"; [ -s "${T:-/nonexistent}/err" ] && sed 's/^/       | /' "$T/err"; }
check() { local what=$1; shift; if "$@"; then ok "$what"; else fail "$what"; fi; }

git_q() { git -c user.name=test -c user.email=test@example.invalid -c init.defaultBranch=main \
              -c advice.detachedHead=false "$@" >/dev/null 2>&1; }

# Stub "make": records calls, writes ./mha2_linux like the real link step (temp + rename),
# fails when the checkout contains BREAK_BUILD.
cat_stub_make() {
    cat > "$T/stub_make" <<'EOF'
#!/usr/bin/env bash
echo "make $*" >> "$STUB_LOG"
[ "${1:-}" = lift ] && { mkdir -p recomp/gen; echo gen > recomp/gen/dispatch.c; exit 0; }
[ -f BREAK_BUILD ] && { echo "error: broken on purpose" >&2; exit 2; }
echo "binary of $(git rev-parse --short HEAD)" > mha2_linux.tmp && chmod +x mha2_linux.tmp && mv -f mha2_linux.tmp mha2_linux
EOF
    chmod +x "$T/stub_make"
}

origin_commit() {  # $1 = file to touch, $2 = message, [$3 = tag]
    git_q -C "$T/work" pull --ff-only
    echo "$2" >> "$T/work/$1"
    git_q -C "$T/work" add -A
    git_q -C "$T/work" commit -m "$2"
    [ -n "${3:-}" ] && git_q -C "$T/work" tag "$3"
    git_q -C "$T/work" push --tags origin main
}

run_update() {  # runs the player's copy of tools/update.sh; sets OUT / RC
    : > "$STUB_LOG"
    OUT=$(cd "$T/player" && env -u MHA2_NO_UPDATE MHA2_UPDATE_MAKE="$T/stub_make" \
          MHA2_DATA_HOME="$T/xdg/mha2-remastered" "$@" tools/update.sh 2>"$T/err")
    RC=$?
}
player_head() { git -C "$T/player" rev-parse HEAD; }
tag_rev()     { git -C "$T/work" rev-parse "$1^{commit}"; }
saves_hash()  { (cd "$T/player" && find game_files/mha2.sav mha2_settings.ini -type f -exec sha256sum {} + | sort | sha256sum); }

setup() {
    rm -rf "$T"; mkdir -p "$T"
    export STUB_LOG="$T/make.log"
    cat_stub_make
    git_q init --bare "$T/origin.git"
    git_q clone "$T/origin.git" "$T/work"
    mkdir -p "$T/work/tools" "$T/work/recomp"
    cp "$ROOT/tools/update.sh" "$T/work/tools/update.sh"
    echo "# lifter v1" > "$T/work/recomp/lift.py"
    printf 'game_files/\nrecomp/gen/\nbuild/\nbackups/\nmha2_linux\nmha2_settings.ini\n' > "$T/work/.gitignore"
    echo "readme" > "$T/work/README.md"
    git_q -C "$T/work" add -A
    git_q -C "$T/work" commit -m "initial"
    git_q -C "$T/work" tag v1.0.0
    git_q -C "$T/work" tag v5-neural-remaster   # old-style tag, must be ignored
    git_q -C "$T/work" push --tags origin main
    git_q clone "$T/origin.git" "$T/player"
    # player data: saves, quick-save id, settings, shared-data-dir saves
    mkdir -p "$T/player/game_files/mha2.sav" "$T/xdg/mha2-remastered/game_files/mha2.sav"
    echo "save slot 1" > "$T/player/game_files/mha2.sav/mha1.sav"
    echo "quick save"  > "$T/player/game_files/mha2.sav/mha9.sav"
    echo "0123456789abcdef" > "$T/player/game_files/mha2_quicksave.id"
    printf 'shader=2\nauto_update=1\n' > "$T/player/mha2_settings.ini"
    echo "xdg save" > "$T/xdg/mha2-remastered/game_files/mha2.sav/mha2.sav"
}

echo "== tools/update.sh"
setup
SAVES0=$(saves_hash)

# 1. first launch: no binary yet -> builds, nothing to update
run_update
check "fresh checkout builds and reports up to date" [ $RC -eq 0 -a -z "$OUT" -a -x "$T/player/mha2_linux" ]
check "first build lifts (no recomp/gen yet)" grep -q '^make lift' "$STUB_LOG"
check "old-style tags are ignored" grep -q 'up to date (newest release: v1.0.0)' "$T/err"

# 2. untagged commits on main never reach players
origin_commit README.md "work in progress"
run_update
check "untagged main commit is not installed" [ "$(player_head)" = "$(tag_rev v1.0.0)" -a -z "$OUT" ]

# 3. new release tag -> fast-forward, backup, rebuild, notice
origin_commit README.md "fix for 1.1" v1.1.0
run_update
check "new release is installed" [ "$(player_head)" = "$(tag_rev v1.1.0)" ]
check "notice names the new version" [ "$OUT" = "Updated to v1.1.0" ]
check "binary rebuilt from the new release" grep -q "binary of $(git -C "$T/work" rev-parse --short v1.1.0)" "$T/player/mha2_linux"
check "saves, quick-save id and settings untouched" [ "$(saves_hash)" = "$SAVES0" ]
BK=$(ls -1d "$T"/player/backups/update-* 2>/dev/null | tail -n 1)
check "backup holds game_files saves" [ -f "$BK/mha2.sav.1/mha1.sav" -a -f "$BK/mha2.sav.1/mha9.sav" ]
check "backup holds shared-data-dir saves" [ -f "$BK/mha2.sav.2/mha2.sav" ]
check "backup holds settings and quick-save id" [ -f "$BK/mha2_settings.ini" -a -f "$BK/mha2_quicksave.id" ]
check "no re-lift when the lifter is unchanged" bash -c "! grep -q '^make lift' '$STUB_LOG'"

# 4. version sort: v1.10.0 > v1.9.0; lifter change triggers a re-lift
origin_commit README.md "1.9" v1.9.0
origin_commit recomp/lift.py "# lifter change" v1.10.0
sleep 1; touch "$T/player/recomp/gen/dispatch.c" 2>/dev/null; sleep 1
run_update
check "newest tag wins by version order (v1.10.0)" [ "$(player_head)" = "$(tag_rev v1.10.0)" -a "$OUT" = "Updated to v1.10.0" ]
check "lifter change re-lifts" grep -q '^make lift' "$STUB_LOG"

# 5. offline -> skip quickly, still start
git -C "$T/player" remote set-url origin "$T/does-not-exist.git"
run_update
check "offline: no update, binary usable" [ $RC -eq 0 -a -z "$OUT" ]
check "offline is reported" grep -q 'could not reach' "$T/err"
git -C "$T/player" remote set-url origin "$T/origin.git"

# 6. local modifications are never overwritten
origin_commit README.md "1.11" v1.11.0
echo "my local tweak" >> "$T/player/README.md"
run_update
check "local changes block the update" [ "$(player_head)" = "$(tag_rev v1.10.0)" -a -z "$OUT" ]
check "local change preserved" grep -q 'my local tweak' "$T/player/README.md"
git -C "$T/player" checkout -q -- README.md

# 7. auto_update=0 / MHA2_NO_UPDATE=1 skip the release check but still build
sed -i 's/auto_update=1/auto_update=0/' "$T/player/mha2_settings.ini"
run_update
check "auto_update=0 skips the update" [ "$(player_head)" = "$(tag_rev v1.10.0)" -a -z "$OUT" ]
sed -i 's/auto_update=0/auto_update=1/' "$T/player/mha2_settings.ini"
run_update MHA2_NO_UPDATE=1
check "MHA2_NO_UPDATE=1 skips the update" [ "$(player_head)" = "$(tag_rev v1.10.0)" -a -z "$OUT" ]
check "build still runs when updates are off" grep -q '^make -j' "$STUB_LOG"

# 8. local commits (diverged) are not touched
git_q -C "$T/player" commit --allow-empty -m "local experiment"
LOCAL=$(player_head)
run_update
check "diverged checkout is not updated" [ "$(player_head)" = "$LOCAL" -a -z "$OUT" ]
git_q -C "$T/player" reset --hard v1.10.0

# 9. broken release: keep the previous binary, notice says so, retry next launch
OLDBIN=$(cat "$T/player/mha2_linux")
git_q -C "$T/work" pull --ff-only
echo broken > "$T/work/BREAK_BUILD"; git_q -C "$T/work" add -A; git_q -C "$T/work" commit -m broken
git_q -C "$T/work" tag v1.12.0; git_q -C "$T/work" push --tags origin main
run_update
check "failed build keeps the previous binary" [ $RC -eq 0 -a "$(cat "$T/player/mha2_linux")" = "$OLDBIN" ]
check "failed build notice" [ "$OUT" = "Update to v1.12.0 failed to build - running the previous version" ]
git_q -C "$T/work" rm -q BREAK_BUILD; git_q -C "$T/work" commit -m "fix build"
git_q -C "$T/work" tag v1.12.1; git_q -C "$T/work" push --tags origin main
run_update
check "next release builds again" [ "$OUT" = "Updated to v1.12.1" ] && grep -q "binary of" "$T/player/mha2_linux"

# 10. no binary and a failing build -> exit 1 (play.sh cannot start)
rm -f "$T/player/mha2_linux"; touch "$T/player/BREAK_BUILD"
run_update
check "no binary + failed build exits 1" [ $RC -eq 1 ]
rm -f "$T/player/BREAK_BUILD"

# 11. a release that rewrites tools/update.sh itself while it runs
{ cat "$ROOT/tools/update.sh"; printf '\n# padding %.0s' $(seq 1 400); echo; } > "$T/work/tools/update.sh.new"
git_q -C "$T/work" pull --ff-only
mv "$T/work/tools/update.sh.new" "$T/work/tools/update.sh"; chmod +x "$T/work/tools/update.sh"
git_q -C "$T/work" add -A; git_q -C "$T/work" commit -m "updater changes"; git_q -C "$T/work" tag v1.13.0
git_q -C "$T/work" push --tags origin main
run_update
check "self-updating updater completes" [ $RC -eq 0 -a "$OUT" = "Updated to v1.13.0" ]

# 12. detached HEAD at an older tag is moved forward
git_q -C "$T/player" checkout --detach v1.10.0
run_update
check "detached checkout follows the newest tag" [ "$(player_head)" = "$(tag_rev v1.13.0)" ]

# 13. backups are capped at 10
ls -1d "$T"/player/backups/update-* >/dev/null 2>&1
for i in $(seq 1 12); do mkdir -p "$T/player/backups/update-20000101-0000$(printf %02d "$i")-old"; done
git_q -C "$T/player" checkout --detach v1.12.1
run_update
check "update backups capped at 10" [ "$(ls -1d "$T"/player/backups/update-* | wc -l)" -eq 10 ]
check "saves still untouched after all updates" [ "$(saves_hash)" = "$SAVES0" ]

rm -rf "$T"
echo "test_update: $PASSES passed, $FAILS failed"
[ $FAILS -eq 0 ]
