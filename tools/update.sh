#!/usr/bin/env bash
# Automatic updater for source installs (a git checkout of mha2-remastered).
#
# play.sh runs this before every launch. It
#   1. fetches release tags (vMAJOR.MINOR[.PATCH]) with a short timeout - offline is fine,
#   2. fast-forwards the checkout to the newest release tag, but only if the tracked files
#      are unmodified and HEAD is behind that tag (local work is never touched),
#   3. backs up savegames + settings to backups/update-<time>-<rev>/ before changing code,
#   4. re-lifts (only when recomp/lift.py changed) and rebuilds incrementally. `make` links
#      to a temp file and renames it, so a failed build keeps the previous ./mha2_linux.
#
# Savegames (game_files/mha2.sav/ or ~/.local/share/mha2-remastered/game_files/mha2.sav/),
# mha2_settings.ini, game_files/ and hd_assets/ are gitignored: git never modifies them.
#
# Skip the release check (still builds if needed): MHA2_NO_UPDATE=1, play.sh --no-update,
# or auto_update=0 in mha2_settings.ini.
#
# stdout carries at most one line - the in-game notice, e.g. "Updated to v5.5.0" - and
# everything else goes to stderr. Exit status: 0 = ./mha2_linux is usable, 1 = no binary.
#
# Test hooks: MHA2_UPDATE_MAKE (build command, default "make"), MHA2_UPDATE_REMOTE
# (default "origin"), MHA2_UPDATE_TIMEOUT (fetch seconds, default 20), MHA2_DATA_HOME.

say() { echo "[update] $*" >&2; }

release_tags() {
    git tag -l 'v*' | grep -E '^v[0-9]+\.[0-9]+(\.[0-9]+)?$' | sort -V
}

auto_update_disabled() {
    [ "${MHA2_NO_UPDATE:-0}" = 1 ] && return 0
    [ -f mha2_settings.ini ] && grep -qE '^[[:space:]]*auto_update[[:space:]]*=[[:space:]]*0' mha2_settings.ini
}

backup_user_data() {  # $1 = short old revision
    local data_home dest src n=0
    data_home=${MHA2_DATA_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/mha2-remastered}
    mkdir -p backups || return 1
    dest=$(mktemp -d "backups/update-$(date +%Y%m%d-%H%M%S)-$1-XXXX") || return 1
    for src in game_files/mha2.sav "$data_home/game_files/mha2.sav"; do
        [ -d "$src" ] || continue
        n=$((n + 1))
        cp -a "$src" "$dest/mha2.sav.$n" || return 1
        echo "$src" > "$dest/mha2.sav.$n.origin"
    done
    [ -f mha2_settings.ini ] && { cp -a mha2_settings.ini "$dest/" || return 1; }
    [ -f game_files/mha2_quicksave.id ] && { cp -a game_files/mha2_quicksave.id "$dest/" || return 1; }
    say "saves and settings backed up to $dest"
    # keep the 10 newest update backups (each is a few hundred KB)
    ls -1d backups/update-* 2>/dev/null | sort | head -n -10 | while read -r old; do rm -rf -- "$old"; done
    return 0
}

update_source() {  # prints the new version on stdout when the checkout moved
    local remote latest old_rev
    remote=${MHA2_UPDATE_REMOTE:-origin}
    if ! GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh -o BatchMode=yes -o ConnectTimeout=10}" \
         timeout "${MHA2_UPDATE_TIMEOUT:-20}" git fetch --quiet --tags --force "$remote" 2>/dev/null; then
        say "could not reach '$remote' (offline?) - skipping the update check"
        return 0
    fi
    latest=$(release_tags | tail -n 1)
    if [ -z "$latest" ]; then
        say "no release tags published yet"
        return 0
    fi
    if git merge-base --is-ancestor "$latest" HEAD 2>/dev/null; then
        say "up to date (newest release: $latest)"
        return 0
    fi
    if ! git diff --quiet || ! git diff --cached --quiet; then
        say "$latest is available, but tracked files have local changes - not updating."
        say "  keep them: git stash && tools/update.sh && git stash pop"
        return 0
    fi
    if ! git merge-base --is-ancestor HEAD "$latest"; then
        say "$latest is available, but this checkout has local commits - update manually (git merge $latest)."
        return 0
    fi
    old_rev=$(git rev-parse --short HEAD)
    backup_user_data "$old_rev" || { say "save backup failed - not updating"; return 0; }
    if git symbolic-ref -q HEAD >/dev/null; then
        git merge --ff-only --quiet "$latest" || { say "fast-forward to $latest failed - not updating"; return 0; }
    else
        git checkout --quiet --detach "$latest" || { say "checkout of $latest failed - not updating"; return 0; }
    fi
    say "updated source $old_rev -> $latest"
    echo "$latest"
}

build() {  # $1 = 1 after a source update; incremental, re-lifts only when the lifter changed
    local make_cmd log
    make_cmd=${MHA2_UPDATE_MAKE:-make}
    log=build/update.log
    mkdir -p build
    : > "$log"
    if [ ! -f recomp/gen/dispatch.c ] || [ recomp/lift.py -nt recomp/gen/dispatch.c ]; then
        say "re-lifting mha2.exe (recompiler changed, takes a minute)..."
        $make_cmd lift >> "$log" 2>&1 || return 1
    fi
    if [ "$1" = 1 ] || [ ! -x mha2_linux ]; then
        say "building (this can take a few minutes)..."
    fi
    $make_cmd -j"$(nproc 2>/dev/null || echo 2)" >> "$log" 2>&1
}

main() {
    local root new_version=""
    root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd) || exit 1
    cd "$root" || exit 1

    if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        if auto_update_disabled; then
            say "automatic updates are off (MHA2_NO_UPDATE / --no-update / auto_update=0)"
        else
            new_version=$(update_source)
        fi
    else
        say "not a git checkout - automatic updates need a 'git clone' install"
    fi

    if build "$([ -n "$new_version" ] && echo 1 || echo 0)"; then
        [ -n "$new_version" ] && echo "Updated to $new_version"
        return 0
    fi
    say "build failed - see $root/build/update.log:"
    tail -n 15 build/update.log >&2
    if [ -x mha2_linux ]; then
        say "starting the previous version; the build is retried on the next launch"
        [ -n "$new_version" ] && echo "Update to $new_version failed to build - running the previous version"
        return 0
    fi
    return 1
}

# Everything above is only definitions: bash has read the whole file before main runs,
# so a `git merge` that rewrites this script mid-run cannot corrupt the running copy.
main "$@"; exit $?
