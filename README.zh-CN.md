<div align="center">

# CS2SDKTest

[![Release](https://img.shields.io/github/v/release/zqunagi/CS2SDKTest?sort=semver)](https://github.com/zqunagi/CS2SDKTest/releases)
![Platform](https://img.shields.io/badge/platform-Linux-informational)
![License](https://img.shields.io/badge/license-MIT-green)

[在线结果展示](https://zqunagi.github.io/CS2SDKTest/) · [下载 Release](https://github.com/zqunagi/CS2SDKTest/releases)

[English](README.md) · **简体中文**

</div>

---

## 简介

CS2SDKTest 是一个 cs2 插件，用于验证 [s2sdk](https://github.com/alliedmodders/s2sdk) 在实际游戏服务器环境中的可用性与兼容性。

测试按接口划分为多个套件，覆盖以下两类检查：

| 类别 | 内容 | 判定 |
|---|---|---|
| **调用** | 实际调用接口方法并校验返回值 | `OK` / `FAIL` / `CRASHED` / `SKIP` / `N/A` |
| **布局** | 读取结构体真实内存，核对字段偏移与大小 | `OK` / `FAIL` |

> [!WARNING]
> **测试结果不保证完全准确。** 由于部分接口行为没有可对照的基准或者依赖运行时状态，结果可能存在误判（误报或漏报）：`FAIL` / `CRASHED` 不一定意味着 s2sdk 存在缺陷，也可能源于测试用例本身的错误、调用前置条件不满足或测试环境差异；`OK` 同样不能证明接口完全正确。出现异常结果时，应结合反汇编或引擎二进制**人工**确认根因，再作结论。
>
> **绝大部分测试用例由 AI 生成。** 用例未经过逐项人工审查，其中的调用方式、参数、断言条件和预期值可能有误，不应将其视为接口行为的权威依据。

## 快速开始

需要一台 Linux 上的 CS2 专用服务器。下文 `<server>` 指包含 `game/` 的目录，`<csgo>` 指 `<server>/game/csgo`。

1. 从 [Releases](https://github.com/zqunagi/CS2SDKTest/releases) 下载 `cs2sdktest-linux-x64-<tag>.zip`，或参照[构建](#构建)自行编译。
2. 解压到 `<csgo>`。包内的目录结构是相对于该目录的：

```
bin/linuxsteamrt64/libcs2sdktest.so
bin/linuxsteamrt64/cst_run.sh
configs/gamedata.toml
configs/cs2sdktest-meta.json
```

```bash
unzip -o cs2sdktest-linux-x64-<tag>.zip -d <server>/game/csgo
```

3. 运行包里自带的脚本。它会预加载动态库并启动服务器，地图就绪后开始测试，测完服务器自行退出：

```bash
bash <server>/game/csgo/bin/linuxsteamrt64/cst_run.sh
```

服务器的控制台输出会直接显示在你的终端上，你输入的内容也会照常交给引擎的控制台。可调整的内容见 [`cst_run.sh` 选项](#cst_runsh-选项)。

4. 在 `<csgo>/logs/` 查看结果。

## `cst_run.sh` 选项

均为可选；不指定时使用默认值。

| 选项 | 说明 | 默认值 |
|---|---|---|
| `--delay <秒>` | 地图开始模拟后，等待多久再开始测试 | `10` |
| `--suite <name>` | 仅执行指定套件 | 全部 |
| `--keep-running` | 测试完毕后不退出服务器 | 退出 |
| `--map <name>` | 启动使用的地图 | `de_dust2` |
| `--port <n>` | 服务器端口 | `10001` |
| `--server-dir <目录>` | 服务器目录（包含 `game/` 的那个） | 根据脚本位置推算 |
| `--build-id <id>` | 游戏的 Steam 构建号，写入 `results.json` | 无 |
| `--result-dir <目录>` | 把结果文件写到这里，而不是 `<csgo>/logs` | `<csgo>/logs` |
| `-- <参数>` | `--` 之后的内容原样追加到 `cs2` 命令行 | 无 |
| `-h`、`--help` | 显示选项 | |

```bash
bash cst_run.sh --delay 30 --suite netserver --map de_mirage -- +sv_lan 1
```

如果同一个服务器的 `cs2` 已经在运行，脚本会拒绝启动。脚本的退出码就是 `cs2` 的退出码。

## 环境变量

`cst_run.sh` 会根据选项设置这些变量。你也可以自己设置，然后手动以 `LD_PRELOAD=<csgo>/bin/linuxsteamrt64/libcs2sdktest.so` 启动 `cs2`（路径必须是绝对路径：动态库会根据自己被加载的位置推算出 `<csgo>`）。

| 环境变量 | 说明 |
|---|---|
| `CST_RUN=1` | 必须设置。不设置时动态库不起作用，所以可以放心一直预加载。 |
| `CST_RUN_SUITE=<name>` | 仅执行指定套件 |
| `CST_RUN_DELAY=<秒>` | 地图开始模拟后，等待多久再开始测试（0–600，默认 10） |
| `CST_RUN_QUIT=0` | 测试完毕后不退出服务器（默认退出） |
| `CST_GAME_DIR=<csgo>` | 游戏目录；无法根据动态库被加载的位置推算时使用 |
| `CST_RESULT_DIR=<目录>` | 把 `results.json`、`results.jsonl`、`crash.json`、`schema-dump.json` 写到这里，而不是 `<csgo>/logs` |
| `CST_BUILD_ID=<id>` | 游戏的 Steam 构建号，会写进 `results.json`（动态库在进程内看不到它） |

## 输出

所有文件写入 `<csgo>/logs/`（或 `CST_RESULT_DIR`）：

| 文件 | 内容 |
|---|---|
| `run-<时间>.log` | 完整运行日志，每次调用一行，逐行刷新 |
| `results.json` / `results.jsonl` | 每次调用及其判定；`.jsonl` 边运行边写，所以即使进程中途死亡也会留下已完成的部分 |
| `schema-dump.json` | `SchemaDiffSuite` 导出的引擎 schema。仅限本地，且未脱敏：不要公开 |
| `crash.json` | 进程因 SIGSEGV、SIGBUS、SIGILL、SIGFPE 或 SIGABRT 死亡时写出：信号、出错地址、运行到了哪一步、正在执行的调用以及回溯。随后信号继续交给引擎自己的处理器，所以退出码和转储不变 |

测试调用内部的错误会被捕获并报告为 `CRASHED`；`crash.json` 覆盖其余所有情况，例如地图加载期间的崩溃。

`configs/cs2sdktest-meta.json`（版本、s2sdk 提交、平台）让运行这个包的人即使在动态库没来得及写任何东西时，也能确认这是哪一个构建。

新 Release 发布或游戏版本更新后，自动测试结果会发布到[展示页](https://zqunagi.github.io/CS2SDKTest/)。

## 构建

**环境要求**：Linux x64，CMake ≥ 3.21，clang 或 gcc。

```bash
git clone --recursive https://github.com/zqunagi/CS2SDKTest.git
cd CS2SDKTest
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

> 未安装 Ninja 时，使用 `linux-x64-make-release` 预设。

只支持 Linux。产物位于 `build/<preset>/package/`，目录结构已经按上面的样子组织好：

```
package/
├── bin/linuxsteamrt64/
│   ├── libcs2sdktest.so
│   └── cst_run.sh
└── configs/
    ├── gamedata.toml
    └── cs2sdktest-meta.json
```

动态库不导出任何符号，导出了构建就会失败。这是有意为之：`LD_PRELOAD` 会让它排在进程里所有库的前面，如果它导出了 `CreateInterface` 之类的名字，就会顶替引擎自己的同名函数。

## 许可

MIT。
