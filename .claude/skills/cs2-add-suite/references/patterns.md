# Code patterns

Shapes that already work in this repo, with the reasoning. Copy them; the
reasoning is what tells you when to deviate.

## Acquire

```cpp
bool FooSuite::Acquire(TestContext& context)
{
    foo_ = static_cast<IFoo*>(context.QueryInterface("modulename", FOO_INTERFACE_VERSION));

    if (foo_ == nullptr)
    {
        // Distinguish the two failures: "module absent" is expected on a
        // dedicated server for client-only modules, while "module present but
        // no longer answers this version" is the finding.
        const Module& module = context.GetModule("modulename");
        if (!module.IsValid())
        {
            context.UnavailableCall("IFoo", "the modulename module is not loaded");
        }
        else
        {
            context.UnavailableCall("IFoo",
                "modulename is loaded but does not answer '%s'", FOO_INTERFACE_VERSION);
        }
        return false;
    }

    context.Note("interface at %p (%s)", static_cast<void*>(foo_), FOO_INTERFACE_VERSION);
    return true;
}
```

Nothing else belongs here — see the fault-guard pitfall. Prerequisites that
require an engine call go in `ExerciseCalls`, wrapped:

```cpp
CS2ST_CALL(context, "IVEngineServer2::GetServerGlobals (for maxClients)", {
    IVEngineServer2* engine = static_cast<IVEngineServer2*>(
        context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
    if (engine == nullptr) { context.Detail("-> engine2 does not answer"); return; }
    globals_ = engine->GetServerGlobals();
    context.Detail("-> %p, maxClients %d", static_cast<void*>(globals_),
        globals_ != nullptr ? globals_->maxClients : -1);
});
```

## A call

```cpp
CS2ST_CALL(context, "IFoo::GetCount", {
    const int count = foo_->GetCount();
    context.Detail("-> %d", count);
    if (count < 0)
    {
        context.Fail("a count cannot be negative");   // the type says so
    }
});
```

`Detail` always runs; `Fail` only when the SDK's own declaration is
contradicted. Print the value even when it passes — the log is for diffing, so
a value that changes next month should be visible without a failure.

## Oracles — the strongest checks available

Two routes to one fact, sharing no code. A generic check says "this pointer
looks plausible"; an oracle says "this is the wrong object".

**A getter against the field it must read.** Also the cheapest way to test the
assumption that an interface pointer really is the concrete class the SDK names:

```cpp
CS2ST_CALL(context, "CFoo::m_bReady vs IsReady()", {
    const bool from_call = foo_->IsReady();
    const bool from_field = concrete_->m_bReady;
    context.Detail("call %s vs field@0x%zx %s",
        from_call ? "true" : "false",
        static_cast<std::size_t>(offsetof(CFoo, m_bReady)),
        from_field ? "true" : "false");
    if (from_call != from_field)
    {
        context.Fail("the getter and the field it must be reading disagree");
    }
});
```

**A returned pointer against an address computed from the declared layout.**
This confirms the vtable slot *and* the struct layout at once:

```cpp
const CBar* from_layout = reinterpret_cast<const CBar*>(
    reinterpret_cast<const unsigned char*>(owner)
    + offsetof(COwner, m_Bars) + sizeof(CBar) * index);
if (from_call != from_layout) { context.Fail(...); }
```

Get the base object right — see the pitfalls file, this exact check reported
fourteen false failures because the array lived on a different object than the
one being asked.

**Two lookups of the same thing** (by name vs by ID, handle-wrapped vs raw) must
return the identical pointer. Catches a slot swap neither call alone would.

**A round trip:** render a name, feed it back to the lookup, expect the original
object.

**A cached summary against the data it summarises:** `m_nMinEnumeratorValue`
against every enumerator's `m_nValue` — every entry inside the range, and the
extremes actually attained.

**A count against the array it counts:** `(count > 0) != (pointer != nullptr)`
is a contradiction, and it catches a field inserted between the two.

## Struct layout

```cpp
StructInspector inspect(context, "CFoo", instance, sizeof(CFoo));

inspect.Pointer("m_pNext",  offsetof(CFoo, m_pNext));
inspect.String ("m_pszName", offsetof(CFoo, m_pszName));
inspect.Int32  ("m_nCount", offsetof(CFoo, m_nCount), 0, INT32_MAX);
inspect.Bool   ("m_bFlag",  offsetof(CFoo, m_bFlag));
inspect.Raw    ("m_Opaque", offsetof(CFoo, m_Opaque), 16);
inspect.HexDump();
inspect.Summarize();
```

