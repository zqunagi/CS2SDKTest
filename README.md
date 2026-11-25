<div align="center">

# CS2SDKTest

[![Release](https://img.shields.io/github/v/release/zqunagi/CS2SDKTest?sort=semver)](https://github.com/zqunagi/CS2SDKTest/releases)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-informational)
![License](https://img.shields.io/badge/license-MIT-green)

[Live results](https://zqunagi.github.io/CS2SDKTest/) · [Download Release](https://github.com/zqunagi/CS2SDKTest/releases)

**English** · [简体中文](README.zh-CN.md)

</div>

---

## Overview

CS2SDKTest is a CS2 plugin for testing [s2sdk](https://github.com/alliedmodders/s2sdk).

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

1. Download `cs2sdktest-linux-x64-<tag>.zip` from [Releases](https://github.com/zqunagi/CS2SDKTest/releases), or [build](#build) it yourself.
2. Unzip it into `<server>/game/csgo`.
3. Inject and run the tests with the script or the executable.

```bash
chmod +x <server>/game/csgo/cs2sdktest/bin/cst_run.sh
bash <server>/game/csgo/cs2sdktest/bin/cst_run.sh
```

```bat
<server>\game\csgo\cs2sdktest\bin\cst_run.exe
```


## Build

**Requirements**: CMake ≥ 3.21; clang or gcc on Linux x64, Visual Studio 2022 on Windows x64.

Linux:
```bash
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

Windows:

```bat
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

## License

MIT.
