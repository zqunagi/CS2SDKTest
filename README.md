# CS2SDKTest

一个 Metamod:Source 插件，行为上就是个**普通插件** —— 但它的工作是把 s2sdk 暴露的接口
尽可能都真的用一遍：调用它们的方法、hook 它们的回调，然后把发生了什么记进日志。

CS2 更新之后跑一遍，日志里哪条从 `OK` 变成 `CRASHED`、哪个 hook 从有计数变成 `never`，
就是被 Valve 改掉的地方。

> **当前覆盖：两个套件。**
>
> | 套件 | 覆盖 |
> |---|---|
> | `engine` | `IVEngineServer2` / `ISource2Engine` / `IAppSystem` —— 约 40 个只读调用，约 35 个 SKIP，外加 `CGlobalVars` 的 27 个字段布局检查 |
> | `server` | `ISource2Server`（= `IServerGameDLL`）—— **11 个 hook**、约 20 个只读调用、4 个枚举列表、约 30 个 SKIP |

> 两份文档：
> [`docs/STATUS.md`](docs/STATUS.md) 是项目状态和架构决策（为什么这么做，接手先看这个），
> [`docs/TEST_TARGETS.md`](docs/TEST_TARGETS.md) 是 s2sdk 待测清单（按重要度排序）。

---

## 为什么是"调用 + hook"而不是查版本号

查接口版本字符串（`Source2Server001` 还在不在）只能抓到最表层的变化。CS2 更新最常见的
破坏方式是**版本号根本没变、但虚表槽位挪了或参数改了** —— 版本号探测完全看不见，
而调用一次就会当场崩掉或返回垃圾。

Schema 那条路也走不通：SchemaSystem 里几千个类和 s2sdk 头文件里那些手写的 C++ 定义
基本对不上，拿它去校验 s2sdk 的结构体布局意义不大。

所以只有真的调、真的 hook。

---

## 目录结构

```
CS2SDKTest/
├── CMakeLists.txt / CMakePresets.json
├── cmake/
│   ├── HL2SDK.cmake        SDK 的 include / 宏 / 预编译库
│   ├── Metamod.cmake       metamod 头文件 + META_IS_SOURCE2
│   ├── Protobuf.cmake      用 SDK 自带 protoc 生成 eiface.h 要的 pb 头
│   ├── PluginVersion.cmake git 版本号
│   └── templates/version_gen.h.in
├── configs/cs2sdktest.vdf.in
├── src/
│   ├── core/       Platform · Log(控制台+文件) · Module · FileSystemUtil
│   ├── data/       GameModules —— 游戏动态库清单，用来定位接口
│   ├── testkit/    CallGuard · TestContext · ITestSuite · TestRunner
│   ├── hook/       HookRecorder(触发计数和参数样本) · HookArg(不可信参数的安全打印)
│   ├── suites/     测试套件 —— 一个接口一个
│   └── plugin/     Plugin(ISmmPlugin) · Commands
├── s2sdk/          submodule, cs2 分支（原 hl2sdk）
└── metamod-source/ submodule, master
```

依赖是单向的：`plugin → testkit → data → core`，`hook` 只被测试套件和命令层用。
`core` 不认识 SDK 之外的任何东西。

### 两个容易混的词

| 词 | 指什么 |
|---|---|
| **module（模块）** | 游戏进程里加载的**动态库** —— `engine2.so`、`server.so`、`schemasystem.so`。和 Source 引擎自己的用法一致，`core/Module.h` 和 `data/GameModules.h` 说的都是这个 |
| **suite（套件）** | **我们写的测试代码**，一个接口一个，放在 `src/suites/` |

套件是模块清单的使用者：`Source2EngineSuite` 里那句
`ctx.QueryInterface("engine2", ...)`，`"engine2"` 查的就是 `GameModules` 表 ——
SDK 的版本常量只说要哪个接口，从不说去哪个库要，所以只能挨个库问它的 `CreateInterface`。

