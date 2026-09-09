#!/bin/sh
# Report which methods of an SDK class a suite has neither called nor SkipCall'd.
#
# The whole point of this project is that a silently missing method is
# indistinguishable from a method somebody checked and found fine. This is the
# mechanical check for that: it reads the class out of the SDK header and asks,
# for each method, whether the suite source mentions it at all.
#
# Usage:
#   audit-coverage.sh <header> <ClassName> <suite.cpp> [<suite2.cpp> ...]
#
# Exit status is the number of unaccounted methods, so it composes:
#   audit-coverage.sh ... || echo "gaps found"
#
# POSIX sh + awk on purpose - no python, no perl. This has to run in whatever
# shell the build happens in, including a bare WSL container.

set -u

if [ $# -lt 3 ]; then
    echo "usage: $0 <header> <ClassName> <suite.cpp> [more.cpp ...]" >&2
    exit 2
fi

HEADER=$1
CLASS=$2
shift 2

[ -r "$HEADER" ] || { echo "cannot read header: $HEADER" >&2; exit 2; }
for f in "$@"; do
    [ -r "$f" ] || { echo "cannot read source: $f" >&2; exit 2; }
done

# ---- extract the class's method names -------------------------------------
#
# Anchored on an EXACT class name. This matters more than it looks: matching
# "ISchemaSystem" as a substring also hits "ISchemaSystemTypeScope", which
# silently audits the wrong class and reports everything as covered. That bug
# cost a full review cycle, hence the exact comparison on the parsed name.
METHODS=$(awk -v cls="$CLASS" '
    # Strip comments first. A commented-out declaration is not in the vtable,
    # so reporting it as "untested" sends someone hunting for a method that
    # does not exist - eiface.h has exactly one of these (SetCommandClient).
    {
        if (inblock) {
            if (match($0, /\*\//)) { $0 = substr($0, RSTART + RLENGTH); inblock = 0 }
            else { next }
        }
        while (match($0, /\/\*.*\*\//)) { $0 = substr($0, 1, RSTART - 1) substr($0, RSTART + RLENGTH) }
        if (match($0, /\/\*/)) { $0 = substr($0, 1, RSTART - 1); inblock = 1 }
        sub(/\/\/.*$/, "")
        if ($0 ~ /^[ 	]*$/) next
    }
    !inside {
        line = $0
        sub(/^[ \t]+/, "", line)
        if (line ~ /^(abstract_class|class|struct)[ \t]+/) {
            n = line
            sub(/^(abstract_class|class|struct)[ \t]+/, "", n)
            sub(/[ \t]*[:{].*$/, "", n)
            sub(/[ \t]+$/, "", n)
            if (n == cls) { inside = 1 }
        }
        next
    }
    # A "};" flush against the left margin ends the outer class. Nested structs
    # in these headers are indented, so their closers do not match this.
    inside && /^};/ { exit }
    inside {
        # Accumulate a declaration until it has a "(" - Valve wraps long ones
        # across lines, and the name sits on whichever line reaches the paren.
        buf = buf " " $0
        if (index(buf, "(") == 0) next
        decl = buf
        buf = ""
        if (decl !~ /virtual/) next
        # Cut from the first "(" and take the trailing identifier, which
        # survives return types like SchemaMetaInfoHandle_t<CFoo>.
        sub(/\(.*$/, "", decl)
        sub(/[ \t]+$/, "", decl)
        if (match(decl, /~?[A-Za-z_][A-Za-z0-9_]*$/)) {
            name = substr(decl, RSTART, RLENGTH)
            if (name != "virtual") print name
        }
    }
' "$HEADER" | sort -u)

if [ -z "$METHODS" ]; then
    echo "no virtual methods found for '$CLASS' in $HEADER" >&2
    echo "  (check the exact class name - the anchor is an exact match)" >&2
    exit 2
fi

TOTAL=$(printf '%s\n' "$METHODS" | wc -l | tr -d ' ')

# ---- check each against the suite sources ---------------------------------
#
# A method counts as accounted for if the suite names it in a log target
# ("Class::Method"), calls it through a pointer, or instantiates it as a
# template. SkipCall lines match the first form, which is deliberate: an
# explicit skip carrying a reason IS coverage as far as this project is
# concerned. What we are hunting is the method nobody wrote a line about.
MISSING=0
for m in $METHODS; do
    case $m in
        # A destructor appears as "Class::~Class", so the "::" is followed by a
        # tilde and the plain-name pattern would miss it.
        '~'*)
            stem=`printf '%s' "$m" | sed 's/^~//'`
            pat="::~${stem}"
            ;;
        *)
            pat="::${m}([^A-Za-z0-9_]|\$)|->${m}[(<]|[.]${m}[(<]|[^A-Za-z0-9_]${m}<"
            ;;
    esac
    if grep -qE "$pat" "$@"; then
        continue
    fi
    if [ "$MISSING" -eq 0 ]; then
        echo "$CLASS: unaccounted methods"
    fi
    echo "  $m"
    MISSING=`expr $MISSING + 1`
done

if [ "$MISSING" -eq 0 ]; then
    echo "$CLASS: all $TOTAL method(s) accounted for"
else
    echo "$CLASS: $MISSING of $TOTAL unaccounted"
fi

exit "$MISSING"
