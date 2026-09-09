# 常用代码形状（按需阅读）

以下只保留新增 suite 时最常复用的形状；参数名和接口名按实际 SDK 替换。

## Acquire

```cpp
bool FooSuite::Acquire(TestContext& context)
{
    foo_ = static_cast<IFoo*>(context.QueryInterface("modulename", FOO_INTERFACE_VERSION));
    if (foo_ == nullptr)
    {
        const Module& module = context.GetModule("modulename");
        if (!module.IsValid())
            context.UnavailableCall("IFoo", "模块未加载");
        else
            context.UnavailableCall("IFoo", "模块未提供该版本接口");
        return false;
    }
    context.Note("interface at %p", static_cast<void*>(foo_));
    return true;
}
```

需要引擎调用的前置数据放在受保护的调用中：

```cpp
CS2ST_CALL(context, "获取前置数据", {
    globals_ = engine_->GetServerGlobals();
    context.Detail("-> %p", static_cast<void*>(globals_));
});
```

## 普通调用与 oracle

```cpp
CS2ST_CALL(context, "IFoo::GetCount", {
    const int count = foo_->GetCount();
    context.Detail("-> %d", count);
    if (count < 0)
        context.Fail("计数不应为负数");
});
```

优先比较两条独立路径：getter 与字段、按名称与按 ID、返回指针与 `offsetof` 计算地址、摘要值与其覆盖的数据。

## 结构体

`StructInspector` 不获取内存，它只是「某个地址 + 某个长度」上的一个视图：构造参数就是 `(context, 类名, base, size)`，每次读做 `base + offset`，按 `size` 边界检查，并走故障保护。地址由 suite 自己拿到（接口返回、工厂、别的对象里的字节偏移），inspector 从不分配也从不查询。

**一个声明类一个 inspector，不是一个对象一个。** 链上 `A ← B ← C` 拿到 C 的实例就发三段，字段行归属声明它的那个类：

```cpp
// 基类段：static_cast 拿到基类子对象地址，用基类自己的 sizeof
StructInspector base(context, "CFooBase",
                     static_cast<const CFooBase*>(instance), sizeof(CFooBase));
base.Pointer("__vftable", 0);                        // vptr 归引入它的类
base.Int32  ("m_nBase", offsetof(CFooBase, m_nBase));
base.Summarize();                                     // 基类段不 HexDump

// 派生段：只放自己声明的字段
StructInspector derived(context, "CFoo", instance, sizeof(CFoo));
derived.String ("m_pszName", offsetof(CFoo, m_pszName));
derived.Int32  ("m_nCount", offsetof(CFoo, m_nCount), 0, INT32_MAX);
derived.Bool   ("m_bFlag", offsetof(CFoo, m_bFlag));
derived.Summarize();
derived.HexDump();                                    // 只有最派生段 dump
```

`Summarize()` 在 `HexDump()` 之前：先给结论，再给看到失败的人拿去对照的原始字节。

1. 一个声明类一个 inspector。基类字段绝不写进派生类那段。
2. 每段指向该类自己的子对象，用 `static_cast<A*>(instance)`。不要假设偏移 0，也不要手工加基类偏移——多继承下第二个及以后的基类不在 0，虚继承下偏移要运行期查表，`static_cast` 两种都对。
3. 每段用该类自己的 `sizeof`。基类段只读基类声明的字节，边界检查才有意义。
4. 没有数据成员的类跳过（纯接口、`IAppSystem` 之类），用一行 Note 说明它只做调用覆盖。
5. vptr 行归引入它的那个类，在该子对象偏移 0；派生段不重复。
6. 偏移取自声明类：`offsetof(A, m)`；private 成员用 `CS2ST_PRIVATE_FIELD(..., A, ...)`，A 写声明类。
7. 顺序基类在前、派生在后。基类偏了后面全无意义，而「第一个失败的偏移才是有意义的那个」只在按布局顺序排列时成立。
8. 最派生段若只是**假定**对象是该类型（从接口指针或字节偏移推断出来的），前面必须有身份 oracle，日志里写明成立与否。基类段不需要：派生对象必然完整包含基类子对象，按基类 `sizeof` 读一定在界内。
9. `HexDump` 只在最派生段做。基类若有 tail padding 被派生类复用，基类段 dump 出来的尾部其实是派生类的字段。
10. 一个类一段 = `list-struct-coverage.sh` 里恰好登记一次、可追责任人。这是覆盖大量类时防重复的机制。

按声明顺序检查；大结构体覆盖中间字段；数组只扫描有限数量，并先验证指针和数量。

**布局目标的前置筛选**：同时满足①声明了数据成员 ②能拿到引擎创建的活实例。不满足的分开处理——纯接口只做调用覆盖；模板只 inspect 现场真实存在的那个实例化，登记名写全（`CUtlVector<ConVarRef>`）；拿不到实例的记一行「无可达实例」，**不要自己 new 一个来测**，那测的是我们自己的编译结果，不是游戏的。

## Hook

```cpp
recorder.Declare(kHookOnBar, Id());
SH_ADD_HOOK(IFoo, OnBar, foo_, SH_MEMBER(this, &FooSuite::Pre_OnBar), false);
SH_ADD_HOOK(IFoo, OnBar, foo_, SH_MEMBER(this, &FooSuite::Post_OnBar), true);

void FooSuite::Pre_OnBar(CPlayerSlot slot, const char* name)
{
    char scratch[48];
    CS2ST_HOOK_ENTER_ARGS(kHookOnBar, "slot=%d name=\"%.63s\"",
        slot.Get(), SafeStringArg(name, scratch));
    RETURN_META(MRES_IGNORED);
}
```

post 处理器记录离开并同样返回 `MRES_IGNORED`。签名必须直接复制 SDK 声明，不能凭感觉改写。

## 跳过

```cpp
context.SkipCall("IFoo::Reset", "会清空在线服务器上的绑定");
context.SkipCall("IFoo::FindByKey", "无法安全构造内部 key");
```

每个方法一行，说明具体副作用或参数为何无法安全获得。
