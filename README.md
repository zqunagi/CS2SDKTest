<div align="center">

# CS2SDKTest

**A full-interface test plugin for s2sdk.**

[![Build](https://github.com/zqunagi/CS2SDKTest/actions/workflows/build.yml/badge.svg)](https://github.com/zqunagi/CS2SDKTest/actions)
[![Release](https://img.shields.io/github/v/release/zqunagi/CS2SDKTest?sort=semver)](https://github.com/zqunagi/CS2SDKTest/releases)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational)
![License](https://img.shields.io/badge/license-MIT-green)

[Live results](https://zqunagi.github.io/CS2SDKTest/) · [Download Release](https://github.com/zqunagi/CS2SDKTest/releases)

**English** · [简体中文](README.zh-CN.md)

</div>

---

## Overview

CS2SDKTest is a [Metamod:Source](https://www.sourcemm.net/) plugin for testing [s2sdk](https://github.com/alliedmodders/s2sdk) as a whole. It runs inside a real CS2 server process and, for each interface exposed by s2sdk, performs calls, hooks, and struct layout checks, then records the results to a log.

Tests are organized into suites, one per interface, and cover three kinds of checks:

| Category | What it does | Result |
|---|---|---|
| **Call** | Calls the interface methods and validates return values | `OK` / `FAIL` / `CRASHED` / `SKIP` |
| **Hook** | Installs hooks; counts callback invocations and samples arguments | Invocation count; `never` if not triggered |
| **Layout** | Reads real struct memory and verifies field offsets and sizes | `OK` / `FAIL` |

Checking interface version strings alone cannot detect changed vtable slots or changed parameters, so this project verifies interfaces by actually calling them. The same approach works as a regression check after a game update: if a result changes from `OK` to `CRASHED` or `FAIL`, or a hook goes from having a count to `never`, the corresponding interface needs to be adapted.

## Quick Start

1. Download the archive for your platform from [Releases](https://github.com/zqunagi/CS2SDKTest/releases), or [build](#build) it yourself.
2. Copy the `addons/` directory from the package into the server's `game/csgo/` directory.
3. Start the server and run in the console:

```
] cst run                # call tests
] cst hooks install      # install hooks
] cst hooks dump         # show trigger counts and argument samples
```

Logs are written to `<csgo>/addons/cs2sdktest/logs/`.

## Commands

| Command | Description |
|---|---|
| `cst help` | Show usage |
| `cst suites` | List the test suites included in this build |
| `cst targets` | List game libraries and their load status |
| `cst run [suite]` | Run the call tests and write the log |

## Autorun

When the environment variables below are set, the plugin runs all tests automatically after the server starts, for unattended runs and CI:

| Variable | Description |
|---|---|
| `CST_AUTORUN=1` | Enable autorun |
| `CST_AUTORUN_SUITE=<name>` | Run only the specified suite |
| `CST_AUTORUN_DELAY=<seconds>` | Delay before running (0–600) |
| `CST_AUTORUN_QUIT=0` | Keep the server running after the tests finish (it exits by default) |

Server launch parameters used for testing:

```
-dedicated +hostport 10001 +game_type 0 +game_mode 1 +map de_dust2 +sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 +mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure
```

After a new Release or a game update, the automated test results are published to the [results page](https://zqunagi.github.io/CS2SDKTest/).

## Build

**Requirements**: CMake ≥ 3.21; Visual Studio 2022 (x64) on Windows, clang or gcc (x64) on Linux.

```bash
git clone --recursive https://github.com/zqunagi/CS2SDKTest.git
cd CS2SDKTest
```

**Windows**

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

**Linux**

```bash
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

> If Ninja is not installed, use the `linux-x64-make-release` preset on Linux.

### Install and Deploy

Build output is in `build/<preset>/package/`, already laid out in Metamod's directory structure:

```
package/
└── addons/
    ├── metamod/cs2sdktest.vdf
    └── cs2sdktest/bin/cs2sdktest.{dll,so}
```

Copy the contents of `package/` into the server's `game/csgo/`, or set `CS2SDKTEST_GAME_DIR` at configure time and deploy directly with the `deploy` target:

```bash
cmake --preset linux-x64-release -D CS2SDKTEST_GAME_DIR=/srv/cs2/game/csgo
cmake --build --preset linux-x64-release --target deploy
```

## Warnings

> [!WARNING]
> **Results are not guaranteed to be fully accurate.** Some interface behavior has no reference to compare against, or depends on runtime state, so results may contain false positives or false negatives. A `FAIL` / `CRASHED` does not necessarily mean s2sdk is defective; it may come from a bug in the test case itself, an unmet precondition, or differences in the test environment. Likewise, `OK` does not prove an interface is fully correct. When a result looks abnormal, confirm the root cause manually against the disassembly or the engine binary before drawing conclusions.
>
> **Most test cases were written by AI.** They have not been reviewed one by one, so the calls, arguments, assertions, and expected values may be wrong. Do not treat them as an authoritative description of interface behavior.

## License

MIT.
