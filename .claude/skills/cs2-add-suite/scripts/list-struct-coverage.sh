#!/bin/sh
# 列出已由 suite 检查的结构体及其归属。
# 用法：
#   list-struct-coverage.sh [suites-dir]        # default: src/suites

set -u
DIR=${1:-src/suites}

[ -d "$DIR" ] || { echo "目录不存在: $DIR" >&2; exit 2; }

# StructInspector 的 type_name 按约定是结构体名，可作为登记键。
awk '
    FNR == 1 {
        suite = FILENAME
        gsub(/^.*\//, "", suite)
        sub(/\.cpp$/, "", suite)
    }
    # 获取构造函数中 context 后的第一个字符串。
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
        printf "%-34s  %s\n", "结构体", "负责的 suite"
        printf "%-34s  %s\n", "----------------------------------", "------------------------"
        for (s in owner) { printf "%-34s  %s\n", s, owner[s] }
    }
' "$DIR"/*.cpp | { read -r h1; read -r h2; printf '%s\n%s\n' "$h1" "$h2"; sort; }

cat <<'NOTE'

以上结构体已有覆盖。新 suite 只需注释其归属，不要重复添加 StructInspector：

  // CGlobalVars 的布局由 Source2EngineSuite 检查，此处不重复输出。
NOTE
