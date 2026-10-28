<div align="center">

# CS2SDKTest

**面向 s2sdk 的全接口测试插件。**

[![Build](https://github.com/zqunagi/CS2SDKTest/actions/workflows/build.yml/badge.svg)](https://github.com/zqunagi/CS2SDKTest/actions)
[![Release](https://img.shields.io/github/v/release/zqunagi/CS2SDKTest?sort=semver)](https://github.com/zqunagi/CS2SDKTest/releases)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational)
![License](https://img.shields.io/badge/license-MIT-green)

[在线结果展示](https://zqunagi.github.io/CS2SDKTest/) · [下载 Release](https://github.com/zqunagi/CS2SDKTest/releases)

[English](README.md) · **简体中文**

</div>

---

## 简介

CS2SDKTest 是一个 [Metamod:Source](https://www.sourcemm.net/) 插件，用于对 [s2sdk](https://github.com/alliedmodders/s2sdk) 进行整体测试。插件在真实的 CS2 服务器进程中运行，对 s2sdk 暴露的接口逐项执行调用、hook 和结构体布局检查，并将结果记录到日志。

测试按接口划分为多个套件，覆盖以下三类检查：

| 类别 | 内容 | 判定 |
|---|---|---|
| **调用** | 实际调用接口方法并校验返回值 | `OK` / `FAIL` / `CRASHED` / `SKIP` |
| **Hook** | 安装 hook，统计回调触发次数与参数样本 | 触发计数；未触发记为 `never` |
| **布局** | 读取结构体真实内存，核对字段偏移与大小 | `OK` / `FAIL` |

仅检查接口版本字符串无法发现虚表槽位变化或参数变更，因此本项目采用实际调用的方式验证。该方法同样适用于游戏更新后的回归检查：结果由 `OK` 变为 `CRASHED` 或 `FAIL`，或 hook 由有计数变为 `never`，即表明对应接口需要适配。

## 快速开始

1. 从 [Releases](https://github.com/zqunagi/CS2SDKTest/releases) 下载对应平台的压缩包，或参照[构建](#构建)自行编译。
2. 将包内的 `addons/` 复制到服务器的 `game/csgo/` 目录。
3. 启动服务器，在控制台执行：

```
] cst run                # 调用测试
] cst hooks install      # 安装 hook
] cst hooks dump         # 查看触发计数与参数样本
```

日志写入 `<csgo>/addons/cs2sdktest/logs/`。

## 命令

| 命令 | 说明 |
|---|---|
| `cst help` | 显示用法 |
| `cst suites` | 列出本次构建包含的测试套件 |
| `cst targets` | 列出游戏动态库清单及加载状态 |
| `cst run [suite]` | 执行调用测试并写入日志 |

## 自动运行

设置环境变量后，插件会在服务器启动后自动执行全部测试，用于无人值守和持续集成：

| 环境变量 | 说明 |
|---|---|
| `CST_AUTORUN=1` | 启用自动运行 |
| `CST_AUTORUN_SUITE=<name>` | 仅执行指定套件 |
| `CST_AUTORUN_DELAY=<秒>` | 延迟执行时间（0–600） |
| `CST_AUTORUN_QUIT=0` | 执行完毕后不退出服务器（默认退出） |

测试所用的服务器启动参数：

```
-dedicated +hostport 10001 +game_type 0 +game_mode 1 +map de_dust2 +sv_hibernate_when_empty 0 +sv_cheats 0 +bot_quota 1 +mp_warmup_pausetimer 1 +mp_freezetime 0 +mp_autoteambalance 0 +mp_limitteams 0 -insecure
```

新 Release 发布或游戏版本更新后，自动测试结果会发布到[展示页](https://zqunagi.github.io/CS2SDKTest/)。

## 构建

**环境要求**：CMake ≥ 3.21；Windows 使用 Visual Studio 2022（x64），Linux 使用 clang 或 gcc（x64）。

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

> 未安装 Ninja 时，Linux 可使用 `linux-x64-make-release` 预设。

### 安装与部署

构建产物位于 `build/<preset>/package/`，已按 Metamod 的目录结构组织：

```
package/
└── addons/
    ├── metamod/cs2sdktest.vdf
    └── cs2sdktest/bin/cs2sdktest.{dll,so}
```

可将 `package/` 下的内容复制到服务器的 `game/csgo/`，也可在 configure 时指定 `CS2SDKTEST_GAME_DIR`，通过 `deploy` 目标直接部署：

```bash
cmake --preset linux-x64-release -D CS2SDKTEST_GAME_DIR=/srv/cs2/game/csgo
cmake --build --preset linux-x64-release --target deploy
```

## 警告

> [!WARNING]
> **测试结果不保证完全准确。** 由于部分接口行为没有可对照的基准或者依赖运行时状态，结果可能存在误判（误报或漏报）：`FAIL` / `CRASHED` 不一定意味着 s2sdk 存在缺陷，也可能源于测试用例本身的错误、调用前置条件不满足或测试环境差异；`OK` 同样不能证明接口完全正确。出现异常结果时，应结合反汇编或引擎二进制人工确认根因，再作结论。
>
> **绝大部分测试用例由 AI 生成。** 用例未经过逐项人工审查，其中的调用方式、参数、断言条件和预期值可能有误，不应将其视为接口行为的权威依据。

## 许可

MIT。
