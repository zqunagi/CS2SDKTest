# Landmines

Each of these cost real time. They are grouped by what kind of damage they do:
things that stop the build, things that kill the server, things that produce a
confidently wrong log, and things that leave holes in coverage.

The silent ones are the expensive ones. A build error costs minutes; a `FAIL`
row that means nothing costs a review cycle and trains people to ignore the log.

---

## Build

### `#define` inside a macro argument list

**Symptom:** `error: 'CSchemaType_Builtin' does not refer to a value` on a line
that is obviously a macro invocation with a type argument.

**Cause:** `CS2ST_CALL(context, target, { ... })` expands to `[&]() { ... }`, so
that braced block is a **macro argument**. A preprocessor directive inside a
macro argument list is undefined behaviour. Clang processes it at the wrong
point, the inner macro never expands, and the invocation parses as a function
call whose first argument is a type name.

```cpp
CS2ST_CALL(context, target, {
#define CHECK(T, cat)  /* ... */          // UB - inside a macro argument
    CHECK(CSchemaType_Builtin, SCHEMA_TYPE_BUILTIN);
#undef CHECK
});
```

**Fix:** a namespace-scope template function, which is cleaner anyway:

```cpp
template <typename T>
void CheckDowncast(TestContext& context, CSchemaType* type, const char* type_name, ...);
```

A `#define` at **function scope** whose *body* contains `CS2ST_CALL` is fine —
`CS2ST_MAP_COUNT` in `SchemaSystemSuite` does exactly that. The rule is narrow:
never a directive *inside* an argument list.

### glibc steals `major` and `minor`

`<sys/sysmacros.h>` defines both as function-like macros and arrives
transitively via `<sys/types.h>`. Locals named `major`/`minor` — the obvious
names for `GetApiVersions(int& major, int& minor, ...)` — break in confusing
ways. Name them `api_major` / `api_minor`.

### SDK integer typedefs vs `<cstdint>`

Use `std::uint32_t` etc. for your own locals so the code doesn't depend on which
SDK header happened to be pulled in. The exception is a reference out-parameter:
`GetSizeAndAlignment(int&, uint8&)` needs exactly `uint8`, because a reference
demands the exact type. Comment those so nobody "tidies" them later.

Watch narrowing when OR-ing enumerators: `|` promotes to `int`, so
`static_cast<std::uint8_t>(A | B | C)` is needed to avoid a future
`-Wconversion` build breaking.

---

## Crashes

### `Acquire()` runs without a fault guard

`TestRunner::RunCalls` wraps the sweep in a `FaultGuardScope`. **`InstallHooks`
does not** — and it calls `Acquire()` too. An engine call in `Acquire()`
therefore takes the server down with nothing in the log on the hooks path.

Keep `Acquire()` to `QueryInterface` and null checks. Fetch anything else inside
`ExerciseCalls` with `CS2ST_CALL` around it, even when it feels like setup.

### Hook callbacks have no fault guard at all

The guard is only armed during a call sweep. A hook fires in the engine's own
frame, long after that scope is gone — so a fault inside a handler kills the
process rather than producing a `CRASHED` row.

Consequences for handler code:
- Vet every pointer before touching it (`LooksLikeObjectPointer` in `hook/HookArg.h`)
- Render every `const char*` through `SafeStringArg` and cap it with `%.63s`
- Never walk a structure you were handed
- Keep it cheap — several run every tick

A null check is not enough: a misread `const char*` argument is a float's bit
pattern, which is rarely null.

The compensating evidence is the trace line. The log flushes per line, so the
last `enter X` with no matching `leave X` names the handler the process died in.

### Unbounded walks over a count you don't trust

Every array length in these structs is a field whose offset is exactly what's
under test. If it's wrong, the number is arbitrary. Cap every walk
(`kMaxEntriesScanned`) and bounds-check the count before indexing — otherwise a
misplaced field turns a probe into a hang or a segfault.

### `CBufferString` on the heap

Construct with `CBufferStringN<256> buf(false)` — heap allocation disabled. A
fault mid-call skips every destructor between there and the guard; a stack-only
buffer cannot leak.

---

## Wrong conclusions

These are the worst class of bug here, because the suite passes review and
reports something false.

### Semantic assertions dressed up as layout checks

`Fail` means *the SDK's declaration says this cannot be so*. It does not mean
*I expected something else*.

Measured on a live server: `m_LoadedModules`, `m_ResourceManifestDescs`,
`m_DetectedSchemaMismatches` and `m_CompleteModuleRegistrationCallbacks` are all
empty, while `m_nRegistrations` reads 4109 and 17 type scopes are registered.
Registration plainly happened; those containers just don't retain anything. An
assertion that "a running server has loaded modules" was **wrong**, and it was
the project's own stated rule being broken: assert what the *type* implies (a
count cannot be negative), never what the semantics ought to be.

The layout was fine, and the proof is worth internalising: `m_TypeScopes` sits
*after* the empty containers and read back exactly 17, matching
`GetNumStrings()`. **Misalignment does not repair itself** — a correct field
downstream proves the empty one above it is genuinely empty.

### Inventing a contract the SDK never stated

`GetTypeScopeForBinding` takes a `SchemaTypeScope_t`. It looks obvious that
`SCHEMA_GLOBAL_TYPE_SCOPE` returns the global scope. Measured: with a module
name supplied, **all three enum values return that module's scope**, including
`GLOBAL`, while `DefaultTypeScopeIsLocal()` was false.