List fields in declaration order so the log reads down the struct — that is what
makes "the first failure is where it diverged" legible. Address base-class
fields through the base class so offsets survive an insertion between the two.

For a large struct, don't stop at the head and tail. The middle is exactly where
an inserted field hides. A run of plausible counts read at distinct offsets
through that middle pins the whole span:

```cpp
#define CS2ST_MAP_COUNT(member)                                                 \
    CS2ST_CALL(context, "CFoo::" #member ".Count", {                            \
        const int count = foo_->member.m_Map.Count();                           \
        context.Detail("@0x%zx -> %d",                                          \
            static_cast<std::size_t>(offsetof(CFoo, member)), count);           \
        if (count < 0 || count > kMaxPlausibleCount)                            \
        {                                                                       \
            context.Fail("%d is not a count; this offset is not that map", count); \
        }                                                                       \
    })

CS2ST_MAP_COUNT(m_Pointers);
CS2ST_MAP_COUNT(m_Atomics);
#undef CS2ST_MAP_COUNT
```

A function-scope `#define` whose body contains `CS2ST_CALL` is fine. A `#define`
*inside* a `CS2ST_CALL` argument is undefined behaviour — see pitfalls.

A polymorphic array only needs its first element's vtable pointer checked; that
is the same evidence the whole array gives, at a fraction of the log.

## Walking a list safely

```cpp
CS2ST_CALL(context, "CFoo::m_pItems (bounds)", {
    items_ = foo_->m_pItems;
    item_count_ = static_cast<int>(foo_->m_nItemCount);
    context.Detail("-> %p, %d item(s)", static_cast<void*>(items_), item_count_);
    if (items_ == nullptr || item_count_ <= 0) { items_ = nullptr; return; }
    if (item_count_ > kMaxEntriesScanned)
    {
        context.Detail("-> scanning the first %d", kMaxEntriesScanned);
        item_count_ = kMaxEntriesScanned;
    }
});
if (items_ == nullptr) { context.SkipCall("CFoo items", "no items exposed"); return; }
```

Vet once, then walk. Print the first few individually so a fault names the index
it happened at; check *all* of them in aggregate, because the value of the scan
is the one outlier among two hundred.

## Hooks

```cpp
SH_DECL_HOOK2_void(IFoo, OnBar, SH_NOATTRIB, 0, CPlayerSlot, const char*);
```

Copy the signature from the SDK rather than paraphrasing. SourceHook derives its
calling convention from what you write, so a mismatch doesn't fail to compile —
it silently hands every handler garbage, which looks exactly like the drift the
suite exists to detect.

```cpp
recorder.Declare(kHookOnBar, Id());        // before installing, always

#define CS2ST_ADD_PAIR(method) \
    SH_ADD_HOOK(IFoo, method, foo_, SH_MEMBER(this, &FooSuite::Pre_##method), false); \
    SH_ADD_HOOK(IFoo, method, foo_, SH_MEMBER(this, &FooSuite::Post_##method), true)
```

Handler, remembering there is **no fault guard** here:

```cpp
void FooSuite::Pre_OnBar(CPlayerSlot slot, const char* name)
{
    char scratch[48];
    CS2ST_HOOK_ENTER_ARGS(kHookOnBar, "slot=%d name=\"%.63s\"",
        slot.Get(), SafeStringArg(name, scratch));
    RETURN_META(MRES_IGNORED);
}

void FooSuite::Post_OnBar(CPlayerSlot, const char*) 
{
    CS2ST_HOOK_LEAVE(kHookOnBar);
    RETURN_META(MRES_IGNORED);
}
```

Where an argument's validity is itself checkable, check it — that is where a
signature change shows up. A protobuf enum has a generated `_IsValid`; a
`CCommand`'s `ArgC()` can be bounds-checked before any argument is read; an
engine-constructed `CBufferString` can be read as a *struct*
(`Length() <= AllocatedNum()`) rather than as text.

## Skips

```cpp
context.SkipCall("IFoo::Reset", "would clear every binding on a live server");
context.SkipCall("IFoo::FindByKey",
    "keyed by an internal ID with no route to obtain one from outside");
```

One line per method. State what would happen, not just "side effects" — the next
reader needs enough to re-evaluate the decision, and several skips in this repo
were reversed once someone did.