---

## 怎么加覆盖

一个**测试套件（suite）** = 一个接口（`IVEngineServer2`、`ISchemaSystem`、`IFileSystem`……）。
以接口为单位是因为「拿到指针」这一步是共享的：拿不到就整块跳过，而不是刷一屏一模一样的失败。

新建 `src/suites/EngineServerSuite.cpp`：

```cpp
class EngineServerSuite : public ITestSuite
{
    const char* Id() const override { return "engine"; }
    const char* DisplayName() const override { return "IVEngineServer2"; }

    bool Acquire(TestContext& ctx) override
    {
        engine_ = static_cast<IVEngineServer2*>(
            ctx.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
        if (engine_ == nullptr)
        {
            ctx.UnavailableCall("IVEngineServer2", "engine2 does not answer %s",
                                INTERFACEVERSION_VENGINESERVER);
            return false;
        }
        return true;
    }

    void ExerciseCalls(TestContext& ctx) override
    {
        CS2ST_CALL(ctx, "IVEngineServer2::GetServerGlobals", {
            void* globals = engine_->GetServerGlobals();
            ctx.Detail("-> %p", globals);
            if (globals == nullptr) ctx.Fail("engine promises a singleton here");
        });
    }

    IVEngineServer2* engine_ = nullptr;
};
```

然后两步收尾：

1. `TestRunner::CreateDefault()` 里加一行 `runner.Add(...)`
2. `CMakeLists.txt` 的 `CS2SDKTEST_SOURCES` 里加上这个 `.cpp`

`CS2ST_CALL` 负责三件事：把调用体走一遍崩溃保护、打一行对齐的日志、把计数加对。
所以填覆盖的时候只需要写调用本身。

### hook 那一半

```cpp
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);

int InstallHooks(TestContext& ctx) override
{
    HookRecorder::Get().Declare("IServerGameDLL::GameFrame", Id());
    SH_ADD_HOOK(IServerGameDLL, GameFrame, server_, SH_MEMBER(this, &Self::OnGameFrame), true);
    installed_ = true;
    return 1;
}

void OnGameFrame(bool simulating, bool first, bool last)
{
    CS2ST_HOOK_FIRED_ARGS("IServerGameDLL::GameFrame",
                          "simulating=%d first=%d last=%d", simulating, first, last);
    RETURN_META(MRES_IGNORED);
}
```

**`Declare()` 一定要在装 hook 之前调**：这样即使这个 hook 一次都没触发，它也会出现在
dump 里显示 `never` —— "更新后不再触发了"正是最有价值的那一行，静默消失就白搭了。

hook 不逐次记日志。`GameFrame` 一分钟就是几万行，而且有意思的从来不是第一千次调用。
每个 hook 只记：进入/离开计数 + 头 3 次的参数快照 + 头 3 次进出的 trace 行。

### hook 回调里崩了会怎样

**不会被 `CallGuard` 抓住。** guard 只在 `meta_test run` 那一轮扫描期间装着 handler，
hook 是引擎在自己的帧里调进来的，那时候 handler 早还给引擎了（就算还在，
`t_armed` 也是 0，会转发出去）。所以 hook 回调里崩 = 服务器当场挂。

补救是 trace 行：日志逐行 flush，最后一行 `enter X` 没有对应的 `leave X`，就知道崩在哪。

所以 hook 回调里**不要解引用参数**。唯一必须解引用的是 `const char*`，
`src/hook/HookArg.h` 里的 `SafeStringArg()` 会先验地址再交给 `%s`，
并用 `%.63s` 限长。这不是洁癖：参数签名变了正是这个插件要抓的东西，
而签名一变，`const char*` 槽位里的东西就不是指针了，`nullptr` 检查挡不住。

### 结构体成员怎么测

