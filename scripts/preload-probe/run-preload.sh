#!/usr/bin/env bash
# Phase 1 check of the Metamod-free build, on the server. Does what the runner
# will do later: LD_PRELOAD the library into cs2 with CST_AUTORUN=1 and look at
# what came out.
#
#   ./run-preload.sh /path/to/cs2sdktest_preload.so
#
# Nothing is installed and gameinfo.gi is not touched. Run it on a server whose
# gameinfo.gi has no Metamod line to see the case this build exists for.
set -u

SO="${1:?usage: $0 /path/to/cs2sdktest_preload.so}"
SO="$(readlink -f "$SO")"
[ -f "$SO" ] || { echo "no such file: $SO"; exit 2; }

CS2_DIR="${CS2_DIR:-/root/cs2-ds}"
OUT_DIR="${OUT_DIR:-/tmp/cs2sdktest-preload}"
RUN_SECONDS="${RUN_SECONDS:-240}"
DELAY="${DELAY:-30}"
LAUNCH_ARGS="${LAUNCH_ARGS:--dedicated +hostport 10001 +game_type 0 +game_mode 1 +map de_dust2 +sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 +mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure}"

rm -rf "$OUT_DIR"; mkdir -p "$OUT_DIR"

echo "== exports of the library (must be empty)"
nm -D --defined-only "$SO" | head -5
echo "== (end of exports)"

export LD_LIBRARY_PATH="$CS2_DIR/game/bin/linuxsteamrt64:${LD_LIBRARY_PATH:-}"
cd "$CS2_DIR/game/bin/linuxsteamrt64" || exit 1

# shellcheck disable=SC2086
timeout -k 10 "$RUN_SECONDS" env \
	LD_PRELOAD="$SO" CST_AUTORUN=1 CST_AUTORUN_DELAY="$DELAY" CST_RESULT_DIR="$OUT_DIR" \
	./cs2 $LAUNCH_ARGS >"$OUT_DIR/console.log" 2>&1 </dev/null
echo "cs2 exit code: $?"

echo "== autorun markers in console.log"
grep -nE "SDKTEST|autorun:" "$OUT_DIR/console.log" | head -40
echo "== files written"
ls -la "$OUT_DIR"
[ -f "$OUT_DIR/results.json" ] && head -c 600 "$OUT_DIR/results.json" && echo
[ -f "$OUT_DIR/crash.json" ] && { echo "== crash.json"; head -c 1500 "$OUT_DIR/crash.json"; echo; }
