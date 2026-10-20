#!/usr/bin/env bash
# Polled by a systemd timer. Tests when (a) a new GitHub Release exists, or
# (b) the CS2 build id changed. Outbound connections only; no inbound ports.
#   ci-poll.sh            normal poll
#   ci-poll.sh --force    test the latest release now
set -u

CONFIG="${CST_CI_CONFIG:-/etc/cs2sdktest-ci.env}"
# shellcheck disable=SC1090
source "$CONFIG" || { echo "cannot read $CONFIG" >&2; exit 2; }
: "${GH_REPO:?}" "${CS2_DIR:?}" "${STEAMCMD:?}" "${STATE_DIR:?}"
APP_ID="${APP_ID:-730}"; RUN_TIMEOUT="${RUN_TIMEOUT:-600}"
FORCE=0; [ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$STATE_DIR/runs"
exec 9>"$STATE_DIR/lock"
flock -n 9 || exit 0                      # a run is already in progress

log() { echo "[$(date -u +%FT%TZ)] $*"; }
read_state() { cat "$STATE_DIR/$1" 2>/dev/null || true; }

# ---- signal A: latest release ------------------------------------------------
gh_api() {
  curl -fsS -H "Accept: application/vnd.github+json" \
    ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} "https://api.github.com/repos/$GH_REPO/$1"
}
rel_json="$(gh_api releases/latest)" || { log "github unreachable"; exit 0; }
rel_tag="$(printf '%s' "$rel_json" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)"
asset_url="$(printf '%s' "$rel_json" | sed -n 's/.*"browser_download_url": *"\([^"]*linux-x64[^"]*\.zip\)".*/\1/p' | head -1)"
[ -n "$rel_tag" ] && [ -n "$asset_url" ] || { log "no usable release yet"; exit 0; }

# ---- signal B: game build id -------------------------------------------------
local_build() { sed -n 's/.*"buildid"[^"]*"\([0-9]*\)".*/\1/p' "$CS2_DIR/steamapps/appmanifest_$APP_ID.acf" 2>/dev/null | head -1; }
remote_build() {
  "$STEAMCMD" +login anonymous +app_info_update 1 +app_info_print "$APP_ID" +quit 2>/dev/null |
  awk '/"branches"/{b=1} b&&/"public"/{p=1} p&&/"buildid"/{gsub(/[^0-9]/,"",$2); print $2; exit}'
}
remote="$(remote_build)"; before="$(local_build)"

reason=""
[ "$rel_tag" != "$(read_state last_tag)" ] && reason="release"
if [ -n "$remote" ] && [ "$remote" != "$before" ]; then reason="gameupdate"; fi
[ "$FORCE" = 1 ] && reason="manual"
[ -z "$reason" ] && exit 0
log "triggered by: $reason (release $rel_tag, build ${before:-?} -> ${remote:-?})"

# ---- update game (only if the build moved) ----------------------------------
if [ "$reason" = "gameupdate" ] || [ -z "$before" ]; then
  "$STEAMCMD" +force_install_dir "$CS2_DIR" +login anonymous +app_update "$APP_ID" validate +quit \
    || { log "steamcmd update failed"; exit 1; }
fi
build="$(local_build)"                    # what is actually installed

# ---- deploy the plugin -------------------------------------------------------
run_id="$(date -u +%Y%m%d-%H%M%S)_${rel_tag}_b${build}"
run_dir="$STATE_DIR/runs/$run_id"; mkdir -p "$run_dir"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} "$asset_url" -o "$tmp/pkg.zip" \
  || { log "download failed"; exit 1; }
unzip -qo "$tmp/pkg.zip" -d "$CS2_DIR/game/csgo" || { log "unzip failed"; exit 1; }
[ -n "${POST_UPDATE_HOOK:-}" ] && "$POST_UPDATE_HOOK"

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