接口测的是**函数**。但接口还会交出**结构体** —— `GetServerGlobals()` 给你一个
`CGlobalVars*`，那是一块裸内存加一份 SDK 自己猜出来的布局：里面五个字段直接叫
`m_unk001`~`m_unk005`，是逆向的人知道两个已知字段之间隔了 20 字节、却不知道那是什么，
拿 int 填上去的。

Valve 在那里插一个字段，后面所有偏移就全错了 —— **不崩，不报错，只是数字全是错的**。
比虚表错位更阴。

`StructInspector` 干这个。原理是：**很多 C++ 类型的合法值域远小于它占的字节**。

| 类型 | 占用 | 能表示 | 实际合法 |
|---|---|---|---|
| `bool` | 1 字节 | 256 种 | **2 种**（0 / 1） |
| 指针 | 8 字节 | 2⁶⁴ 种 | null，或规范地址且已映射 |
| `float` | 4 字节 | 2³² 种 | 有限值（NaN/Inf 占了很大一块编码空间） |
| 枚举 | 4 字节 | 2³² 种 | 通常三四种 |

所以从偏移 0x54 读出 `0x4A`，而 SDK 说那里是 `bool` —— `bool` 不可能是 0x4A，
**那个偏移就不是那个字段**。判据只用到"字段是什么类型"，不涉及"值在业务上应该是多少"，
所以一套实现对所有结构体通用。

用法是写一份字段清单，判定逻辑全共享：

```cpp
StructInspector inspect(ctx, "CGlobalVars", globals_, sizeof(CGlobalVars));

inspect.Float  ("realtime",         offsetof(CGlobalVarsBase, realtime));
inspect.Int32  ("maxClients",       offsetof(CGlobalVarsBase, maxClients), 0, INT32_MAX);
inspect.Pointer("m_pfnWarningFunc", offsetof(CGlobalVarsBase, m_pfnWarningFunc));
inspect.Bool   ("m_bInSimulation",  offsetof(CGlobalVarsBase, m_bInSimulation));
inspect.String ("mapname",          offsetof(CGlobalVars,     mapname));
inspect.Summarize();
inspect.HexDump();
```

几个要点：

- **偏移必须用 `offsetof`，不能手写数字。** 要比的是「SDK 假设的布局」对「游戏的实际内存」，
  所以假设必须由编译器从 SDK 头文件算出来。手抄成 `0x54` 的话，submodule 一升级就悄悄失效了。
- **`bool` 必须读原始字节，不能读成 `bool`。** 读成 `bool` 编译器会把非零规约成 `true`，
  0x4A 变成"true"，证据当场没了。
- **`int` 没有通用判据**（任何位模式都是合法 int），所以不带范围的重载只打印不判定。
  给范围时只给**类型**层面的约束（计数不能为负），不给语义层面的（`maxClients ≤ 64` 那种）——
  一写语义就不通用了。
- **第一个失败的偏移才是重点。** 错位是累积的，后面一长串红色都是它的余震，
  所以 `Summarize()` 会单独指出它。

**它是证伪工具，不是证明工具。** 全绿只代表"没发现矛盾"。它看不见：同类型字段互换、
int 字段错位、结构体末尾新增的字段。

#### 未初始化字段

不少 SDK 结构体的构造函数是空的 —— `CGlobalVarsBase` 的字面就是 `{}`。引擎从不写的字段
里躺的是分配器留下的残留，越界值跟布局没关系。实测 `m_bEnableAssertions` 就是 `0xFF`。

这种字段标 `kMayBeUninitialized`，结果记成 `SUSPECT`（黄色）而不是 `FAIL`。检查本身**没有放宽**：
值照样打印、照样计数，所以哪天它开始装别的东西，那一行还是会变。

标之前要有依据：**只有当前后字段都正确时才能标**。`m_bEnableAssertions` 在 `0x41`，
而 `tickcount@0x44` 和 `mapname@0x60` 都读得干干净净 —— 布局要是从 `0x41` 就滑了，
不可能到 `0x44` 又自己接上。错位不会自我修复，这就是判据。

