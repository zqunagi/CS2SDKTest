#!/usr/bin/env bash
# Runs the LD_PRELOAD probe against the CS2 dedicated server. Read probe.c first.
#
#   ./run-probe.sh                # build, run for ~90s, print the probe log
#   PROBE_SERVERCOMMAND_SLOT=NN ./run-probe.sh
#
# It does not touch gameinfo.gi and does not need Metamod. To test the
# "Metamod is broken" case honestly, run it on a server whose gameinfo.gi has
# no Metamod line.
set -u

CS2_DIR="${CS2_DIR:-/root/cs2-ds}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${OUT_DIR:-/tmp/preload-probe}"
RUN_SECONDS="${RUN_SECONDS:-90}"
# Same arguments the Runner uses (config.psd1 RemoteLaunchArgs default), so the probe
# sees the engine in the state the real run does. Override with LAUNCH_ARGS.
LAUNCH_ARGS="${LAUNCH_ARGS:--dedicated +hostport 10001 +game_type 0 +game_mode 1 +map de_dust2 +sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 +mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure}"

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/probe.log "$OUT_DIR"/console.log

gcc -O1 -shared -fPIC -o "$OUT_DIR/probe.so" "$HERE/${PROBE_SRC:-probe.c}" -ldl -lpthread || exit 1

export LD_LIBRARY_PATH="$CS2_DIR/game/bin/linuxsteamrt64:${LD_LIBRARY_PATH:-}"
export PROBE_OUT="$OUT_DIR/probe.log"
export PROBE_DELAY="${PROBE_DELAY:-30}"
# LD_PRELOAD is applied to the cs2 command only. Exporting it would also load the
# probe into every child process the shell starts afterwards (timeout, cat, grep),
# which is what made the first run's log confusing - one log, several pids.
echo "PROBE_OUT=$PROBE_OUT PROBE_DELAY=$PROBE_DELAY PROBE_SERVERCOMMAND_SLOT=${PROBE_SERVERCOMMAND_SLOT:-<unset>}"

cd "$CS2_DIR/game/bin/linuxsteamrt64" || exit 1
# shellcheck disable=SC2086  # LAUNCH_ARGS is meant to word-split
timeout -k 10 "$RUN_SECONDS" env LD_PRELOAD="$OUT_DIR/probe.so" ./cs2 $LAUNCH_ARGS \
	>"$OUT_DIR/console.log" 2>&1 </dev/null
echo "cs2 exit code: $?"

echo "---- probe.log ----"
cat "$OUT_DIR/probe.log" 2>/dev/null || echo "(no probe.log: the library was never loaded)"
echo "---- console.log lines mentioning the probe ----"
grep -n "PROBE_\|ld.so\|LD_PRELOAD" "$OUT_DIR/console.log" | head -20
