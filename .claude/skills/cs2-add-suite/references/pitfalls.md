# 常见陷阱（按需阅读）

只在遇到对应问题时查阅本文件；它不是新增 suite 的必读清单。

## 编译

- 不要在 `CS2ST_CALL(...)` 的宏参数内部写 `#define`/`#undef`。需要复用逻辑时使用命名模板或把宏定义放在函数作用域外。
- `CS2ST_CALL(context, target, body)` 是函数式宏，**只有圆括号和方括号能保护逗号，花括号和尖括号不能**。body 里任何顶层逗号都会被当成第四个宏参数，报 `too many arguments provided to function-like macro invocation`：

  ```cpp
  CS2ST_CALL(context, "...", {
      const Row table[] = { {"a", 1}, {"b", 2} };   // 花括号里的逗号 → 编译失败
      std::map<Key, Value> m;                       // 尖括号里的逗号 → 编译失败
      int a = 1, b = 2;                             // 一条声明多个声明符 → 编译失败
  });
  ```

  把表和多参模板提到宏外的匿名命名空间，宏内只留循环。需要运行期值的表用一个返回值的辅助函数填，调用点只剩一行。`context.Detail("%d", a, b)` 这类没问题——逗号在圆括号内。

  **写完就跑 `scripts/check-macro-commas.sh <file.cpp>`**，不要等编译器一轮一轮报——它一次列全所有出错行。
- `<sys/sysmacros.h>` 可能把 `major`、`minor` 定义成宏；本地变量改用 `api_major`、`api_minor`。
- 自己的变量使用 `std::uint32_t` 等标准类型；SDK 引用型出参必须保留 SDK 的精确 typedef。位运算结果必要时显式转换，避免缩窄。

## 崩溃

- `Acquire()` 也会在安装 hook 时运行，但该路径没有故障保护；这里只做查询和空指针判断，其他引擎调用放进 `ExerciseCalls` 的 `CS2ST_CALL`。
- hook 回调始终没有故障保护：先用 `LooksLikeObjectPointer`、`SafeStringArg` 等工具验证指针/字符串，字符串限制长度，不要遍历未经验证的结构体。
- 读取数组前检查指针和数量，并设置上限；否则错位字段可能导致越界或卡死。
- 可能抛出故障时使用栈上缓冲区，例如 `CBufferStringN<256> buf(false)`，避免堆对象析构路径不完整。

## 错误结论

- `Fail` 只验证类型/布局保证，不要把“运行中的服务器通常应该有某数据”当成断言。实测为空的容器不代表布局错位。
- SDK 没有写明的语义不要臆测；记录返回值，用非空、对象类型等无争议事实做断言。
- oracle 全部失败时先核对基对象、数组所属对象、`offsetof` 和步长。整组一致失败通常是 oracle 算错，不是 SDK 整体错位。
- 不要默认接口指针就是 SDK 命名的具体类；先用 getter 与字段对照等 oracle 验证。

## 覆盖

- 只读方法也应覆盖；“同类版本已测”“调用昂贵”“拿不到 key”都不是默认跳过理由。先从现场数据、map key 或 ID 构造参数。
- 审计类名必须精确匹配；脚本已处理注释，注释掉的声明不算虚方法。
- 每个跳过的方法单独一行，并写可重新评估的具体原因；不同分支中的重复 `SkipCall` 可能是有意的。

## 布局与 hook

- 偏移一律用 `offsetof`，不要写常数；`bool` 用原始字节读取；整数范围只在类型确实有范围时检查。
- **`offsetof` 能否使用取决于成员的访问级别，三种情况三种办法。** 不要因为 `offsetof` 用不了就整个结构体不测：

  | 访问级别 | 办法 |
  |---|---|
  | public | 直接 `offsetof` |
  | protected | 派生一个**不加任何成员**的探针类，在类内用 `offsetof` 取偏移；再用 `sizeof(探针) == sizeof(原类型)` 断言探针没改变布局 |
  | private | 用 `testkit/PrivateField.h` 的 `CS2ST_PRIVATE_FIELD` 捕获成员指针，再 `PrivateFieldOffset<Tag>(实例)` 拿偏移。靠的是 [temp.spec]/6：显式模板实例化里不做访问检查。合法、无 UB，且偏移由编译器从声明算出 |

  **不要因为 `offsetof` 用不了就手写偏移。** "这字段明显在 0x08"这种推理通常没错，错的是把结果**写死**——header 一变（Valve 插字段、submodule bump 之后 SDK 跟着改），`offsetof` 和捕获的成员指针会自动跟上，手写的数字不会，于是测试开始读错字段**并且报绿**，恰好在这个项目唯一要抓的那个时刻骗人。多继承（两张虚表）、`alignas`、空基类优化、派生类复用基类尾部 padding，都是"看着显然"然后算错的常见来源，而算错同样是静默的。

  拿不到成员指针的字段（数组成员、真正没有声明的字节）再退到：① 用公开邻居反推（`offsetof(C, m_Next) - sizeof(void*)`）；② 用返回字段地址的 accessor 推（`ConVarData::Value()` 返回 `m_Values` 的地址）；③ **按值反查偏移**——用 getter 拿到已知值扫内存定位，再校验声明要求的相邻间距。③ 也可以和捕获的偏移互为独立第二路径：两者必须一致。

  ③的间距校验是关键：单个字段的偏移对不对无从判断，但声明里连续的几个同类型字段必须按 `sizeof` 等距排开，插入过字段就会破。
- 基类区间（vptr、基类自己的数据成员）不要跳过。那里插入字段会让下面所有 `offsetof` 行同时位移，而每一行单看都正常。vptr 在偏移 0、多态类第一个数据成员在 `sizeof(void*)` 是 ABI 规定而非手写常数，但要在注释里说明这个区别。
- `kMayBeUninitialized` 仅用于已确认引擎不会写入的字段，不能用来掩盖周围字段尚未验证的问题。
- hook 必须先 `Declare()`，安装 pre/post 成对出现并返回 `MRES_IGNORED`。`AddListener` 没有逆操作，卸载/重装时要避免重复注册。