---

## 命令

命令注册时**没有**加 `FCVAR_CLIENT_CAN_EXECUTE` —— 它会调引擎接口、装 hook、写文件，
属于服主操作，不该让连进来的玩家能触发。

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

`FAIL` 和 `CRASHED` 两行会用 ANSI 红色标出（`CRASHED` 是亮红），其余不着色 ——
每行都染色的话就没有哪行突出了，而要找的正是两百行里那三行。

颜色默认控制台和日志文件都开。文件里带转义码的好处是 `cat` / `less -R` 看到的高亮
和控制台一样，坏处是编辑器里会看到乱码 —— `log color console` 只给控制台着色，
`log color off` 全关。Windows 上加载时会先探测控制台是否支持 VT 转义，不支持就自动关掉，
不会打出一屏转义序列。

典型用法：

```
] meta_test run                     # 调用扫一遍，看有没有崩的
] meta_test hooks install           # 装上 hook
   ... 打一局 ...
] meta_test hooks dump              # 看哪些触发了、参数长什么样
```

`run` 和 `hooks dump` 会各自开一个带时间戳的日志文件；如果已经有 `log open` 开着的
会话日志，就直接写进那个，不打断它。

日志在 `<csgo>/addons/cs2sdktest/logs/`，路径是从插件自身 dll/so 的位置推出来的。

### 输出长这样

```
--- IVEngineServer2 (engine) ------------------------------------------
  IVEngineServer2::GetServerGlobals                    OK       -> 0x7f2a4c001200
  IVEngineServer2::GetGameGlobals                      CRASHED
  IVEngineServer2::GetPlayerUserId                     FAIL     returned -1 for a connected slot
  IVEngineServer2::IsDedicatedServer                   OK       -> true

  4 call(s): 2 ok, 1 crashed, 1 failed, 0 unavailable, 0 skipped
```

```
  HOOK                                              ENTER     LEAVE  FIRST ARGUMENTS
  ISource2Server::GameFrame                          1247      1247  simulating=1 first=1 last=0
  ISource2Server::ServerConVarChanged                   2         1  sv_cheats = "1"
  ISource2Server::GameServerSteamAPIActivated           1         1
  ISource2Server::OnHostNameChanged                 never         -
```

每个 hook 都装两次：pre 记 `ENTER`，post 记 `LEAVE`。

- **两个数不等 = 进去了没出来** —— 原函数崩了，或者被别的插件 `MRES_SUPERCEDE` 挡掉了
  （post 链就不会走）。上面 `ServerConVarChanged` 那行就是。
- 只装 post 的话这两种情况和"引擎不再调用了"在 dump 里长得一模一样，分不出来。

前 3 次进入和离开还会往日志文件里写一行 `[trace] enter/leave`。日志逐行 flush，
所以**崩在某个 hook 里时，文件最后一行是 `enter X` 而没有 `leave X`** —— 直接指出崩在哪。
`hooks install` 会自动开一个日志文件，就是为了这个。

---

## 崩溃保护

所有调用都走 `src/testkit/CallGuard.h`，故障会被抓住变成一行 `CRASHED` 日志，
而不是把服务器带走。

- **Windows**：`__try/__except`，始终生效。只接管硬件故障；C++ 异常
  （`0xE06D7363`）和断点会继续往外传，不然会把别人打算 catch 的错误吞掉。
  栈溢出捕获后会调 `_resetstkoflw()` 重置保护页，否则第二次溢出直接终止进程。
- **Linux**：`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` 的 handler +
  `sigsetjmp`/`siglongjmp`，只在一轮扫描期间装（`FaultGuardScope`），
  结束后把引擎自己的 handler 还回去。带 `sigaltstack`，所以栈耗尽时 handler 还有栈可用。