Two `Fail`s were asserting a contract that exists nowhere in the SDK. When
semantics are undocumented, record what each input actually returns — that
record is the deliverable — and assert only what needs no guess (non-null, and
the answer is an object of the right kind).

### An oracle computed from the wrong base object

The strongest check in the project compares a virtual call's return against an
address computed with `offsetof`. It reported all fourteen builtins as "moved".
The arithmetic said otherwise:

```
Type_Builtin(0) on the libserver.so scope -> 0x7fc0c0db8c98
GlobalTypeScope()                         -> 0x7fc0c0db8b80
                                    difference = 0x118
stride between consecutive builtins        = 0x28 = 40
```

`offsetof(CSchemaSystemTypeScope, m_BuiltinTypes)` is `0x118` and
`sizeof(CSchemaType_Builtin)` is 40. Both matched exactly — **the SDK was
right**. The builtin array lives in the *global* scope; a module scope delegates
to it. The test was computing the address from the wrong object.

Which also explained a module scope reporting `m_bBuiltinTypesInitialized ==
false`: it has no array of its own to initialise.

**When an oracle fails across the board, suspect the oracle.** A real layout
break is usually one field, and everything after it. Fourteen-for-fourteen with
a consistent stride is a wrong base pointer. Do the arithmetic before editing
the SDK.

### Assuming the object is the class the SDK names

Casting an interface pointer to the concrete class the SDK claims backs it is an
assumption. Test it before believing anything read through it: find a virtual
getter and the field it must be returning, and check they agree. In
`SchemaSystemSuite` that is `SchemaSystemIsReady()` vs
`m_bSchemaSystemIsReady` — cheap, and it gates everything downstream.

---

## Coverage holes

### Skipping read-only methods for bad reasons

Called out in review, and rightly. All three of these were wrong:

- *"twin of the class version"* — separate slot, separate implementation
- *"the call is expensive"* — cost is not correctness
- *"can't construct the lookup key"* — usually you can, from live data

The by-ID overloads of `FindRawClassBinding` / `FindRawEnumBinding` looked
untestable because class IDs are internal. They're the **keys of
`m_DeclaredClasses`**. Walking the map produced them, and closed the last
untested lookups on the interface. Look for the data before concluding it's
unreachable.

### Substring class-name matching

An audit anchored on `ISchemaSystem` also matches `ISchemaSystemTypeScope`, so
it audits the wrong class and reports everything covered. The bundled script
compares the parsed name for **exact** equality. If you write your own
one-off check, do the same.

### Commented-out declarations

`eiface.h` has `// virtual void SetCommandClient(...)` — commented out, so not
in the vtable and correctly untestable. An extractor that doesn't strip comments
reports it as a coverage gap and sends someone hunting for a method that doesn't
exist. The bundled script strips `//` and `/* */`.

### Skips merged into families

"The `Type_` family, 14 methods" tells a reader neither which fourteen nor
whether the reason holds for all of them — and it hid the fact that
`Type_Builtin` is a pure lookup that *should* be called. One line per method.

### Duplicate skip targets are usually fine

Two `SkipCall`s with the same target but different reasons, in branches guarded
by early `return`, are correct — only one is reachable per run. Check the
control flow before "fixing" it.

---

## Struct inspection

From `StructInspector.h` and `docs/STATUS.md`, restated because they are easy to
get wrong under time pressure:

- **Offsets come from `offsetof`, never hand-written numbers.** The comparison is
  "the layout the SDK assumes" against "the memory the game handed us", so the
  assumption must be computed from the headers. A literal `0x54` silently stops
  tracking a submodule bump.
- **`bool` is read as a raw byte.** Reading it as `bool` lets the compiler
  normalise any non-zero to `true` and the evidence disappears.
- **`String()` must not require 8-byte alignment.** String literals pack into
  `.rodata` at arbitrary bytes. This produced a false positive on
  `CGlobalVars::startspot` once already. Object pointers *are* aligned —
  `Pointer()` checks that, `String()` must not.
- **`int` has no general criterion** — every bit pattern is a legal int. Pass a
  range only where the *type* implies one (a count cannot be negative), never
  where the game's semantics do.
- **`kMayBeUninitialized`** is for fields the engine never writes (several SDK
  constructors are literally `{}`). It downgrades `FAIL` to `SUSPECT` without
  relaxing the check. Only apply it once the surrounding fields are known good —
  an isolated violation between two correct fields cannot be misalignment,
  because misalignment does not repair itself.

---

## Hooks

- **`Declare()` before installing.** A hook that never fires must still appear in
  the dump as `never`. "The engine stopped calling this" is the most valuable
  row after an update, and a silently missing one says nothing.
- **Always `MRES_IGNORED`**, including `RETURN_META_VALUE(MRES_IGNORED, x)`,
  where the value is discarded and the game's own result stands. That is what
  makes this observation rather than interference. `ClientConnect` decides
  whether a player gets on the server — `MRES_SUPERCEDE` there turns a
  diagnostic into an outage.
- **Pre/post pairs.** Pre records entry, post records return. Unequal counts mean
  the original didn't return — it faulted, or another plugin superseded it —
  which is a different failure from "the engine stopped calling", and only the
  pair distinguishes them.
- **`ISmmAPI::AddListener` has no inverse.** Metamod drops a plugin's listeners
  only at unload. So "removing" `IMetamodListener` hooks can only stop the
  recording, and re-installing must not register twice or every callback is
  counted twice. `MetamodListenerSuite` keeps `listener_registered_` separate
  from `recording_` for exactly this.
