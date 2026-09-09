---
name: cs2-add-suite
description: >
  How to add a new test suite to CS2SDKTest - the Metamod plugin that exercises
  hl2sdk interfaces so a CS2 update's breakage shows up as a log diff. Use this
  whenever the work involves covering an SDK interface in this repo: "add a
  suite", "cover ISchemaSystem / IFileSystem / ICvar / CGameEntitySystem", "do
  the next item in TEST_TARGETS.md", "test this interface", or when reviewing or
  extending an existing file in src/suites/. Also use it when a suite's log
  comes back with FAIL/CRASHED rows and the cause has to be sorted into "the SDK
  changed" versus "the test is wrong" - that judgement has burned this project
  before and the skill records how it went wrong. Covers the completeness audit
  (no interface method left unmentioned), struct-layout coverage and its
  project-wide deduplication rule, the call/hook/oracle patterns, and the
  landmines that have already cost real review cycles.
---

# Adding a test suite to CS2SDKTest

## What a suite is for

This plugin's product is **a log you can diff after a CS2 update**. A row that
changes from `OK` to `CRASHED`, or a hook that goes from a count to `never`, is
the finding. Everything below serves that: coverage so nothing is silently
absent, and judgement so the rows that do change mean something.

Before writing anything, read in this order:

1. `docs/STATUS.md` — the decisions and the measured findings. The "走过的弯路"
   and "实测发现" sections exist so you don't rediscover them.
2. `docs/TEST_TARGETS.md` — what to write next and why it's ranked there.
3. The closest existing suite. `Source2ServerSuite` for a hook-heavy interface,
   `SchemaSystemSuite` for a data/layout-heavy one, `Source2EngineSuite` for the
   simplest complete example.

## The three rules

These are the ones that get violated under time pressure, so they come first.

### 1. Every method is accounted for — called, or skipped with a reason

A reader has to be able to tell *"checked, fine"* from *"nobody looked"*. A
missing line says neither. So every method on the interface either gets a real
call or a `SkipCall` line naming why.

Verify it mechanically rather than by eye — a 50-method interface will beat you:

```sh
sh .claude/skills/cs2-add-suite/scripts/audit-coverage.sh \
   hl2sdk/public/eiface.h ISource2GameClients src/suites/Source2GameClientsSuite.cpp
```

Exit status is the number of unaccounted methods. Run it on **every** class the
suite touches — the interface, its concrete class, and any polymorphic type it
hands out. Do not declare the suite finished until each reports `all N accounted
for`.

**"Only read-only methods get called" is not a licence to skip liberally.** The
reasons that sound respectable and are not:

| Excuse | Why it's wrong |
|---|---|
| "twin of the class version, already covered" | Separate vtable slot, separate implementation. A slot shift can hit one family and not the other. |
| "the call is expensive" | Cost is not correctness. If it's read-only, call it. |
| "can't construct the lookup key" | Usually you can, from live data — a field's type carries its own atomic ID, a map's key *is* the ID the by-ID overload wants. Look before concluding. |

Legitimate reasons to skip: it mutates state, it needs an argument that cannot
be synthesised safely, it only spews to console, or it writes through a pointer
that outlives the call. Write the actual reason — "side effects" alone doesn't
let the next reader re-evaluate it.

List skips **one method per line**. "The Type_ family, 14 methods" tells a
reader neither which fourteen nor whether the reason applies to all of them.

### 2. Inspect every struct layout the suite can reach

Interfaces hand out structs, and those are where `StructInspector` earns its
keep. Concrete classes with real members — not pure interfaces — are the high
value targets: get the layout wrong and property reads silently return the
neighbouring field.

Reach for a struct whenever the interface returns one, fills one as an out
parameter, or exposes one as a public member. `SchemaSystemSuite` inspects nine.

### 3. Each struct is inspected exactly once, project-wide

A layout either holds or it doesn't — it cannot hold in one suite and fail in
another. A second inspection adds log noise and zero evidence.

Check who already owns a struct before writing a block for it:

```sh
sh .claude/skills/cs2-add-suite/scripts/list-struct-coverage.sh
```

If it's listed, leave a comment instead:

