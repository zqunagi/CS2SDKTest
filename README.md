<div align="center">

# CS2SDKTest

[![Release](https://img.shields.io/github/v/release/zqunagi/CS2SDKTest?sort=semver)](https://github.com/zqunagi/CS2SDKTest/releases)
![Platform](https://img.shields.io/badge/platform-Linux-informational)
![License](https://img.shields.io/badge/license-MIT-green)

[Live results](https://zqunagi.github.io/CS2SDKTest/) · [Download Release](https://github.com/zqunagi/CS2SDKTest/releases)

**English** · [简体中文](README.zh-CN.md)

</div>

---

## Overview

CS2SDKTest is a shared library that is loaded into a Counter-Strike 2 dedicated server with `LD_PRELOAD`, and verifies the usability and compatibility of [s2sdk](https://github.com/alliedmodders/s2sdk) in a real game server environment. It needs no Metamod, no plugin framework, and no edit to `gameinfo.gi`, so a game update that breaks those cannot stop the tests from starting.

Tests are organized into suites, one per interface, and cover two kinds of checks:

| Category | What it does | Result |
|---|---|---|
| **Call** | Calls the interface methods and validates return values | `OK` / `FAIL` / `CRASHED` / `SKIP` / `N/A` |
| **Layout** | Reads real struct memory and verifies field offsets and sizes | `OK` / `FAIL` |

Nothing is hooked or left installed in the game: a run is a single sweep that writes its results and then quits the server.

> [!WARNING]
> **Results are not guaranteed to be fully accurate.** Some interface behavior has no reference to compare against, or depends on runtime state, so results may contain false positives or false negatives. A `FAIL` / `CRASHED` does not necessarily mean s2sdk is defective; it may come from a bug in the test case itself, an unmet precondition, or differences in the test environment. Likewise, `OK` does not prove an interface is fully correct. When a result looks abnormal, confirm the root cause **manually** against the disassembly or the engine binary before drawing conclusions.
>
> **Most test cases were written by AI.** They have not been reviewed one by one, so the calls, arguments, assertions, and expected values may be wrong. Do not treat them as an authoritative description of interface behavior.

## How it runs

1. The library is preloaded into the `cs2` process. Its constructor does nothing but start a worker thread, and only when `CST_AUTORUN=1` is set and the process is `cs2`.
2. The worker waits for `libserver.so` and finds `ISource2Server::GameFrame` in the interface's vtable by what the function does (it references the string `"GameFrame"`), not by a slot number from the SDK headers, so a shifted slot cannot make it patch the wrong function. It replaces that one vtable slot with its own.
3. In the first frame where the map is simulating, on the engine's main thread, it sets up the cvar system, waits `CST_AUTORUN_DELAY` seconds, runs every suite once, writes the results, and sends `quit`.

If the hook cannot be placed (the string is not found, or matches more than one slot), `libserver.so` never appears, or no map starts simulating, it says why on the console and exits instead of idling until a timeout.

## Quick Start

You need a CS2 dedicated server on Linux. Below, `<server>` is the directory that contains `game/`, and `<csgo>` is `<server>/game/csgo`.

1. Download `cs2sdktest-linux-x64-<tag>.zip` from [Releases](https://github.com/zqunagi/CS2SDKTest/releases), or [build](#build) it yourself.
2. Unzip it into `<csgo>`. The package is laid out relative to that directory:

```
bin/linuxsteamrt64/cs2sdktest.so
configs/gamedata.toml
configs/cs2sdktest-meta.json
```

```bash
unzip -o cs2sdktest-linux-x64-<tag>.zip -d <server>/game/csgo
```

3. Start the server with the library preloaded. **The path must be absolute**: the library works out `<csgo>` from where it was loaded, and a relative path would defeat that.

```bash
cd <server>/game/bin/linuxsteamrt64
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
LD_PRELOAD=<server>/game/csgo/bin/linuxsteamrt64/cs2sdktest.so CST_AUTORUN=1 \
  ./cs2 -dedicated +hostport 10001 +game_type 0 +game_mode 1 +map de_dust2 \
  +sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 \
  +mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure
```

4. When the server has quit on its own, read the results in `<csgo>/logs/`.

Without `CST_AUTORUN=1` the library does nothing, so it is safe to leave in a launch script.

## Environment variables

| Variable | Description |
|---|---|
| `CST_AUTORUN=1` | Required. Without it the library is inert. |
| `CST_AUTORUN_SUITE=<name>` | Run only the specified suite |
| `CST_AUTORUN_DELAY=<seconds>` | Wait this long after the map starts simulating before the sweep (0–600, default 10) |
| `CST_AUTORUN_QUIT=0` | Keep the server running after the tests finish (it quits by default) |
| `CST_GAME_DIR=<csgo>` | The game directory, if it cannot be worked out from where the library was loaded |
| `CST_RESULT_DIR=<dir>` | Write `results.json`, `results.jsonl`, `crash.json` and `schema-dump.json` here instead of `<csgo>/logs` |
| `CST_BUILD_ID=<id>` | The game's Steam build id, copied into `results.json` (the library cannot see it from inside the process) |

## Output

Everything is written to `<csgo>/logs/` (or `CST_RESULT_DIR`):

| File | Content |
|---|---|
| `autorun-<time>.log` | The full run log, one line per call, flushed line by line |
| `results.json` / `results.jsonl` | Every call with its outcome; the `.jsonl` is written as the run goes, so a run that dies still leaves what it got through |
| `schema-dump.json` | The engine's schema as dumped by `SchemaDiffSuite`. Local only and unredacted: do not publish it |
| `crash.json` | Written when the process dies on SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGABRT: the signal, the fault address, how far the run had got, the call in flight and a backtrace. The signal then continues to the engine's own handler, so exit codes and dumps are unchanged |

Faults inside a test call are caught and reported as `CRASHED`; `crash.json` covers everything outside one, such as a crash while the map loads.

`configs/cs2sdktest-meta.json` (version, s2sdk commit, platform) lets whoever runs the package identify the build even if the library never got to write anything.

After a new Release or a game update, the automated test results are published to the [results page](https://zqunagi.github.io/CS2SDKTest/).

## Build

**Requirements**: Linux x64, CMake ≥ 3.21, clang or gcc.

```bash
git clone --recursive https://github.com/zqunagi/CS2SDKTest.git
cd CS2SDKTest
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

> If Ninja is not installed, use the `linux-x64-make-release` preset.

The build is Linux only. The result is in `build/<preset>/package/`, already laid out as above:

```
package/
├── bin/linuxsteamrt64/cs2sdktest.so
└── configs/
    ├── gamedata.toml
    └── cs2sdktest-meta.json
```

The library exports no symbols, and the build fails if it does. That is deliberate: `LD_PRELOAD` puts it ahead of every other library in the process, so an exported name such as `CreateInterface` would replace the engine's own.

## License

MIT.
