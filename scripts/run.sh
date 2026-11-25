#!/usr/bin/env bash
# Runs the CS2SDKTest sweep against this server's CS2: preloads the library into
# the dedicated server, lets it test every interface once, and exits.
#
# Lives next to the library, in <csgo>/bin/linuxsteamrt64, and finds everything
# else from there. The server's console output stays on your terminal, and
# anything you type goes to the engine's console as usual.
#
#   bash run.sh [options] [-- <extra cs2 arguments>]
#
# Options (all optional; the default in brackets):
#   --delay N         seconds on a simulating map before the tests start  [10]
#   --suite NAME      run only this suite                                 [all]
#   --keep-running    leave the server running after the tests finish     [quit]
#   --server-dir DIR  the server directory, the one that contains game/   [derived]
#   --map NAME        map to start on                                     [de_dust2]
#   --port N          host port                                           [10001]
#   --build-id ID     the game's Steam build id, copied into results.json [none]
#   --result-dir DIR  write results.json, crash.json, ... here            [<csgo>/logs]
#   --                everything after this is appended to the cs2 command line
#   -h, --help        this text
#
# The cs2 arguments used by default are the ones the automated runs use; change
# the map and port with the options above, or add more after "--".
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

delay=""
suite=""
keep_running=0
server_dir=""
map="de_dust2"
port="10001"
build_id=""
result_dir=""
extra=()

usage() { awk 'NR==1{next} /^set -u/{exit} {sub(/^# ?/,""); print}' "${BASH_SOURCE[0]}"; }

need_value() { [ "$#" -ge 2 ] && [ -n "$2" ] || { echo "run.sh: $1 needs a value" >&2; exit 2; }; }

while [ "$#" -gt 0 ]; do
	case "$1" in
		--delay)       need_value "$@"; delay="$2"; shift 2 ;;
		--suite)       need_value "$@"; suite="$2"; shift 2 ;;
		--keep-running) keep_running=1; shift ;;
		--server-dir)  need_value "$@"; server_dir="$2"; shift 2 ;;
		--map)         need_value "$@"; map="$2"; shift 2 ;;
		--port)        need_value "$@"; port="$2"; shift 2 ;;
		--build-id)    need_value "$@"; build_id="$2"; shift 2 ;;
		--result-dir)  need_value "$@"; result_dir="$2"; shift 2 ;;
		-h|--help)     usage; exit 0 ;;
		--)            shift; extra=("$@"); break ;;
		*)             echo "run.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
	esac
done

case "$delay" in ''|*[!0-9]*) [ -z "$delay" ] || { echo "run.sh: --delay must be a whole number of seconds" >&2; exit 2; } ;; esac
case "$port"  in ''|*[!0-9]*) echo "run.sh: --port must be a number" >&2; exit 2 ;; esac

# <server>/game/csgo/bin/linuxsteamrt64  ->  <server>
lib="$here/libcs2sdktest.so"
[ -f "$lib" ] || { echo "run.sh: $lib not found; run this from the unpacked package under <csgo>/bin/linuxsteamrt64" >&2; exit 2; }
if [ -z "$server_dir" ]; then
	server_dir="$(cd "$here/../../../.." && pwd)"
fi
server_dir="$(cd "$server_dir" 2>/dev/null && pwd)" || { echo "run.sh: --server-dir does not exist" >&2; exit 2; }

cs2_dir="$server_dir/game/bin/linuxsteamrt64"
[ -x "$cs2_dir/cs2" ] || { echo "run.sh: no executable cs2 at $cs2_dir/cs2 (is $server_dir the directory that contains game/?)" >&2; exit 2; }

# Never start a second server on top of one somebody is already running.
for pid in $(pgrep -x cs2 2>/dev/null); do
	if [ "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" = "$(readlink -f "$cs2_dir/cs2")" ]; then
		echo "run.sh: a cs2 from this server is already running (pid $pid)" >&2
		exit 3
	fi
done

# The engine's own libraries live next to the executable.
export LD_LIBRARY_PATH="$cs2_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# What the library reads (see the README). Only set what was asked for; the
# library has its own defaults for the rest.
export CST_RUN=1
[ -n "$delay" ]      && export CST_RUN_DELAY="$delay"
[ -n "$suite" ]      && export CST_RUN_SUITE="$suite"
[ "$keep_running" = 1 ] && export CST_RUN_QUIT=0
[ -n "$build_id" ]   && export CST_BUILD_ID="$build_id"
[ -n "$result_dir" ] && export CST_RESULT_DIR="$result_dir"

cd "$cs2_dir" || exit 1

echo "run.sh: preloading $lib"
echo "run.sh: server $server_dir, map $map, port $port${delay:+, delay ${delay}s}${suite:+, suite $suite}"

# exec: cs2 replaces this shell, so its output is yours, Ctrl-C reaches it
# directly, and its exit status is this script's. LD_PRELOAD is given as an
# absolute path: the library works out where the game directory is from where it
# was loaded.
exec env LD_PRELOAD="$lib" ./cs2 \
	-dedicated +hostport "$port" +game_type 0 +game_mode 1 +map "$map" \
	+sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 \
	+mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure \
	${extra[@]+"${extra[@]}"}
