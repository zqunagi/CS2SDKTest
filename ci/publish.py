#!/usr/bin/env python3
"""Sanitise one finished run and publish it to the gh-pages branch.

Called by ci-poll.sh as PUBLISH_HOOK:  publish.py <run_dir>

Only run.json and results.json are published, both rewritten on the way out.
Console logs, jsonl, plugin logs and dumps never leave the server.
"""
import base64
import getpass
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
from datetime import datetime

CONFIG = os.environ.get("CST_CI_CONFIG", "/etc/cs2sdktest-ci.env")
KEEP_RUNS = 500  # entries kept in index.json


def load_config(path):
    cfg = {}
    rx = re.compile(r"""\s*([A-Z_][A-Z0-9_]*)=(?:"([^"]*)"|'([^']*)'|(\S*))""")
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = rx.match(line)
            if m:
                cfg[m.group(1)] = next(g for g in m.groups()[1:] if g is not None)
    return cfg


def make_sanitizer(cfg):
    literals = []
    for key, tag in (("CS2_DIR", "<cs2>"), ("STATE_DIR", "<state>")):
        if cfg.get(key):
            literals.append((cfg[key].rstrip("/"), tag))
    for value, tag in ((socket.gethostname(), "<host>"), (getpass.getuser(), "<user>")):
        if len(value) >= 3:
            literals.append((value, tag))
    literals.sort(key=lambda p: -len(p[0]))

    patterns = [
        (re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}(?::\d+)?\b"), "<ip>"),
        (re.compile(r"\b[0-9a-fA-F]{1,4}(?::[0-9a-fA-F]{0,4}){5,7}\b"), "<ip>"),
        (re.compile(r"STEAM_\d:\d:\d+"), "<steamid>"),
        (re.compile(r"\[U:\d:\d+\]"), "<steamid>"),
        (re.compile(r"\b7656119\d{10}\b"), "<steamid>"),
        (re.compile(r"[\w.+-]+@[\w-]+(?:\.[\w-]+)+"), "<email>"),
        (re.compile(r"/home/[^/\s\"']+"), "/home/<user>"),
        (re.compile(r"/root\b"), "/<root>"),
    ]

    def clean(text):
        for lit, tag in literals:
            text = text.replace(lit, tag)
        for rx, tag in patterns:
            text = rx.sub(tag, text)
        return text

    def walk(node):
        if isinstance(node, str):
            return clean(node)
        if isinstance(node, list):
            return [walk(n) for n in node]
        if isinstance(node, dict):
            return {k: walk(v) for k, v in node.items()}
        return node

    return walk


def hour(ts):
    """Round an ISO-8601 UTC timestamp down to the hour."""
    try:
        dt = datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ")
        return dt.strftime("%Y-%m-%dT%H:00:00Z")
    except (ValueError, TypeError):
        return ""


def git(args, cwd, env):
    subprocess.run(["git"] + args, cwd=cwd, env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: publish.py <run_dir>")
    run_dir = sys.argv[1]
    cfg = load_config(CONFIG)
    repo = cfg.get("PAGES_REPO") or cfg.get("GH_REPO")
    token = cfg.get("PAGES_TOKEN")
    if not repo or not token:
        sys.exit("PAGES_REPO/GH_REPO and PAGES_TOKEN must be set")
    # Commits on gh-pages are made as the repository owner, not a made-up bot.
    author_name = cfg.get("GIT_AUTHOR_NAME")
    author_email = cfg.get("GIT_AUTHOR_EMAIL")
    if not author_name or not author_email:
        sys.exit("GIT_AUTHOR_NAME and GIT_AUTHOR_EMAIL must be set")
    sanitize = make_sanitizer(cfg)

    run = read_json(os.path.join(run_dir, "run.json"))
    results = None
    rpath = os.path.join(run_dir, "results.json")
    if os.path.exists(rpath):
        try:
            results = read_json(rpath)
        except ValueError:
            results = None  # unreadable means unpublished, not "publish it raw"

    finished = hour(run.get("finished"))
    run_pub = sanitize(run)
    run_pub["finished"] = finished
    run_pub.pop("exit_code", None)
    if results is not None:
        results = sanitize(results)
        results["started"] = hour(results.get("started"))
        results.pop("duration_ms", None)
    summary = (results or {}).get("summary", {})

    # Public id: hour precision, so the directory name does not leak timing.
    base_id = "%s_%s_b%s" % (finished[:13].replace("-", "").replace("T", "-"),
                             run.get("release", "?"), run.get("game_buildid", "?"))
    base_id = re.sub(r"[^A-Za-z0-9._-]", "_", base_id)

    # The token goes in through git's env-based config, never argv or the URL.
    auth = base64.b64encode(("x-access-token:" + token).encode()).decode()
    env = dict(os.environ,
               GIT_TERMINAL_PROMPT="0",
               GIT_CONFIG_COUNT="1",
               GIT_CONFIG_KEY_0="http.extraheader",
               GIT_CONFIG_VALUE_0="Authorization: Basic " + auth)
    url = "https://github.com/%s.git" % repo

    work = tempfile.mkdtemp(prefix="cst-pages-")
    try:
        for _ in range(3):
            shutil.rmtree(work, ignore_errors=True)
            git(["clone", "-q", "--depth", "1", "--branch", "gh-pages", url, work], None, env)
            git(["config", "user.name", author_name], work, env)
            git(["config", "user.email", author_email], work, env)

            index_path = os.path.join(work, "data", "index.json")
            index = read_json(index_path)
            taken = {r["id"] for r in index["runs"]}
            run_id, n = base_id, 1
            while run_id in taken:
                n += 1
                run_id = "%s-%d" % (base_id, n)

            out = os.path.join(work, "data", "runs", run_id)
            os.makedirs(out)
            write_json(os.path.join(out, "run.json"), run_pub)
            if results is not None:
                write_json(os.path.join(out, "results.json"), results)

            index["runs"].insert(0, {
                "id": run_id, "status": run.get("status"), "reason": run.get("reason"),
                "release": run.get("release"), "game_buildid": run.get("game_buildid"),
                "finished": finished,
                "failed": summary.get("failed"), "crashed": summary.get("crashed"),
            })
            for old in index["runs"][KEEP_RUNS:]:
                shutil.rmtree(os.path.join(work, "data", "runs", old["id"]), ignore_errors=True)
            index["runs"] = index["runs"][:KEEP_RUNS]
            write_json(index_path, index)

            git(["add", "-A"], work, env)
            git(["commit", "-q", "-m", "result " + run_id], work, env)
            try:
                git(["push", "-q", "origin", "gh-pages"], work, env)
                print("published", run_id)
                return
            except subprocess.CalledProcessError:
                continue  # someone pushed first; re-clone and redo
        sys.exit("push failed after 3 attempts")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
