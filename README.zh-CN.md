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

CS2SDKTest 是一个通过 `LD_PRELOAD` 注入 Counter-Strike 2 专用服务器的动态库，用于验证 [s2sdk](https://github.com/alliedmodders/s2sdk) 在实际游戏服务器环境中的可用性与兼容性。它不依赖 Metamod，也不依赖任何插件框架，更不需要修改 `gameinfo.gi`，所以游戏更新导致这些东西失效时，测试本身依然能够启动。

测试按接口划分为多个套件，覆盖以下两类检查：

| 类别 | 内容 | 判定 |
|---|---|---|
| **调用** | 实际调用接口方法并校验返回值 | `OK` / `FAIL` / `CRASHED` / `SKIP` / `N/A` |
| **布局** | 读取结构体真实内存，核对字段偏移与大小 | `OK` / `FAIL` |

不会在游戏里留下任何钩子或常驻内容：一次运行就是一轮完整的测试，写出结果后退出服务器。

> [!WARNING]
> **测试结果不保证完全准确。** 由于部分接口行为没有可对照的基准或者依赖运行时状态，结果可能存在误判（误报或漏报）：`FAIL` / `CRASHED` 不一定意味着 s2sdk 存在缺陷，也可能源于测试用例本身的错误、调用前置条件不满足或测试环境差异；`OK` 同样不能证明接口完全正确。出现异常结果时，应结合反汇编或引擎二进制**人工**确认根因，再作结论。
>
> **绝大部分测试用例由 AI 生成。** 用例未经过逐项人工审查，其中的调用方式、参数、断言条件和预期值可能有误，不应将其视为接口行为的权威依据。

## 运行原理

1. 动态库被预加载进 `cs2` 进程。构造函数只启动一个工作线程，并且只有设置了 `CST_AUTORUN=1`、进程确实是 `cs2` 时才会这样做。
2. 工作线程等待 `libserver.so`，然后在 `ISource2Server` 的虚表里按函数的行为找到 `GameFrame`（该函数会引用字符串 `"GameFrame"`），而不是使用 SDK 头文件里的槽位序号，所以槽位发生偏移时也不会误改别的函数。它把这一个虚表槽位换成自己的函数。
3. 在地图开始模拟后的第一帧（引擎主线程）里初始化 cvar 系统，等待 `CST_AUTORUN_DELAY` 秒，把所有套件各运行一遍，写出结果，然后发送 `quit`。

如果钩子放不上（找不到字符串，或匹配到不止一个槽位）、`libserver.so` 始终没有出现、或者一直没有地图进入模拟，它会在控制台说明原因并退出，而不是一直挂到超时。

## 快速开始

需要一台 Linux 上的 CS2 专用服务器。下文 `<server>` 指包含 `game/` 的目录，`<csgo>` 指 `<server>/game/csgo`。

1. 从 [Releases](https://github.com/zqunagi/CS2SDKTest/releases) 下载 `cs2sdktest-linux-x64-<tag>.zip`，或参照[构建](#构建)自行编译。
2. 解压到 `<csgo>`。包内的目录结构是相对于该目录的：

```
bin/linuxsteamrt64/cs2sdktest.so
configs/gamedata.toml
configs/cs2sdktest-meta.json
```

```bash
unzip -o cs2sdktest-linux-x64-<tag>.zip -d <server>/game/csgo
```

3. 预加载动态库并启动服务器。**路径必须是绝对路径**：动态库会根据自己被加载的位置推算出 `<csgo>`，相对路径会让推算失败。

```bash
cd <server>/game/bin/linuxsteamrt64
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
LD_PRELOAD=<server>/game/csgo/bin/linuxsteamrt64/cs2sdktest.so CST_AUTORUN=1 \
  ./cs2 -dedicated +hostport 10001 +game_type 0 +game_mode 1 +map de_dust2 \
  +sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 \
  +mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure
```

4. 服务器自行退出后，在 `<csgo>/logs/` 查看结果。

没有设置 `CST_AUTORUN=1` 时动态库什么也不做，所以可以放心留在启动脚本里。

## 环境变量

| 环境变量 | 说明 |
|---|---|
| `CST_AUTORUN=1` | 必须设置。不设置时动态库不起作用。 |
| `CST_AUTORUN_SUITE=<name>` | 仅执行指定套件 |
| `CST_AUTORUN_DELAY=<秒>` | 地图开始模拟后，等待多久再开始测试（0–600，默认 10） |
| `CST_AUTORUN_QUIT=0` | 测试完毕后不退出服务器（默认退出） |
| `CST_GAME_DIR=<csgo>` | 游戏目录；无法根据动态库被加载的位置推算时使用 |
| `CST_RESULT_DIR=<目录>` | 把 `results.json`、`results.jsonl`、`crash.json`、`schema-dump.json` 写到这里，而不是 `<csgo>/logs` |
| `CST_BUILD_ID=<id>` | 游戏的 Steam 构建号，会写进 `results.json`（动态库在进程内看不到它） |

## 输出

所有文件写入 `<csgo>/logs/`（或 `CST_RESULT_DIR`）：

| 文件 | 内容 |
|---|---|
| `autorun-<时间>.log` | 完整运行日志，每次调用一行，逐行刷新 |
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
├── bin/linuxsteamrt64/cs2sdktest.so
└── configs/
    ├── gamedata.toml
    └── cs2sdktest-meta.json
```

动态库不导出任何符号，导出了构建就会失败。这是有意为之：`LD_PRELOAD` 会让它排在进程里所有库的前面，如果它导出了 `CreateInterface` 之类的名字，就会顶替引擎自己的同名函数。

## 许可

MIT。
