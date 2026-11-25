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

CS2SDKTest is a CS2 plugin that verifies the usability and compatibility of [s2sdk](https://github.com/alliedmodders/s2sdk) in a real game server environment.

Tests are organized into suites, one per interface, and cover two kinds of checks:

| Category | What it does | Result |
|---|---|---|
| **Call** | Calls the interface methods and validates return values | `OK` / `FAIL` / `CRASHED` / `SKIP` / `N/A` |
| **Layout** | Reads real struct memory and verifies field offsets and sizes | `OK` / `FAIL` |

> [!WARNING]
> **Results are not guaranteed to be fully accurate.** Some interface behavior has no reference to compare against, or depends on runtime state, so results may contain false positives or false negatives. A `FAIL` / `CRASHED` does not necessarily mean s2sdk is defective; it may come from a bug in the test case itself, an unmet precondition, or differences in the test environment. Likewise, `OK` does not prove an interface is fully correct. When a result looks abnormal, confirm the root cause **manually** against the disassembly or the engine binary before drawing conclusions.
>
> **Most test cases were written by AI.** They have not been reviewed one by one, so the calls, arguments, assertions, and expected values may be wrong. Do not treat them as an authoritative description of interface behavior.

## Quick Start

You need a CS2 dedicated server, on Linux or Windows. Below, `<server>` is the directory that contains `game/`, and `<csgo>` is `<server>/game/csgo`.

1. Download `cs2sdktest-linux-x64-<tag>.zip` from [Releases](https://github.com/zqunagi/CS2SDKTest/releases), or [build](#build) it yourself.
2. Unzip it into `<csgo>`. The package is laid out relative to that directory:

```
bin/linuxsteamrt64/libcs2sdktest.so
bin/linuxsteamrt64/cst_run.sh
configs/gamedata.toml
configs/cs2sdktest-meta.json
```

```bash
unzip -o cs2sdktest-linux-x64-<tag>.zip -d <server>/game/csgo
```

3. Run the script that comes with it. It starts the server with the library preloaded, the tests run once the map is up, and the server quits by itself when they are done:

```bash
bash <server>/game/csgo/bin/linuxsteamrt64/cst_run.sh
```

**Windows**: download `cs2sdktest-windows-x64-<tag>.zip`, unzip it into `<csgo>` the same way, and run `cst_run.exe`. Windows has no `LD_PRELOAD`, so it uses [Microsoft Detours](https://github.com/microsoft/Detours) instead: it creates `cs2.exe` suspended, adds `cs2sdktest.dll` to the new process's import table, and lets it run. The library is therefore in the process before the engine has loaded anything, as with `LD_PRELOAD`. `cs2.exe` itself is not modified.

```bat
<server>\game\csgo\bin\win64\cst_run.exe
```

The server's console output stays on your terminal, and anything you type goes to the engine's console as usual. See [`cst_run.sh` options](#cst_runsh-options) for what you can change.

4. Read the results in `<csgo>/logs/`.

## `cst_run.sh` / `cst_run.exe` options

The two take the same options (on Windows `--server-dir` is worked out from where `cst_run.exe` is).

All optional; without them the defaults apply.

| Option | Description | Default |
|---|---|---|
| `--delay <seconds>` | Wait this long after the map starts simulating before the tests start | `10` |
| `--suite <name>` | Run only this suite | all |
| `--keep-running` | Keep the server running after the tests finish | quits |
| `--map <name>` | Map to start on | `de_dust2` |
| `--port <n>` | Host port | `10001` |
| `--server-dir <dir>` | The server directory (the one that contains `game/`) | worked out from the script's location |
| `--build-id <id>` | The game's Steam build id, copied into `results.json` | none |
| `--result-dir <dir>` | Write the result files here instead of `<csgo>/logs` | `<csgo>/logs` |
| `-- <args>` | Everything after `--` is appended to the `cs2` command line | none |
| `-h`, `--help` | Show the options | |

```bash
bash cst_run.sh --delay 30 --suite netserver --map de_mirage -- +sv_lan 1
```

The script refuses to start if a `cs2` from the same server is already running. Its exit status is `cs2`'s.

## Environment variables

`cst_run.sh` and `cst_run.exe` set these from their options. On Linux you can also set them yourself and start `cs2` by hand with `LD_PRELOAD=<csgo>/bin/linuxsteamrt64/libcs2sdktest.so` (the path must be absolute: the library works out `<csgo>` from where it was loaded). On Windows, start the server with `cst_run.exe`, which does the injection.

| Variable | Description |
|---|---|
| `CST_RUN=1` | Required. Without it the library is inert, so it is safe to leave preloaded. |
| `CST_RUN_SUITE=<name>` | Run only the specified suite |
| `CST_RUN_DELAY=<seconds>` | Wait this long after the map starts simulating before the tests (0–600, default 10) |
| `CST_RUN_QUIT=0` | Keep the server running after the tests finish (it quits by default) |
| `CST_GAME_DIR=<csgo>` | The game directory, if it cannot be worked out from where the library was loaded |
| `CST_RESULT_DIR=<dir>` | Write `results.json`, `results.jsonl`, `crash.json` and `schema-dump.json` here instead of `<csgo>/logs` |
| `CST_BUILD_ID=<id>` | The game's Steam build id, copied into `results.json` (the library cannot see it from inside the process) |

## Output

Everything is written to `<csgo>/logs/` (or `CST_RESULT_DIR`):

| File | Content |
|---|---|
| `run-<time>.log` | The full run log, one line per call, flushed line by line |
| `results.json` / `results.jsonl` | Every call with its outcome; the `.jsonl` is written as the run goes, so a run that dies still leaves what it got through |
| `schema-dump.json` | The engine's schema as dumped by `SchemaDiffSuite`. Local only and unredacted: do not publish it |
| `crash.json` | Written when the process dies on SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGABRT: the signal, the fault address, how far the run had got, the call in flight and a backtrace. The signal then continues to the engine's own handler, so exit codes and dumps are unchanged |

Faults inside a test call are caught and reported as `CRASHED`; `crash.json` covers everything outside one, such as a crash while the map loads. `crash.json` is only produced on Linux for now: on Windows a crash outside a test call does not write it.

`configs/cs2sdktest-meta.json` (version, s2sdk commit, platform) lets whoever runs the package identify the build even if the library never got to write anything.

After a new Release or a game update, the automated test results are published to the [results page](https://zqunagi.github.io/CS2SDKTest/).

## Build

**Requirements**: CMake ≥ 3.21; clang or gcc on Linux x64, Visual Studio 2022 on Windows x64.

```bash
git clone --recursive https://github.com/zqunagi/CS2SDKTest.git
cd CS2SDKTest
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

> If Ninja is not installed, use the `linux-x64-make-release` preset.

On Windows:

```bat
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

Configuring downloads Microsoft Detours (`v4.0.1`) and builds it with the project; for an offline build, point `-DCS2SDKTEST_DETOURS_DIR=<Detours source directory>` at a local copy. The result is in `build/<preset>/package/`, already laid out as above:

```
package/
├── bin/linuxsteamrt64/
│   ├── libcs2sdktest.so
│   └── cst_run.sh
└── configs/
    ├── gamedata.toml
    └── cs2sdktest-meta.json
```

The Windows package has `bin/win64/cs2sdktest.dll` and `bin/win64/cst_run.exe` in place of the Linux files, and the rest is the same.

On Linux the library exports no symbols, and the build fails if it does. That is deliberate: `LD_PRELOAD` puts it ahead of every other library in the process, so an exported name such as `CreateInterface` would replace the engine's own. A Windows DLL's exports are resolved per module, so that cannot happen there; it exports a single empty function at ordinal 1, because Detours requires ordinal 1 to exist when it adds the DLL to an import table.

## License

MIT.