日志里会写成这样：

```
  IVEngineServer2::GetGameGlobals                      CRASHED  SIGSEGV at 0x8
  IServerGameDLL::GameFrame                            CRASHED  ACCESS_VIOLATION writing 0x0 at 0x7f2a4c0011f0
```

### 它做不到的事

- **什么都不会被展开。** 抓到故障时，从出错点到 guard 之间的析构函数全部跳过 ——
  那次调用分配的内存泄漏，持有的锁不会释放。诊断扫描可以接受，正常代码不行。
- **进程只是"大概"还健康。** 一个调用崩在半路，可能已经把引擎状态搞乱了。
  一次扫描里出现 CRASHED，之后的结果就都要打问号 —— 重启服务器再跑一遍。
- **不出故障的内存破坏看不见。** 一个调用踩了别处的合法内存，这里照样记 `OK`，
  然后十分钟后服务器崩掉。

有两个实现细节值得知道，因为它们约束了代码怎么写：

- MSVC 不允许在用了 SEH 的函数里出现需要展开的对象（C2712），所以 `InvokeGuarded`
  用 `std::function` 做类型擦除而不是模板，实际调用放在一个 `noinline` 的辅助函数里，
  `__try` 那个函数体里只有 POD。
- Linux 侧 `siglongjmp` 之后局部变量的值不保证还在（编译器可能把它们放在了寄存器里，
  而 `siglongjmp` 会把寄存器恢复成 `sigsetjmp` 时的样子），所以要跨过跳转的东西
  存在 `thread_local` 里而不是局部变量里。`sigsetjmp(buf, 1)` 的第二个参数也不能省 ——
  它保证信号掩码被恢复，否则 `SIGSEGV` 会一直处于 block 状态，下一次故障直接杀进程。

---

## 构建

### 依赖

- CMake ≥ 3.21
- Windows：Visual Studio 2022（x64）
- Linux：clang 或 gcc，x64。SDK 的预编译库是老 ABI，所以工程强制
  `_GLIBCXX_USE_CXX11_ABI=0` 并静态链接 libstdc++

```bash
git submodule update --init --recursive
```

### Windows

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

### Linux