```cpp
// CGlobalVars' layout is inspected in Source2EngineSuite. A layout either holds
// or it does not - it cannot hold there and fail here - so repeating the check
// would add thirty log lines and no evidence.
```

The script reads the source, so it can't go stale. Re-run it after adding
inspections to confirm the new struct is registered under your suite.

## Workflow

**1 — Read the SDK declaration end to end.** Every method, every member, and the
*order* of the virtuals. Note where the destructor is declared: under the
Itanium ABI it takes two vtable slots wherever it sits, so an interface
declaring `virtual ~IFoo() = 0` last (the SDK does this) is a live risk that
every method slot is off by two if Valve declares it first. Say so in the header
comment and name the slot-0 call that would prove it.

**2 — Write the header.** Forward-declare the interface and keep the SDK out,
*unless* hook handlers must spell out exact parameter types — then include the
SDK header and say why in a comment. Document what the suite covers and what it
deliberately doesn't.

**3 — `Acquire()`.** Resolve through `context.QueryInterface(module, VERSION)`.
Distinguish "module absent" from "module present but doesn't answer this
version" — the second is the finding. Return `false` to skip the suite cleanly.

> `Acquire()` also runs on the `hooks install` path, which has **no fault guard
> around it**. Any engine call you make here crashes the server with nothing in
> the log. Fetch prerequisites inside `ExerciseCalls` with `CS2ST_CALL` instead.

**4 — `IAppSystem` base slots.** Copy the block from any existing suite. The
first 11 slots sit ahead of everything else, so if they're wrong nothing below
is meaningful. Each has one correct answer, which makes it a vtable alignment
probe rather than a liveness check.

**5 — The calls.** One `CS2ST_CALL` per method. Assert whatever the *type*
guarantees, never what the semantics ought to be — see the pitfalls file, this
is the rule the project most recently broke.

**6 — Struct layouts.** Rules 2 and 3 above. Offsets come from `offsetof`,
never hand-written numbers.

**7 — Hooks, if the engine calls into the game through this interface.**
`Declare()` every hook before installing so one that never fires still shows as
`never`. Install pre/post pairs. Return `MRES_IGNORED` always — this plugin
observes and must not intervene.

**8 — Register and audit.** Add to `TestRunner::CreateDefault()`, both lists in
`CMakeLists.txt`, and `src/suites/README.md`. Then run the audit script on every
class touched, and the struct script to confirm registration.

## Grading a result: type-level, not semantic

`Fail` means *the SDK's own declaration says this cannot be so*. Anything softer
is a `Detail` line, or `Suspect` when there's a known benign explanation.

```cpp
// Good - the type guarantees it
if (count < 0) ctx.Fail("a count cannot be negative");
if ((alignment & (alignment - 1)) != 0) ctx.Fail("%u is not a power of two", alignment);

// Bad - you are asserting how the game ought to behave
if (modules == 0) ctx.Fail("a running server has loaded modules");   // measured false
```

The strongest checks available are **oracles**: the same fact reachable two ways
that share no code, so a disagreement is proof rather than suspicion. A virtual
getter against the field it must be reading; a lookup by name against a lookup
by ID; a returned pointer against an address computed with `offsetof`. Reach for
these wherever the interface offers two routes to one fact.

## Before declaring it done

- `audit-coverage.sh` reports `all N accounted for` for every class touched
- `list-struct-coverage.sh` shows the new structs under your suite, and no
  struct is inspected twice
- Every `Fail` is type-level; semantic expectations are `Detail` or `Suspect`
- Hook handlers dereference nothing unvetted (they run with **no** fault guard)
- Registered in `TestRunner`, `CMakeLists.txt` (both lists), `README.md`
- It compiles — `cmake --build --preset linux-x64-release`
- After a real run: sort every `FAIL` into "SDK changed" vs "test is wrong"
  before touching the SDK. Historically it has always been the test.

## Reference files

- `references/pitfalls.md` — the landmines, each with the symptom that exposed
  it. Read this before writing; several are silent and cost a review cycle each.
- `references/patterns.md` — code shapes for calls, hooks, layouts and oracles,
  with the reasoning behind each.
