#!/usr/bin/env bash
# Polled by a systemd timer. Tests when (a) a new GitHub Release exists, or
# (b) the CS2 build id changed. Outbound connections only; no inbound ports.
#   ci-poll.sh            normal poll
#   ci-poll.sh --force    test the latest release now
set -u

CONFIG="${CST_CI_CONFIG:-/etc/cs2sdktest-ci.env}"
# shellcheck disable=SC1090
source "$CONFIG" || { echo "cannot read $CONFIG" >&2; exit 2; }
: "${GH_REPO:?}" "${CS2_DIR:?}" "${STATE_DIR:?}"
APP_ID="${APP_ID:-730}"; RUN_TIMEOUT="${RUN_TIMEOUT:-600}"
FORCE=0; [ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$STATE_DIR/runs"
exec 9>"$STATE_DIR/lock"
flock -n 9 || exit 0                      # a run is already in progress

log() { echo "[$(date -u +%FT%TZ)] $*"; }
read_state() { cat "$STATE_DIR/$1" 2>/dev/null || true; }

# ---- signal A: latest release ------------------------------------------------
# No api.github.com (no rate limit, no token): the tag comes from where
# /releases/latest redirects to, and the asset name is fixed by build.yml:
# cs2sdktest-linux-x64-<tag>.zip
latest_tag() {
  local final
  final="$(curl -fsSL -o /dev/null -w '%{url_effective}' --max-time 30 \
    "https://github.com/$GH_REPO/releases/latest")" || return 1
  case "$final" in */releases/tag/*) printf '%s' "${final##*/releases/tag/}";; *) return 1;; esac
}
rel_tag="$(latest_tag)" || { log "github unreachable or no release yet"; exit 0; }
asset_url="https://github.com/$GH_REPO/releases/download/$rel_tag/cs2sdktest-linux-x64-$rel_tag.zip"

# ---- signal B: installed game build id --------------------------------------
# Another service keeps CS2 up to date; we only read what is installed.
local_build() { sed -n 's/.*"buildid"[^"]*"\([0-9]*\)".*/\1/p' "$CS2_DIR/steamapps/appmanifest_$APP_ID.acf" 2>/dev/null | head -1; }
state_flags() { sed -n 's/.*"StateFlags"[^"]*"\([0-9]*\)".*/\1/p' "$CS2_DIR/steamapps/appmanifest_$APP_ID.acf" 2>/dev/null | head -1; }
build="$(local_build)"
[ -n "$build" ] || { log "cannot read installed build id"; exit 1; }
# StateFlags 4 = fully installed; anything else means an update is in progress.
[ "$(state_flags)" = "4" ] || { log "game update in progress (StateFlags=$(state_flags)); retry next poll"; exit 0; }

reason=""
[ "$rel_tag" != "$(read_state last_tag)" ] && reason="release"
[ "$build" != "$(read_state last_build)" ] && reason="gameupdate"
[ "$FORCE" = 1 ] && reason="manual"
[ -z "$reason" ] && exit 0
log "triggered by: $reason (release $rel_tag, build $build)"

# ---- deploy the plugin -------------------------------------------------------
run_id="$(date -u +%Y%m%d-%H%M%S)_${rel_tag}_b${build}"
run_dir="$STATE_DIR/runs/$run_id"; mkdir -p "$run_dir"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$asset_url" -o "$tmp/pkg.zip" \
  || { log "download failed"; exit 1; }
unzip -qo "$tmp/pkg.zip" -d "$CS2_DIR/game/csgo" || { log "unzip failed"; exit 1; }

# ---- run the server ----------------------------------------------------------
pkill -f "$LAUNCH_CMD" 2>/dev/null && sleep 3     # stale instance holds the port
export CST_AUTORUN=1 CST_AUTORUN_DELAY="${CST_AUTORUN_DELAY:-15}" CST_RESULT_DIR="$run_dir" CST_BUILD_ID="$build"
start=$(date +%s)
# shellcheck disable=SC2086
timeout -k 10 "$RUN_TIMEOUT" $LAUNCH_CMD $LAUNCH_ARGS >"$run_dir/console.log" 2>&1
code=$?
secs=$(( $(date +%s) - start ))

# ---- judge -------------------------------------------------------------------
if [ -f "$run_dir/results.json" ] && grep -q '"completed":true' "$run_dir/results.json"; then
  status="completed"
elif [ "$code" = 124 ] || [ "$code" = 137 ]; then status="timeout"
else status="crashed"; fi

# Last call that started but never reported: the suspect for a crash or hang.
suspect=""
if [ "$status" != "completed" ] && [ -f "$run_dir/results.jsonl" ]; then
  last="$(tail -n 1 "$run_dir/results.jsonl")"
  case "$last" in *'"ev":"begin"'*) suspect="$last";; esac
fi
# Keep crash evidence if the engine wrote any.
find "$CS2_DIR/game" -maxdepth 3 \( -name '*.mdmp' -o -name 'core*' -o -name '*.dmp' \) -newermt "@$start" \
  -exec cp -t "$run_dir" {} + 2>/dev/null
cp -f "$CS2_DIR"/game/csgo/addons/cs2sdktest/logs/*.log "$run_dir/" 2>/dev/null

esc() { printf '%s' "$1" | sed 's/\/\\/g; s/"/\\"/g'; }
cat >"$run_dir/run.json" <<JSON
{"status":"$status","reason":"$reason","release":"$rel_tag","game_buildid":"$build",
 "exit_code":$code,"duration_s":$secs,"finished":"$(date -u +%FT%TZ)","suspect":"$(esc "$suspect")"}
JSON
log "result: $status (exit $code, ${secs}s) -> $run_dir"

printf '%s' "$rel_tag" >"$STATE_DIR/last_tag"
printf '%s' "$build"   >"$STATE_DIR/last_build"
[ -n "${PUBLISH_HOOK:-}" ] && "$PUBLISH_HOOK" "$run_dir"
exit 0