```bash
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

没装 Ninja（比如一个干净的 WSL）就用 Makefiles 那一组，或者
`sudo apt install ninja-build` 之后再用上面的：

```bash
cmake --preset linux-x64-make-release
cmake --build --preset linux-x64-make-release
```

带 `-gcc-` 的预设走 g++，其余走 clang++。

### 产物

直接按 Metamod 的目录结构摆好在 `build/<preset>/package/`：

```
package/
├── addons/metamod/cs2sdktest.vdf
└── addons/cs2sdktest/bin/cs2sdktest.{dll,so}
```

把 `package/` 的内容整个拷进 `game/csgo/` 即可。也可以在 configure 时指定目标目录，
用 `deploy` 目标一步到位：

```bash
cmake --preset linux-x64-release -D CS2SDKTEST_GAME_DIR=/srv/cs2/game/csgo
cmake --build --preset linux-x64-release --target deploy
```

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `CS2SDKTEST_HL2SDK_DIR` | `./s2sdk` | SDK 路径 |
| `CS2SDKTEST_METAMOD_DIR` | `./metamod-source` | Metamod 路径 |
| `CS2SDKTEST_GAME_DIR` | 空 | 设置后启用 `deploy` 目标 |
| `CS2SDKTEST_USE_MEMOVERRIDE` | `OFF` | 编译 `public/tier0/memoverride.cpp`，把 new/delete 交给 tier0。**MSVC 上目前链不过**（见下面的 SDK 陷阱）。本插件不需要它：只读引擎、只给自己的容器分配内存，没有内存跨模块边界 |
| `CS2SDKTEST_WITH_PROTOBUF` | `ON` | 用 SDK 自带 protoc 生成并链接 protobuf。**别关** —— `public/eiface.h` 无条件 include `network_connection.pb.h`，而 `ISmmPlugin.h` 会拉到 `eiface.h`，所以每个源文件都需要它 |
| `CS2SDKTEST_PROTO_FILES` | `network_connection;netmessages` | 要过 protoc 的 `s2sdk/common/*.proto`（不带扩展名）。import 闭包会自动补齐（如 netmessages 拉进 networkbasetypes/valveextensions/source2_steam_stats） |
| `CS2SDKTEST_TIER1_KEYVALUES3` | `OFF` | 额外编译 `tier1/keyvalues3.cpp` |
| `CS2SDKTEST_TIER1_BITBUF` | `ON` | 额外编译 `tier1/bitbuf.cpp`。`NetworkMessagesSuite` 的序列化往返需要 bf_write/bf_read —— 它们的构造函数没有任何预编译库导出（tier0 里 bitbuf 符号只有 `netadr_t::Serialize/Unserialize`）。关掉后该套件序列化方法全记 SKIP |
| `CS2SDKTEST_WARNINGS_AS_ERRORS` | `OFF` | 只作用于本工程自己的源文件 |

改了 `option()` 的默认值之后旧的 `CMakeCache.txt` 不会跟着变 —— `option()` 不覆盖已缓存的值。
遇到"改了默认值却没生效"，删掉 `build/<preset>/` 重新 configure。

---

## 搭建过程中确认的 SDK 陷阱

- **`META_IS_SOURCE2` 必须定义**。否则 `ISmmPlugin.h` 会去 include `tier1/interface.h`，
  cs2 分支根本没有这个文件。
- **protobuf 不是可选的**。`public/eiface.h` 无条件 include `network_connection.pb.h`。
- **`tier1` 没有预编译库**。`public/tier1/*.h` 基本是模板，真正的 TU 在 `s2sdk/tier1/`，
  要用 `CON_COMMAND_F` / `ConVar` 就必须自己把 `convar.cpp` 编进来。
- **`META_UNREGCMD` 已经和 SDK 对不上**。宏按值传 `ConCommand`，而
  `ISmmAPI::UnregisterConCommand` 要的是 `ConCommandRef*`。见 `Commands.cpp` 里的写法。
- **Windows 必须用 `/MT`**。Valve 的预编译库全是 release 静态 CRT 构建的，
  用 `/MD`（CMake 默认）会得到 `RuntimeLibrary` 不匹配加一堆 `libcpmt`/`msvcprt`
  重复符号。Debug 构建也得用 release CRT —— Valve 没提供 debug 版的库可以匹配。
- **`memoverride.cpp` 在 MSVC 上链不过**。AlliedModders 把 `_recalloc_base` 的定义用
  `#if 0` 关掉了（注释说"会在库卸载时崩溃"），但 `_recalloc_crt` 还在调用它，
  于是 `__imp__recalloc_base` 无法解析。它同时还重定义 `malloc`/`free`，和 CRT 冲突。
- **Linux 上不能加 `-Wl,--exclude-libs,ALL`**。它会把静态库符号全变成 local，
  包括 `interfaces.a` 里的 `CreateInterface` —— 而那正是 Metamod 要找的入口。
  症状不是"找不到符号"：`dlsym` 会沿依赖链找到 `libtier0.so` 导出的同名函数，
  于是插件以 `Failed to get API` 加载失败。
- **`CBaseAppSystem` 模板缺两个虚函数**。`IAppSystem` 在 CS2 上是 11 槽虚表
  （新增了槽 5 `PreShutdown()` 和槽 10 `GetBuildType()`），`CBaseAppSystem` 没补上。
- **`IGameResourceService` 只有接口版本常量，没有类定义。**

最后三条正好是这个插件该去测的东西。

---

## 许可

MIT。`s2sdk` 与 `metamod-source` 各自遵循其上游许可。
