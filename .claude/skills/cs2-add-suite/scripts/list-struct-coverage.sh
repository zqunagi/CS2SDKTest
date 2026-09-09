#!/bin/sh
# List every struct already inspected by a suite, and which suite owns it.
#
# Struct layout checks are expensive to read, not to run: a second suite
# re-inspecting CGlobalVars adds thirty lines to the log and zero information,
# because a layout either holds or it does not - it cannot hold in one suite
# and fail in another. So each struct is inspected exactly once, by the suite
# that most naturally reaches it, and every other suite that touches it leaves
# a comment pointing at the owner.
#
# Run this before adding StructInspector calls to a new suite, to find out
# whether the struct is already somebody else's job.
#
# Usage:
#   list-struct-coverage.sh [suites-dir]        # default: src/suites

set -u
DIR=${1:-src/suites}

[ -d "$DIR" ] || { echo "no such directory: $DIR" >&2; exit 2; }

# The type_name argument to StructInspector's constructor is the log label, and
# by convention it is the struct's real name (sometimes with an "[0]" suffix
# where the instance is an array element). That makes it a reliable index.
awk '
    FNR == 1 {
        suite = FILENAME
        gsub(/^.*\//, "", suite)
        sub(/\.cpp$/, "", suite)
    }
    # Match the constructor and capture the first string literal after it.
    /StructInspector[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\([ \t]*context[ \t]*,/ {
        line = $0
        sub(/^.*context[ \t]*,[ \t]*"/, "", line)
        sub(/".*$/, "", line)
        sub(/\[0\]$/, "", line)
        if (line != "") { owner[line] = suite }
    }
    END {
        n = 0
        for (s in owner) { n++ }
        if (n == 0) { print "  (none found)"; exit }
        printf "%-34s  %s\n", "struct", "owning suite"
        printf "%-34s  %s\n", "----------------------------------", "------------------------"
        for (s in owner) { printf "%-34s  %s\n", s, owner[s] }
    }
' "$DIR"/*.cpp | { read -r h1; read -r h2; printf '%s\n%s\n' "$h1" "$h2"; sort; }

cat <<'NOTE'

A struct listed above is already covered. In a new suite, reference it with a
comment instead of a second StructInspector block:

  // CGlobalVars' layout is inspected in Source2EngineSuite. A layout either
  // holds or it does not - it cannot hold there and fail here - so repeating
  // the check would add log noise and no evidence.
NOTE
