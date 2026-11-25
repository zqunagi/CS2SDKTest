#!/usr/bin/env bash
# Runs one autorun sweep on this server, the way the runner will: install the
# package under game/csgo, LD_PRELOAD the library into cs2 with CST_AUTORUN=1,
# and look at what came out.
#
#   ./run-autorun.sh /path/to/package        # build/<preset>/package
#
# The package is the contents of game/csgo (see CMakeLists.txt):
#
#   bin/linuxsteamrt64/cs2sdktest.so
#   configs/gamedata.toml
#   configs/cs2sdktest-meta.json
#
# It is copied over <server>/game/csgo, and run logs and results are then
# written by the library to <server>/game/csgo/logs. gameinfo.gi is not touched.
set -u

PKG="${1:?usage: $0 /path/to/package}"
PKG="$(readlink -f "$PKG")"
[ -f "$PKG/bin/linuxsteamrt64/cs2sdktest.so" ] || { echo "not a package (no bin/linuxsteamrt64/cs2sdktest.so): $PKG"; exit 2; }

CS2_DIR="${CS2_DIR:-/root/cs2-ds}"
CSGO="$CS2_DIR/game/csgo"
RUN_SECONDS="${RUN_SECONDS:-240}"
DELAY="${DELAY:-10}"
LAUNCH_ARGS="${LAUNCH_ARGS:--dedicated +hostport 10001 +game_type 0 +game_mode 1 +map de_dust2 +sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 +mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure}"

# Copy the package in: bin/, configs/ and the metadata land where the library
# looks for them. cp -a keeps the layout; nothing outside these paths is touched.
mkdir -p "$CSGO/bin/linuxsteamrt64" "$CSGO/configs" "$CSGO/logs"
cp -a "$PKG/." "$CSGO/"
SO="$CSGO/bin/linuxsteamrt64/cs2sdktest.so"

echo "== installed"
ls -la "$SO" "$CSGO/configs/gamedata.toml"

echo "== exports of the library (must be empty)"
nm -D --defined-only "$SO" | head -5
echo "== (end of exports)"

# Only this run's output: remember what was in logs/ before.
before="$(mktemp)"; ls -1 "$CSGO/logs" >"$before" 2>/dev/null

export LD_LIBRARY_PATH="$CS2_DIR/game/bin/linuxsteamrt64:${LD_LIBRARY_PATH:-}"
cd "$CS2_DIR/game/bin/linuxsteamrt64" || exit 1
CONSOLE="$(mktemp)"

# LD_PRELOAD takes the absolute path, and the library works out <csgo> from it;
# a relative path would be reported to it as given and defeat that.
# shellcheck disable=SC2086
timeout -k 10 "$RUN_SECONDS" env \
	LD_PRELOAD="$SO" CST_AUTORUN=1 CST_AUTORUN_DELAY="$DELAY" \
	./cs2 $LAUNCH_ARGS >"$CONSOLE" 2>&1 </dev/null
echo "cs2 exit code: $?"

echo "== autorun markers in the console"
grep -nE "SDKTEST|autorun:|game directory" "$CONSOLE" | head -40

echo "== new files in $CSGO/logs"
ls -1 "$CSGO/logs" | grep -vxFf "$before" | while read -r f; do ls -la "$CSGO/logs/$f"; done
[ -f "$CSGO/logs/results.json" ] && head -c 600 "$CSGO/logs/results.json" && echo
[ -f "$CSGO/logs/crash.json" ] && { echo "== crash.json"; head -c 1500 "$CSGO/logs/crash.json"; echo; }
echo "console log kept at $CONSOLE"
