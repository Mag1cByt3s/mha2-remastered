#!/usr/bin/env bash
# Launcher for source installs (any distro; uses nix-shell when available):
#   1. tools/update.sh - fast-forward to the newest release tag (saves are backed up and
#      never touched), rebuild incrementally; offline or local changes just skip the update
#   2. start ./mha2_linux with all arguments (an "Updated to vX" toast shows at the main menu)
# --no-update (or MHA2_NO_UPDATE=1, or auto_update=0 in mha2_settings.ini) skips step 1's
# release check; the build-if-needed part still runs.

in_env() {  # run a command inside the build environment
    if command -v nix-shell >/dev/null 2>&1 && [ -f shell.nix ]; then
        nix-shell --run "$(printf '%q ' "$@")"
    else
        "$@"
    fi
}

main() {
    cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
    local args=() a notice
    for a in "$@"; do
        if [ "$a" = --no-update ]; then export MHA2_NO_UPDATE=1; else args+=("$a"); fi
    done
    if ! notice=$(in_env tools/update.sh); then
        echo "play.sh: ./mha2_linux could not be built - see build/update.log" >&2
        exit 1
    fi
    export MHA2_UPDATE_NOTICE="$notice"
    in_env ./mha2_linux "${args[@]}"
}

# Parsed completely before running, so an update that rewrites this file is harmless.
main "$@"; exit $?
