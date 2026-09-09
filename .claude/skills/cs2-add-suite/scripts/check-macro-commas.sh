#!/bin/sh
# 检查 CS2ST_CALL 宏体内是否有会被当成宏参数分隔符的逗号。
#
# CS2ST_CALL(context, target, body) 是函数式宏，预处理器只用圆括号和方括号
# 保护逗号，花括号和尖括号不保护。所以 body 里的
#   const Row t[] = { {"a", 1}, {"b", 2} };
#   std::map<Key, Value> m;
#   int a = 1, b = 2;
# 都会变成第四、第五个宏参数，报
#   too many arguments provided to function-like macro invocation
#
# 用法：check-macro-commas.sh <file.cpp> [more.cpp ...]
# 退出码为有问题的行数。

set -u

if [ $# -lt 1 ]; then
    echo "用法: $0 <file.cpp> [more.cpp ...]" >&2
    exit 2
fi

for f in "$@"; do
    [ -r "$f" ] || { echo "无法读取: $f" >&2; exit 2; }
done

awk '
    FNR == 1 {
        if (NR > 1) scan()
        delete lstart
        delete ltext
        src = ""
        total = 0
        nlines = 0
        fname = FILENAME
    }
    {
        nlines++
        ltext[nlines] = $0
        lstart[nlines] = total + 1
        total += length($0) + 1
        src = src $0 "\n"
    }
    END { scan() }

    function line_of(pos,    lo, hi, mid) {
        lo = 1; hi = nlines
        while (lo < hi) {
            mid = int((lo + hi + 1) / 2)
            if (lstart[mid] <= pos) lo = mid; else hi = mid - 1
        }
        return lo
    }

    function scan(    n, i, p, start, depth, j, c, two, instr, inchr, inlc, inbc,
                      args, np, positions, k, pos, ln) {
        if (src == "") return
        n = length(src)
        i = 1
        while (i <= n) {
            p = index(substr(src, i), "CS2ST_CALL(")
            if (p == 0) break
            start = i + p - 1 + 11        # just past the opening paren
            depth = 1
            j = start
            instr = 0; inchr = 0; inlc = 0; inbc = 0
            args = 1
            np = 0
            delete positions
            while (j <= n && depth > 0) {
                c = substr(src, j, 1)
                two = substr(src, j, 2)
                if (inlc) {
                    if (c == "\n") inlc = 0
                } else if (inbc) {
                    if (two == "*/") { inbc = 0; j++ }
                } else if (instr) {
                    if (c == "\\") j++
                    else if (c == "\"") instr = 0
                } else if (inchr) {
                    if (c == "\\") j++
                    else if (c == "'"'"'") inchr = 0
                } else if (two == "//") {
                    inlc = 1; j++
                } else if (two == "/*") {
                    inbc = 1; j++
                } else if (c == "\"") {
                    instr = 1
                } else if (c == "'"'"'") {
                    inchr = 1
                } else if (c == "(" || c == "[") {
                    depth++
                } else if (c == ")" || c == "]") {
                    depth--
                } else if (c == "," && depth == 1) {
                    args++; np++; positions[np] = j
                }
                j++
            }
            # Two commas are the macro separators; anything beyond is a comma
            # the body leaked into the argument list.
            if (args > 3) {
                for (k = 3; k <= np; k++) {
                    pos = positions[k]
                    ln = line_of(pos)
                    if (!(fname ":" ln in seen)) {
                        seen[fname ":" ln] = 1
                        sub(/^[ \t]+/, "", ltext[ln])
                        printf "%s:%d: %s\n", fname, ln, ltext[ln]
                        bad++
                    }
                }
            }
            i = start
        }
    }
    END {
        if (bad > 0) {
            printf "\n%d 行的逗号会被 CS2ST_CALL 当成宏参数分隔符。\n", bad
            print "把表和多参模板提到宏外（匿名命名空间或返回值的辅助函数），宏内只留循环。"
        } else {
            print "CS2ST_CALL: 宏体内没有会泄漏成宏参数的逗号"
        }
        exit bad
    }
' "$@"

exit $?
