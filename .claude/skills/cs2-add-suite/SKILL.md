---
name: cs2-add-suite
description: 为 CS2SDKTest 新增或审查 SDK 接口测试套件。适用于新增 suite、补测接口、检查方法覆盖、结构体布局、调用或 hook。
---

# 添加 CS2 测试套件

目标是生成可在 CS2 更新后对比的日志。保持流程简单：只读取当前接口、最接近的现有 suite，以及实际需要的参考资料；不要默认通读项目文档或全部参考文件。

## 必须保持的规则

1. **接口方法不留空白。** 每个虚方法都要调用，或用一行 `SkipCall` 写明具体原因。需要确认时运行：

   ```sh
   sh .claude/skills/cs2-add-suite/scripts/audit-coverage.sh <sdk头文件> <精确类名> <suite.cpp>
   ```

   只审计本次涉及的类；不要用子串匹配类名。

2. **结构体只登记一次。** 接口能可靠触达的具体类/结构体才做 `StructInspector`。不确定是否已有覆盖时再运行 `list-struct-coverage.sh`；已有覆盖就留注释，不要重复输出。

3. **断言按类型。** `Fail` 只表示 SDK 声明无法成立的事实；运行时预期放到 `Detail`，有已知不确定性时用 `Suspect`。

4. **hook 只观察。** 先 `Declare()`，再安装 pre/post；处理器返回 `MRES_IGNORED`。hook 没有故障保护，访问指针或字符串前先做安全检查。

## 最小工作流

1. 找到 SDK 声明和一个相近的 suite；只读当前实现所需的片段。`docs/STATUS.md`、`docs/TEST_TARGETS.md`、参考文件都按需查看，不是每次必读。
2. 写 header。除非 hook 参数需要，否则不要把 SDK 头文件带入 header。
3. 实现 `Acquire()`：只做 `QueryInterface`、空指针和模块状态判断；需要引擎调用的前置数据放到 `ExerciseCalls`，并用 `CS2ST_CALL` 包住。
4. 在 `ExerciseCalls` 中覆盖 IAppSystem 基类槽位和目标方法；每个方法保持一个清晰的调用/跳过记录，打印返回值。参数无法安全构造或会改变在线状态时才跳过。
5. 按需添加结构体布局、双路径 oracle 和 hook。数组遍历要限制数量，偏移使用 `offsetof`，不要手写数字。
6. 注册到 `TestRunner::CreateDefault()`、`CMakeLists.txt` 的相关列表和 `src/suites/README.md`。
7. 交付前按需运行覆盖审计和结构体登记脚本；是否构建、实测和分析 FAIL，按任务环境决定，不把它们设为无条件步骤。

## 常用判断

- 析构函数可能占两个 vtable 槽；看到 ABI/槽位风险时，在注释中说明并用安全的槽位调用验证。
- `Acquire()` 和 hook 中不要做未经保护的引擎访问。
- `bool` 用原始字节检查；指针/字符串按工具约定读取；大数组和不可信计数必须限界。
- 最有价值的 oracle 是两条独立路径得到同一事实：getter 对字段、按名称对按 ID、返回指针对 `offsetof` 地址。
- 某个 oracle 全部失败时先检查基对象和计算方式，不要直接改 SDK。

## 按需参考

- [references/pitfalls.md](references/pitfalls.md)：编译、崩溃、错误结论和覆盖遗漏的高频坑；遇到对应问题再读。
- [references/patterns.md](references/patterns.md)：调用、布局、列表和 hook 的最小代码形状；需要示例时再读。
- `scripts/audit-coverage.sh`：检查精确类名的虚方法是否已调用或跳过。
- `scripts/list-struct-coverage.sh`：列出已有结构体布局的归属。
