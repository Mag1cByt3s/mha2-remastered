#!/usr/bin/env bash
# Print the CHANGELOG.md section of one release as an annotated-tag message:
#   line 1: "vX.Y.Z — <title>", blank line, then the section body.
# Usage: tools/release_notes.sh vX.Y.Z            (exit 1 if the section is missing)
#        git tag -a vX.Y.Z -F <(tools/release_notes.sh vX.Y.Z)
set -euo pipefail
ver=${1:?usage: tools/release_notes.sh vX.Y.Z}
changelog="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/CHANGELOG.md"

awk -v ver="$ver" '
    BEGIN { pfx = "## [" ver " " }
    index($0, "## [") == 1 {
        if (found) exit
        if (index($0, pfx) == 1) {
            found = 1
            title = substr($0, 5)
            sub(/\][^]]*$/, "", title)          # drop "] (date)"
            print title
            print ""
            next
        }
    }
    found {
        if ($0 == "---") next                    # section separator, not part of the notes
        lines[++n] = $0
    }
    END {
        if (!found) { print "release_notes: no \"## [" ver " ...\" section in CHANGELOG.md" > "/dev/stderr"; exit 1 }
        while (n > 0 && lines[n] == "") n--      # trim trailing blank lines
        start = 1
        while (start <= n && lines[start] == "") start++
        for (i = start; i <= n; i++) print lines[i]
    }
' "$changelog"
