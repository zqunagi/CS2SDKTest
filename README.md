# CS2SDKTest

一个 Metamod:Source 插件：把 s2sdk 暴露的接口逐个真实调用、hook 回调、检查结构体布局，
结果写入日志。用于cs2更新后快速定位s2sdk中需要适配的接口。

## 命令

```
meta_test help                    用法
meta_test suites                  本次构建带的测试套件
meta_test targets                 游戏动态库清单及加载状态
meta_test run [suite]             跑一遍调用，写日志
meta_test hooks install [suite]   装 hook 并保持运行
meta_test hooks remove [suite]    卸掉
meta_test hooks dump              到目前为止触发了什么
meta_test hooks reset             计数清零，hook 留着
meta_test log open [name]         把之后所有输出也写进这个文件
meta_test log close
meta_test log status
meta_test log color <on|off|console>
```

```
] meta_test run                # 调用扫描
] meta_test hooks install      # 安装 hook
] meta_test hooks dump         # 查看触发计数与参数样本
```

日志写在 `<csgo>/addons/cs2sdktest/logs/`。

## 构建

需要 CMake ≥ 3.21；Windows 用 Visual Studio 2022（x64），Linux 用 clang 或 gcc（x64）。

```bash
# Windows
cmake --preset windows-x64
cmake --build --preset windows-x64-release

# Linux
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

没有 Ninja 的环境用 `linux-x64-make-release` 预设。

## 安装

构建产物在 `build/<preset>/package/`，已按 Metamod 的目录结构组织：

```
package/
├── addons/metamod/cs2sdktest.vdf
└── addons/cs2sdktest/bin/cs2sdktest.{dll,so}
```

把 `package/` 下的内容拷贝到服务器的 `game/csgo/`。也可以在 configure 时指定
`CS2SDKTEST_GAME_DIR`，用 `deploy` 目标直接部署：

```bash
cmake --preset linux-x64-release -D CS2SDKTEST_GAME_DIR=/srv/cs2/game/csgo
cmake --build --preset linux-x64-release --target deploy
```

## 许可

MIT。`s2sdk` 与 `metamod-source` 各自遵循其上游许可。
