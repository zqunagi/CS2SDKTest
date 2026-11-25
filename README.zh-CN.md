<div align="center">

# CS2SDKTest

[![Release](https://img.shields.io/github/v/release/zqunagi/CS2SDKTest?sort=semver)](https://github.com/zqunagi/CS2SDKTest/releases)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-informational)
![License](https://img.shields.io/badge/license-MIT-green)

[在线结果展示](https://zqunagi.github.io/CS2SDKTest/) · [下载 Release](https://github.com/zqunagi/CS2SDKTest/releases)

[English](README.md) · **简体中文**

</div>

---

## 简介

CS2SDKTest 是一个用来测试 [s2sdk](https://github.com/alliedmodders/s2sdk) 的 CS2 插件。

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

1. 从 [Releases](https://github.com/zqunagi/CS2SDKTest/releases) 下载 `cs2sdktest-linux-x64-<tag>.zip`，或参照[构建](#构建)自行编译。
2. 解压到 `/game/csgo`。
3. 通过脚本或可执行文件注入并运行测试。

```bash
chmod +x <server>/game/csgo/cs2sdktest/bin/cst_run.sh
bash <server>/game/csgo/cs2sdktest/bin/cst_run.sh
```

```bat
<server>\game\csgo\cs2sdktest\bin\cst_run.exe
```


## 构建

**环境要求**：CMake ≥ 3.21；Linux x64 用 clang 或 gcc，Windows x64 用 Visual Studio 2022。

Linux :
```bash
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

Windows ：

```bat
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

## 许可

MIT。
