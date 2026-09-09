#!/bin/sh
# 检查 SDK 类的虚方法是否在 suite 中调用或 SkipCall。
# 用法：audit-coverage.sh <header> <ClassName> <suite.cpp> [more.cpp ...]
# 退出码为未覆盖方法数量。

set -u

if [ $# -lt 3 ]; then
    echo "用法: $0 <header> <ClassName> <suite.cpp> [more.cpp ...]" >&2
    exit 2
fi

HEADER=$1
CLASS=$2
shift 2

[ -r "$HEADER" ] || { echo "无法读取头文件: $HEADER" >&2; exit 2; }
for f in "$@"; do
    [ -r "$f" ] || { echo "无法读取源文件: $f" >&2; exit 2; }
done

# 精确匹配类名，并在提取前去掉注释。
METHODS=$(awk -v cls="$CLASS" '
    {
        if (inblock) {
            if (match($0, /\*\//)) { $0 = substr($0, RSTART + RLENGTH); inblock = 0 }
            else { next }
        }
        while (match($0, /\/\*.*\*\//)) { $0 = substr($0, 1, RSTART - 1) substr($0, RSTART + RLENGTH) }
        if (match($0, /\/\*/)) { $0 = substr($0, 1, RSTART - 1); inblock = 1 }
        sub(/\/\/.*$/, "")
        if ($0 ~ /^[ \t]*$/) next
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
    inside && /^};/ { exit }
    inside {
        buf = buf " " $0
        if (index(buf, "(") == 0) next
        decl = buf
        buf = ""
        if (decl !~ /virtual/) next
        sub(/\(.*$/, "", decl)
        sub(/[ \t]+$/, "", decl)
        if (match(decl, /~?[A-Za-z_][A-Za-z0-9_]*$/)) {
            name = substr(decl, RSTART, RLENGTH)
            if (name != "virtual") print name
        }
    }
' "$HEADER" | sort -u)

if [ -z "$METHODS" ]; then
    echo "未找到 '$CLASS' 的虚方法: $HEADER" >&2
    echo "请确认类名完全匹配" >&2
    exit 2
fi

TOTAL=$(printf '%s\n' "$METHODS" | wc -l | tr -d ' ')
MISSING=0
for m in $METHODS; do
    case $m in
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
        echo "$CLASS: 未覆盖的方法"
    fi
    echo "  $m"
    MISSING=`expr "$MISSING" + 1`
done

if [ "$MISSING" -eq 0 ]; then
    echo "$CLASS: 全部 $TOTAL 个方法已覆盖"
else
    echo "$CLASS: $TOTAL 个方法中有 $MISSING 个未覆盖"
fi

exit "$MISSING"
