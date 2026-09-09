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

```cpp
StructInspector inspect(context, "CFoo", instance, sizeof(CFoo));
inspect.Pointer("m_pNext", offsetof(CFoo, m_pNext));
inspect.String("m_pszName", offsetof(CFoo, m_pszName));
inspect.Int32("m_nCount", offsetof(CFoo, m_nCount), 0, INT32_MAX);
inspect.Bool("m_bFlag", offsetof(CFoo, m_bFlag));
inspect.HexDump();
inspect.Summarize();
```

按声明顺序检查；大结构体覆盖中间字段；数组只扫描有限数量，并先验证指针和数量。

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
