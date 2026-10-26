#include "suites/ConVarSuite.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include <appframework/iappsystem.h>
#include <tier0/buffer_string.h>

#include "hook/HookArg.h"
#include "hook/HookRecorder.h"
#include "testkit/PrivateField.h"
#include "testkit/StructInspector.h"
#include "testkit/UtlVectorGuard.h"

// The SH_DECL_HOOK block used to live here; under KHook the hook templates are
// declared as members in the header instead, bound to the SDK member pointers
// in the constructor at the bottom of this file. The signatures are icvar.h's
// own, defaulted arguments included in the count: a defaulted parameter is
// still a parameter as far as the ABI is concerned, so CallGlobalChangeCallbacks
// is five and RegisterConCommand is two even though the last of each has a
// default in the declaration.

namespace cs2st
{

// ConVarData is entirely private - sixteen fields, no public data member to
// anchor on - so every offset below is captured through the explicit-
// instantiation access hole. See testkit/PrivateField.h for why that is legal
// and why a hand-written number would not do.
//
// At cs2st scope on purpose: an explicit instantiation must appear in an
// enclosing namespace of its template, which the anonymous namespace below is
// not. Declaration order matches the header, so the inspector rows read down
// the struct.
// ICvar's only data member. Reachable the same way, which removes the last
// hand-derived offset in this suite: it used to be written as sizeof(void*)
// on the reasoning "vptr first, then the first member" - true today, and an
// assumption about the declaration rather than something the compiler
// checked. Now the compiler computes it.
CS2ST_PRIVATE_FIELD(ICvarMaxSplitScreenSlots, ICvar, int, m_MaxSplitScreenSlots);

CS2ST_PRIVATE_FIELD(ConVarDataName,          ConVarData, const char*,    m_pszName);
CS2ST_PRIVATE_FIELD(ConVarDataDefaultValue,  ConVarData, CVValue_t*,     m_defaultValue);
CS2ST_PRIVATE_FIELD(ConVarDataMinValue,      ConVarData, CVValue_t*,     m_minValue);
CS2ST_PRIVATE_FIELD(ConVarDataMaxValue,      ConVarData, CVValue_t*,     m_maxValue);
CS2ST_PRIVATE_FIELD(ConVarDataHelpString,    ConVarData, const char*,    m_pszHelpString);
CS2ST_PRIVATE_FIELD(ConVarDataVarType,       ConVarData, EConVarType,    m_eVarType);
CS2ST_PRIVATE_FIELD(ConVarDataVersion,       ConVarData, short,          m_Version);
CS2ST_PRIVATE_FIELD(ConVarDataTimesChanged,  ConVarData, unsigned int,   m_iTimesChanged);
CS2ST_PRIVATE_FIELD(ConVarDataFlags,         ConVarData, uint64,         m_nFlags);
CS2ST_PRIVATE_FIELD(ConVarDataCallbackIndex, ConVarData, unsigned int,   m_iCallbackIndex);
CS2ST_PRIVATE_FIELD(ConVarDataFilterCBIndex, ConVarData, unsigned int,   m_iFilterCBIndex);
CS2ST_PRIVATE_FIELD(ConVarDataComplCBIndex,  ConVarData, unsigned int,   m_iCompletionCBIndex);
CS2ST_PRIVATE_FIELD(ConVarDataGameInfoFlags, ConVarData, int,            m_GameInfoFlags);
CS2ST_PRIVATE_FIELD(ConVarDataUserInfoIndex, ConVarData, int,            m_UserInfoByteIndex);
CS2ST_PRIVATE_FIELD(ConVarDataEnumInfoFn,   ConVarData, FnGetEnumInfoHandle_t, m_fnGetEnumInfoHandle);

namespace
{

// -----------------------------------------------------------------------------
//  Ground truth: what our own callbacks saw
// -----------------------------------------------------------------------------

/**
 * Everything the probe callbacks record.
 *
 * Fixed-size and POD on purpose. These run inside engine calls, and on the
 * command path they can be triggered by an operator typing at the console, so
 * nothing here may allocate.
 */
struct ProbeState
{
	int int_changes = 0;
	int int_filters = 0;
	int32 int_change_new = 0;
	int32 int_change_old = 0;
	int int_change_slot = 0;

	int bool_changes = 0;

	int probe_global_changes = 0;
	char probe_global_name[64] = {};
	char probe_global_new[64] = {};

	int command_dispatches = 0;
	int command_argc = 0;
	int command_target = 0;
	char command_arg1[64] = {};
};

ProbeState g_probe;

/**
 * The change callback on cs2sdktest_int.
 *
 * Registered through ConVarValueInfo_t::SetCallback, which wraps it in a
 * generated provider thunk. So a correct invocation here proves rather a lot:
 * that the engine found the callback index in the ConVarData it allocated,
 * that it called our provider with the right five arguments, and that the
 * provider cast them back to the typed signature correctly.
 */
void ProbeIntChanged(CConVar<int32>* cvar, CSplitScreenSlot slot,
                     const int32* new_value, const int32* old_value)
{
	(void)cvar;
	++g_probe.int_changes;
	g_probe.int_change_slot = slot.Get();
	// Vetted before dereferencing: a drifted signature puts something that is
	// not a pointer in these registers, and this runs with no fault guard.
	if (LooksLikeObjectPointer(new_value))
	{
		g_probe.int_change_new = *new_value;
	}
	if (LooksLikeObjectPointer(old_value))
	{
		g_probe.int_change_old = *old_value;
	}
}

/** The filter callback on cs2sdktest_int. Always allows: observation only. */
bool ProbeIntFilter(CConVar<int32>* cvar, CSplitScreenSlot slot,
                    const int32* new_value, const int32* old_value)
{
	(void)cvar;
	(void)slot;
	(void)new_value;
	(void)old_value;
	++g_probe.int_filters;
	return true;
}

void ProbeBoolChanged(CConVar<bool>* cvar, CSplitScreenSlot slot,
                      const bool* new_value, const bool* old_value)
{
	(void)cvar;
	(void)slot;
	(void)new_value;
	(void)old_value;
	++g_probe.bool_changes;
}

/**
 * The global change callback used by the closed-loop test in the sweep.
 *
 * Distinct from the persistent one below so install and remove can be verified
 * against a counter that nothing else touches - two different function
 * pointers, so the engine treats them as two different registrations.
 */
void ProbeGlobalChanged(ConVarRefAbstract* ref, CSplitScreenSlot slot,
                        const char* new_value, const char* old_value,
                        ConVarUserInfoSet_t* userinfo_data)
{
	(void)slot;
	(void)old_value;
	(void)userinfo_data;
	++g_probe.probe_global_changes;

	char scratch[48];
	if (LooksLikeObjectPointer(ref))
	{
		std::snprintf(g_probe.probe_global_name, sizeof(g_probe.probe_global_name),
			"%.63s", SafeStringArg(ref->GetName(), scratch));
	}
	std::snprintf(g_probe.probe_global_new, sizeof(g_probe.probe_global_new),
		"%.63s", SafeStringArg(new_value, scratch));
}

/** Hook ids. Used as both the log label and the recorder key. */
constexpr const char* kDispatchConCommand = "ICvar::DispatchConCommand";
constexpr const char* kRegisterConVar     = "ICvar::RegisterConVar";
constexpr const char* kRegisterConCommand = "ICvar::RegisterConCommand";
constexpr const char* kCallGlobalChange   = "ICvar::CallGlobalChangeCallbacks";
constexpr const char* kUnregisterConVar   = "ICvar::UnregisterConVarCallbacks";
constexpr const char* kUnregisterConCmd   = "ICvar::UnregisterConCommandCallbacks";
constexpr const char* kGlobalChangeCb     = "ICvar::<installed global change callback>";
constexpr const char* kOnConVarCreated    = "IConVarListener::OnConVarCreated";
constexpr const char* kOnConCommandCreated = "IConVarListener::OnConCommandCreated";

/** Hooked methods plus the two native observation mechanisms. */
constexpr int kHookedMethodCount = 8;

/**
 * The persistent global change callback, installed by InstallHooks.
 *
 * A plain function rather than a member: FnChangeCallbackGlobal_t is a C
 * function pointer, so the engine has nowhere to put a `this`.
 */
void PersistentGlobalChanged(ConVarRefAbstract* ref, CSplitScreenSlot slot,
                             const char* new_value, const char* old_value,
                             ConVarUserInfoSet_t* userinfo_data)
{
	(void)userinfo_data;

	char name_scratch[48];
	char new_scratch[48];
	char old_scratch[48];

	const char* name = LooksLikeObjectPointer(ref)
		? SafeStringArg(ref->GetName(), name_scratch)
		: "<bad ref>";

	CS2ST_HOOK_ENTER_ARGS(kGlobalChangeCb, "slot=%d %.63s: \"%.31s\" -> \"%.31s\"",
		slot.Get(), name,
		SafeStringArg(old_value, old_scratch),
		SafeStringArg(new_value, new_scratch));
	CS2ST_HOOK_LEAVE(kGlobalChangeCb);
}

// -----------------------------------------------------------------------------
//  The probe convars and commands
// -----------------------------------------------------------------------------
//
// Declared at file scope so their constructors run when this shared object is
// mapped, which queues them in tier1's registration list; Plugin::Load then
// calls META_CONVAR_REGISTER, which drains that queue through
// ICvar::RegisterConVar. That is the SDK's own path for a plugin to declare a
// convar, and taking it means the rest of this file has values it chose itself
// to compare the engine against.
//
// Every one of them is a POD type. That is a deliberate constraint, not a
// coincidence: CS2SDKTEST_USE_MEMOVERRIDE is off, so this module allocates
// from its own heap rather than tier0's, and the project only gets away with
// that because no allocation crosses the module boundary. A CConVar<CUtlString>
// would break that - its value buffer would be allocated here and freed by the
// engine the first time anything reset it - so the string type is covered
// read-only, against an engine string cvar found by scanning the list.

constexpr uint64 kProbeFlags = FCVAR_GAMEDLL | FCVAR_RELEASE;

CConVar<int32> g_probe_int("cs2sdktest_int", kProbeFlags,
	"cs2sdktest: int32 probe convar, min 0 max 100 with change and filter callbacks",
	7, true, 0, true, 100, ProbeIntChanged, ProbeIntFilter);

CConVar<bool> g_probe_bool("cs2sdktest_bool", kProbeFlags,
	"cs2sdktest: bool probe convar with a change callback",
	true, ProbeBoolChanged);

CConVar<float32> g_probe_float("cs2sdktest_float", kProbeFlags,
	"cs2sdktest: float32 probe convar, min 0 max 10",
	1.5f, true, 0.0f, true, 10.0f);

CConVar<int64> g_probe_int64("cs2sdktest_int64", kProbeFlags,
	"cs2sdktest: int64 probe convar", static_cast<int64>(-5));

CConVar<Color> g_probe_color("cs2sdktest_color", kProbeFlags,
	"cs2sdktest: Color probe convar, a non-primitive type", Color(10, 20, 30, 40));

CConVar<Vector> g_probe_vec3("cs2sdktest_vec3", kProbeFlags,
	"cs2sdktest: Vector probe convar, a non-primitive type", Vector(1.0f, 2.0f, 3.0f));

/** FCVAR_PER_USER is what makes GetMaxSplitScreenSlots return more than 1. */
CConVar<int32> g_probe_peruser("cs2sdktest_peruser", kProbeFlags | FCVAR_PER_USER,
	"cs2sdktest: int32 probe convar with per-splitscreen-slot storage", 3);

/**
 * Exists only so the allow_defensive parameter of FindConVar has something to
 * be tested against. There is no other way to check that parameter: every
 * defensive cvar in the game is one we would have to guess the name of.
 */
CConVar<int32> g_probe_defensive("cs2sdktest_defensive", kProbeFlags | FCVAR_DEFENSIVE,
	"cs2sdktest: FCVAR_DEFENSIVE probe convar", 5);

/**
 * Two flags this SDK revision gave names to. Registered at startup like every
 * other probe so the flag word can be read back once the engine owns it:
 * FCVAR_SNAPSHOT_IGNORED (1ull<<30) excludes a convar from TakeConVarSnapshot
 * and ResetConVarsToSnapshot, FCVAR_GAMEINFO_CANNOT_OVERRIDE (1ull<<34) blocks
 * SetConVarsFromGameInfo from touching its default. Neither behavior is safe
 * to exercise directly (both reach beyond this plugin's convars), so the row
 * they feed checks the registration round-trip - the bit surviving into the
 * engine's flag word is the part this plugin can observe.
 */
CConVar<int32> g_probe_snapshot_ignored("cs2sdktest_snapshot_ignored",
	kProbeFlags | FCVAR_SNAPSHOT_IGNORED,
	"cs2sdktest: FCVAR_SNAPSHOT_IGNORED probe convar", 9);

CConVar<int32> g_probe_gameinfo_locked("cs2sdktest_gameinfo_locked",
	kProbeFlags | FCVAR_GAMEINFO_CANNOT_OVERRIDE,
	"cs2sdktest: FCVAR_GAMEINFO_CANNOT_OVERRIDE probe convar", 11);

/** Records what the engine handed the callback, for the dispatch oracle. */
void ProbeCommandCallback(const CCommandContext& context, const CCommand& args)
{
	++g_probe.command_dispatches;
	g_probe.command_argc = args.ArgC();
	g_probe.command_target = static_cast<int>(context.GetTarget());

	// Arg() bounds-checks and returns "" out of range, so this is safe even if
	// ArgC came back as nonsense.
	const char* first = args.Arg(1);
	char scratch[48];
	std::snprintf(g_probe.command_arg1, sizeof(g_probe.command_arg1), "%.63s",
		SafeStringArg(first, scratch));
}

// The other three callback shapes ConCommandCallbackInfo_t can hold, plus the
// two interface-style ones. None of these is ever registered with the engine:
// they exist so the discriminator bitfields and the dispatch branches can be
// exercised locally, which matters because a ConCommandCallbackInfo_t travels
// *into* the engine inside ConCommandCreation_t.

int g_void_callbacks = 0;
int g_contextless_callbacks = 0;
int g_interface_callbacks = 0;
int g_completion_fn_calls = 0;
int g_completion_interface_calls = 0;

void ProbeVoidCallback()
{
	++g_void_callbacks;
}

void ProbeContextlessCallback(const CCommand& args)
{
	(void)args;
	++g_contextless_callbacks;
}

struct ProbeCommandCallbackObject : public ICommandCallback
{
	void CommandCallback(const CCommandContext& context, const CCommand& command) override
	{
		(void)context;
		(void)command;
		++g_interface_callbacks;
	}
};

ProbeCommandCallbackObject g_callback_object;

void ProbeCompletionFn(const CCommand& command, CUtlVector<CUtlString>& completions)
{
	(void)command;
	++g_completion_fn_calls;
	// The vector belongs to whoever called Dispatch, and the only caller is
	// this suite with a stack vector of its own - so appending here allocates
	// and frees on the same side of the module boundary.
	completions.AddToTail(CUtlString("cs2sdktest_completion_a"));
	completions.AddToTail(CUtlString("cs2sdktest_completion_b"));
}

struct ProbeCompletionObject : public ICommandCompletionCallback
{
	int CommandCompletionCallback(const CCommand& command,
	                              CUtlVector<CUtlString>& completions) override
	{
		(void)command;
		++g_completion_interface_calls;
		completions.AddToTail(CUtlString("cs2sdktest_completion_iface"));
		return completions.Count();
	}
};

ProbeCompletionObject g_completion_object;

/**
 * No completion callback on either command, on purpose.
 *
 * A completion callback is invoked by the console with the console's own
 * CUtlVector, and filling it would allocate CUtlStrings here for the engine to
 * free - the module-boundary allocation the build is set up to avoid. The
 * completion methods are still exercised below, against a prefix that matches
 * only our own callback-less registrations.
 */
ConCommand g_probe_command("cs2sdktest_cmd", ProbeCommandCallback,
	"cs2sdktest: probe console command, dispatched by the convar suite",
	kProbeFlags);

ConCommand g_probe_command_defensive("cs2sdktest_cmd_defensive", ProbeCommandCallback,
	"cs2sdktest: FCVAR_DEFENSIVE probe console command",
	kProbeFlags | FCVAR_DEFENSIVE);

// -----------------------------------------------------------------------------
//  Scratch fixtures
// -----------------------------------------------------------------------------
//
// The probes above are the baseline every oracle compares the engine against,
// so a call that rewrites their declared shape (default, bounds, registration)
// would change what the rest of the log means. Those calls are made against
// these instead: a convar and a command nothing else reads, named outside the
// cs2sdktest_ prefix so the by-prefix oracles still match exactly what they
// were written for.
//
// Like the probes they are POD-valued and register at load, which is the one
// registration per plugin load that any plugin costs. The few calls that
// register or unregister something (CConVar, ConCommand, the member accessor)
// run on objects built in static storage and torn down again in the same
// call; g_runtime_fixtures_spent keeps a second sweep in the same load from
// leaking another set.

/** A flag no probe carries; ResetConVarsToDefaultValuesByFlag keys on it. */
constexpr uint64 kScratchFlags = kProbeFlags | FCVAR_UNLOGGED;

int g_scratch_command_calls = 0;
int g_scratch_completion_calls = 0;

void ScratchCommandCallback(const CCommandContext& context, const CCommand& args)
{
	(void)context;
	(void)args;
	++g_scratch_command_calls;
}

/**
 * Counts and appends nothing, on purpose: the console hands this the engine's
 * own CUtlVector, and an entry added here would be a CUtlString allocated from
 * this module's heap for the engine to free.
 */
void ScratchCompletionFn(const CCommand& command, CUtlVector<CUtlString>& completions)
{
	(void)command;
	(void)completions;
	++g_scratch_completion_calls;
}

CConVar<int32> g_scratch_int("cstmp_scratch_int", kScratchFlags,
	"cstmp: int32 scratch convar, min 0 max 100, mutated by the suite and restored",
	50, true, 0, true, 100);

ConCommand g_scratch_command("cstmp_scratch_cmd", ScratchCommandCallback,
	"cstmp: scratch console command with a completion callback that adds nothing",
	kProbeFlags | FCVAR_VCONSOLE_FUZZY_MATCHING,
	CompletionCallbackInfo_t(ScratchCompletionFn));

/**
 * Bounds handed to ConVarData::SetMinValue / SetMaxValue. The SDK stores the
 * pointer rather than copying, so they have to outlive the convar.
 */
CVValue_t g_scratch_min_value(static_cast<int32>(10));
CVValue_t g_scratch_max_value(static_cast<int32>(90));

struct ScratchOwner
{
	int calls = 0;

	void OnCommand(const CCommandContext& context, const CCommand& command)
	{
		(void)context;
		(void)command;
		++calls;
	}
};

ScratchOwner g_scratch_owner_a;
ScratchOwner g_scratch_owner_b;

typedef CConCommandMemberAccessor<ScratchOwner> ScratchAccessor;

alignas(CConVar<int32>) unsigned char g_runtime_cvar_storage[sizeof(CConVar<int32>)];
alignas(ConCommand) unsigned char g_runtime_command_storage[sizeof(ConCommand)];
alignas(ScratchAccessor) unsigned char g_runtime_accessor_storage[sizeof(ScratchAccessor)];

CConVar<int32>* g_runtime_cvar = nullptr;
ConCommand* g_runtime_command = nullptr;
ScratchAccessor* g_runtime_accessor = nullptr;

/** Set the first time the construct/destroy calls run in this load. */
bool g_runtime_fixtures_spent = false;

/**
 * The creation listener used by the closed-loop test.
 *
 * File scope rather than a local: the engine keeps the pointer for as long as
 * it is registered, and a fault between the register and the remove call would
 * leave it pointing at a dead stack frame.
 */
struct CreationListenerProbe : public IConVarListener
{
	int cvars = 0;
	int commands = 0;

	void OnConVarCreated(ConVarRefAbstract* new_cvar) override
	{
		(void)new_cvar;
		++cvars;
	}

	void OnConCommandCreated(ConCommand* new_command) override
	{
		(void)new_command;
		++commands;
	}
};

CreationListenerProbe g_creation_probe;

// -----------------------------------------------------------------------------
//  Small helpers
// -----------------------------------------------------------------------------

/** Cap on any walk of an engine-owned list. */
constexpr int kListWalkCap = 16384;

/** Names of our own registrations, for "did the iteration see them" checks. */
const char* const kProbeCvarNames[] = {
	"cs2sdktest_int",
	"cs2sdktest_bool",
	"cs2sdktest_float",
	"cs2sdktest_int64",
	"cs2sdktest_color",
	"cs2sdktest_vec3",
	"cs2sdktest_peruser",
	"cs2sdktest_defensive",
};

constexpr std::size_t kProbeCvarCount = sizeof(kProbeCvarNames) / sizeof(kProbeCvarNames[0]);

/**
 * What each probe convar was declared as, for the type round-trip check.
 *
 * At file scope rather than inside the CS2ST_CALL that uses it, and that is
 * not a style choice: CS2ST_CALL is a function-like macro, and only
 * parentheses protect a comma in a macro argument. A braced initializer list
 * in the body would be split into extra macro arguments and fail to compile.
 */
struct ProbeTypeExpectation
{
	const char* name;
	ConVarRefAbstract* ref;
	EConVarType type;
};

const ProbeTypeExpectation kProbeTypes[] = {
	{"cs2sdktest_int",     &g_probe_int,     TranslateConVarType<int32>()},
	{"cs2sdktest_bool",    &g_probe_bool,    TranslateConVarType<bool>()},
	{"cs2sdktest_float",   &g_probe_float,   TranslateConVarType<float32>()},
	{"cs2sdktest_int64",   &g_probe_int64,   TranslateConVarType<int64>()},
	{"cs2sdktest_color",   &g_probe_color,   TranslateConVarType<Color>()},
	{"cs2sdktest_vec3",    &g_probe_vec3,    TranslateConVarType<Vector>()},
	{"cs2sdktest_peruser", &g_probe_peruser, TranslateConVarType<int32>()},
};

constexpr std::size_t kProbeTypeCount =
	sizeof(kProbeTypes) / sizeof(kProbeTypes[0]);

/**
 * Arguments for the CCommand(argc, argv) constructor. Non-const element type
 * because that constructor takes const char**, and at file scope for the same
 * macro-argument reason as the table above.
 */
const char* kProbeArgv[] = {"cs2sdktest_cmd", "alpha", "with space"};

/** Engine convar types the Clamp row scans for; at file scope for the same macro reason. */
const EConVarType kClampKinds[] = {EConVarType_Int32, EConVarType_Float32};
constexpr std::size_t kClampKindCount = sizeof(kClampKinds) / sizeof(kClampKinds[0]);

/**
 * Gives ConVarRefAbstracts protected members to offsetof.
 *
 * m_ConVarAccessIndex, m_ConVarRegisteredIndex and m_ConVarData are protected,
 * so no free function can name them - but a derived class can, and a derived
 * class that adds nothing has the same layout. The alternative would be writing
 * the three offsets down as numbers, which is what this project does not do.
 *
 * Never instantiated; offsetof only needs the type to be complete.
 */
struct ConVarRefAbstractProbe : public ConVarRefAbstract
{
	static std::size_t AccessIndexOffset()
	{
		return offsetof(ConVarRefAbstractProbe, m_ConVarAccessIndex);
	}
	static std::size_t RegisteredIndexOffset()
	{
		return offsetof(ConVarRefAbstractProbe, m_ConVarRegisteredIndex);
	}
	static std::size_t DataOffset()
	{
		return offsetof(ConVarRefAbstractProbe, m_ConVarData);
	}
};

/**
 * The five pointers ConVarData declares first, in declaration order.
 *
 * Used by the value-driven offset locator: every one of them has a getter, so
 * their offsets can be found by searching the block for the value rather than
 * by asking offsetof - which cannot be asked, since all of ConVarData is
 * private.
 */
const char* const kConVarDataPointerFields[] = {
	"m_pszName",
	"m_defaultValue",
	"m_minValue",
	"m_maxValue",
	"m_pszHelpString",
};

constexpr std::size_t kConVarDataPointerCount =
	sizeof(kConVarDataPointerFields) / sizeof(kConVarDataPointerFields[0]);

/** How many private fields ConVarData declares that can be captured. */
constexpr std::size_t kConVarDataFieldCount = 15;

/**
 * Writes the captured offsets of ConVarData's private fields into `out`, in
 * declaration order, and returns how many were written.
 *
 * A function rather than a table at the call site for two reasons: the offsets
 * need a live instance, so they cannot be a file-scope constant; and a braced
 * initializer list inside a CS2ST_CALL body would have its commas taken as
 * extra macro arguments.
 */
std::size_t CollectConVarDataOffsets(const ConVarData* data, std::size_t* out,
                                     std::size_t capacity)
{
	if (data == nullptr || out == nullptr || capacity < kConVarDataFieldCount)
	{
		return 0;
	}

	std::size_t n = 0;
	out[n++] = PrivateFieldOffset<ConVarDataName>(data);
	out[n++] = PrivateFieldOffset<ConVarDataDefaultValue>(data);
	out[n++] = PrivateFieldOffset<ConVarDataMinValue>(data);
	out[n++] = PrivateFieldOffset<ConVarDataMaxValue>(data);
	out[n++] = PrivateFieldOffset<ConVarDataHelpString>(data);
	out[n++] = PrivateFieldOffset<ConVarDataVarType>(data);
	out[n++] = PrivateFieldOffset<ConVarDataVersion>(data);
	out[n++] = PrivateFieldOffset<ConVarDataTimesChanged>(data);
	out[n++] = PrivateFieldOffset<ConVarDataFlags>(data);
	out[n++] = PrivateFieldOffset<ConVarDataCallbackIndex>(data);
	out[n++] = PrivateFieldOffset<ConVarDataFilterCBIndex>(data);
	out[n++] = PrivateFieldOffset<ConVarDataComplCBIndex>(data);
	out[n++] = PrivateFieldOffset<ConVarDataGameInfoFlags>(data);
	out[n++] = PrivateFieldOffset<ConVarDataUserInfoIndex>(data);
	out[n++] = PrivateFieldOffset<ConVarDataEnumInfoFn>(data);
	return n;
}

const char* ConVarTypeName(EConVarType type)
{
	switch (type)
	{
		case EConVarType_Invalid:  return "invalid";
		case EConVarType_Bool:     return "bool";
		case EConVarType_Int16:    return "int16";
		case EConVarType_UInt16:   return "uint16";
		case EConVarType_Int32:    return "int32";
		case EConVarType_UInt32:   return "uint32";
		case EConVarType_Int64:    return "int64";
		case EConVarType_UInt64:   return "uint64";
		case EConVarType_Float32:  return "float32";
		case EConVarType_Float64:  return "float64";
		case EConVarType_String:   return "string";
		case EConVarType_Color:    return "color";
		case EConVarType_Vector2:  return "vector2";
		case EConVarType_Vector3:  return "vector3";
		case EConVarType_Vector4:  return "vector4";
		case EConVarType_Qangle:   return "qangle";
		case EConVarType_VectorWS: return "vectorws";
		case EConVarType_MAX:      return "<MAX>";
		default:                   return "<out of range>";
	}
}

const char* TierName(AppSystemTier_t tier)
{
	switch (tier)
	{
		case APP_SYSTEM_TIER0:      return "TIER0";
		case APP_SYSTEM_TIER1:      return "TIER1";
		case APP_SYSTEM_TIER2:      return "TIER2";
		case APP_SYSTEM_TIER3:      return "TIER3";
		case APP_SYSTEM_TIER4:      return "TIER4";
		case APP_SYSTEM_TIER_OTHER: return "TIER_OTHER";
		default:                    return "<out of range>";
	}
}

/** True when `name` is one of ours. */
bool IsProbeCvarName(const char* name)
{
	if (name == nullptr)
	{
		return false;
	}
	for (std::size_t i = 0; i < kProbeCvarCount; ++i)
	{
		if (std::strcmp(name, kProbeCvarNames[i]) == 0)
		{
			return true;
		}
	}
	return false;
}

/** True for the scratch convars, which are ours but not part of the probe set. */
bool IsScratchCvarName(const char* name)
{
	return name != nullptr && std::strncmp(name, "cstmp_", 6) == 0;
}

/**
 * Walks the registered convars for the first one of `type` that is not one of
 * ours, so a type this plugin cannot safely register is still covered by
 * reading a real engine cvar.
 */
bool FindEngineCvarOfType(ICvar* cvar, EConVarType type, ConVarRef* out)
{
	int walked = 0;
	for (ConVarRef iter = cvar->FindFirstConVar();
	     iter.IsValidRef() && walked < kListWalkCap;
	     iter = cvar->FindNextConVar(iter), ++walked)
	{
		ConVarRefAbstract ref(iter);
		if (ref.GetType() != type || !ref.IsConVarDataAvailable())
		{
			continue;
		}
		if (IsProbeCvarName(ref.GetName()) || IsScratchCvarName(ref.GetName()))
		{
			continue;
		}
		*out = iter;
		return true;
	}
	return false;
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool ConVarSuite::Acquire(TestContext& context)
{
	// tier0, not engine2: CS2 keeps the cvar system in the platform layer. That
	// is worth pinning rather than searching for, because "tier0 stopped
	// answering VEngineCvar007" and "some other module answers it now" are
	// different findings and the second one is only visible if the first is
	// asked specifically.
	cvar_ = static_cast<ICvar*>(context.QueryInterface("tier0", CVAR_INTERFACE_VERSION));
	cvar_module_ = "tier0";

	if (cvar_ == nullptr)
	{
		std::string found_in;
		cvar_ = static_cast<ICvar*>(
			context.QueryInterfaceAnywhere(CVAR_INTERFACE_VERSION, &found_in));
		cvar_module_ = found_in;
	}

	if (cvar_ == nullptr)
	{
		const Module& tier0 = context.GetModule("tier0");
		if (!tier0.IsValid())
		{
			context.UnavailableCall("ICvar", "tier0 is not loaded");
		}
		else
		{
			context.UnavailableCall("ICvar",
				"no loaded module answers '%s'", CVAR_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("interface at %p (%s, from %s)", static_cast<void*>(cvar_),
		CVAR_INTERFACE_VERSION, cvar_module_.c_str());
	return true;
}

void ConVarSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseOwnRegistrations(context);
	ExerciseConVarLookup(context);
	ExerciseConVarRefApi(context);
	ExerciseConVarDataApi(context);
	ExerciseTypedRefs(context);
	ExerciseValueWrites(context);
	ExerciseChangeCallbacks(context);
	ExerciseCreationListeners(context);
	ExerciseDynamicRegistration(context);
	ExerciseConCommandLookup(context);
	ExerciseCallbackInfoShapes(context);
	ExerciseCommandTokenizer(context);
	ExerciseCommandDispatch(context);
	ExerciseCompletion(context);
	ExerciseSnapshots(context);
	ExerciseUserInfoCvars(context);
	ExerciseCharacterSet(context);
	ExerciseScratchCalls(context);
	InspectCvarObjectLayout(context);
	InspectCharacterSetLayout(context);
	InspectConVarRefLayout(context);
	InspectConCommandDataLayout(context);
	InspectConVarDataLayout(context);
	NoteTypeTraitsTable(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = cvar_->GetTier();
		// Compared as int: the enum's underlying type may be unsigned, which
		// would make a "< 0" check on the enum itself vacuously false.
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", TierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier",
				tier_value);
		}
	});

	// Recorded, not asserted. The cvar system is a singleton in the ordinary
	// sense, but nothing in the SDK says tier0's CCvar reports itself as one
	// through this slot, and a FAIL that rests on a guess is worse than no
	// check - it teaches a reader to skip the column.
	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		context.Detail("-> %s", cvar_->IsSingleton() ? "true" : "false");
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = cvar_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)",
				static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = cvar_->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}

		// Capped: walking a garbage pointer until it happens to hit a null is
		// how a probe turns into a hang instead of a crash.
		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface", {
		void* self = cvar_->QueryInterface(CVAR_INTERFACE_VERSION);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(cvar_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run cvar system startup");
	context.SkipCall("IAppSystem::Init", "would re-run cvar system startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind tier0's interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin cvar system shutdown");
	context.SkipCall("IAppSystem::Shutdown",
		"would tear down every registered convar in the process");
	context.SkipCall("IAppSystem::Disconnect", "would tear down tier0's interfaces");
}

// -----------------------------------------------------------------------------
//  Our own registrations - the ground truth everything else is checked against
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseOwnRegistrations(TestContext& context)
{
	context.Note("[ground truth] %zu convars and 2 commands this plugin registered "
	             "at load, with values we chose", kProbeCvarCount);

	// Two independent routes to the interface: the module's own
	// CreateInterface, which is what Acquire used, and the pointer Metamod
	// handed Plugin::Load through GetEngineFactory. They have to be the same
	// object; if they are not, one of the two lookups is resolving to
	// something else and every result in this section is suspect.
	CS2ST_CALL(context, "ICvar <-> g_pCVar (module factory vs Metamod)", {
		context.Detail("module factory -> %p, g_pCVar -> %p",
			static_cast<void*>(cvar_), static_cast<void*>(g_pCVar));
		if (g_pCVar == nullptr)
		{
			context.Fail("g_pCVar is null, but tier1 needs it for every "
			             "ConVarRef and ConCommandRef method");
		}
		else if (g_pCVar != cvar_)
		{
			context.Fail("the two lookups disagree; tier1's helpers go through "
			             "g_pCVar while this suite calls the other one");
		}
	});

	// One line per probe convar. What is being checked is not that the engine
	// has *some* answer but that it has *our* answer: the name we passed, the
	// type our template argument translates to, the help text we wrote.
	for (std::size_t i = 0; i < kProbeCvarCount; ++i)
	{
		const char* name = kProbeCvarNames[i];

		CS2ST_CALL(context, std::string("registered convar: ") + name, {
			const ConVarRef found = cvar_->FindConVar(name, true);
			if (!found.IsValidRef())
			{
				context.Fail("this plugin registered it at load; the cvar system "
				             "does not know it");
				return;
			}

			ConVarRefAbstract ref(found);
			const char* engine_name = ref.GetName();

			context.Detail("access=%u registered=%d type=%s(%d) flags=0x%llx help=%s",
				static_cast<unsigned>(found.GetAccessIndex()),
				found.GetRegisteredIndex(),
				ConVarTypeName(ref.GetType()), static_cast<int>(ref.GetType()),
				static_cast<unsigned long long>(ref.GetFlags()),
				ref.HasHelpText() ? "yes" : "no");

			if (engine_name == nullptr || std::strcmp(engine_name, name) != 0)
			{
				context.Fail("name reads back as \"%s\"", engine_name != nullptr
					? engine_name : "(null)");
			}
			if (!ref.IsConVarDataValid())
			{
				context.Fail("ConVarData is the invalid-cvar placeholder");
			}
			if (!ref.IsConVarDataAvailable())
			{
				context.Fail("registered, but the data still carries FCVAR_REFERENCE");
			}
			if (!ref.HasHelpText())
			{
				context.Fail("a help string was supplied at registration");
			}
		});
	}

	// The types have to survive the trip too. Our template argument decides
	// what TranslateConVarType returns, the engine stores it in the ConVarData
	// it allocated, and GetType reads it back out - so a mismatch means either
	// EConVarType was renumbered or ConVarData's type field moved.
	CS2ST_CALL(context, "declared type vs ICvar-stored type", {
		int mismatches = 0;
		for (std::size_t i = 0; i < kProbeTypeCount; ++i)
		{
			const ProbeTypeExpectation& expected = kProbeTypes[i];
			const EConVarType actual = expected.ref->GetType();
			if (actual != expected.type)
			{
				++mismatches;
				context.Note("  %s: declared %s(%d), engine says %s(%d)",
					expected.name,
					ConVarTypeName(expected.type), static_cast<int>(expected.type),
					ConVarTypeName(actual), static_cast<int>(actual));
			}
		}
		context.Detail("%zu of %zu declared types match",
			kProbeTypeCount - static_cast<std::size_t>(mismatches), kProbeTypeCount);
		if (mismatches > 0)
		{
			context.Fail("%d convar(s) came back as a type we did not declare", mismatches);
		}
	});
}

// -----------------------------------------------------------------------------
//  Finding and iterating convars
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseConVarLookup(TestContext& context)
{
	context.Note("[ICvar] convar lookup and iteration");

	CS2ST_CALL(context, "ICvar::FindConVar (one of ours)", {
		const ConVarRef found = cvar_->FindConVar("cs2sdktest_int", false);
		context.Detail("-> access=%u registered=%d valid=%s",
			static_cast<unsigned>(found.GetAccessIndex()),
			found.GetRegisteredIndex(), found.IsValidRef() ? "yes" : "no");
		if (!found.IsValidRef())
		{
			context.Fail("cs2sdktest_int is registered and carries no defensive flag");
		}
		// The ConVarRef our own CConVar object holds came out of the
		// registration call; the one above came out of a by-name lookup. Two
		// routes to the same handle, so the indices have to agree.
		else if (found.GetAccessIndex() != g_probe_int.GetAccessIndex())
		{
			context.Fail("by-name lookup gives access index %u, our registration "
			             "produced %u",
				static_cast<unsigned>(found.GetAccessIndex()),
				static_cast<unsigned>(g_probe_int.GetAccessIndex()));
		}
	});

	CS2ST_CALL(context, "ICvar::FindConVar (nonexistent name)", {
		const ConVarRef found = cvar_->FindConVar("cs2sdktest_no_such_convar", true);
		context.Detail("-> valid=%s", found.IsValidRef() ? "yes" : "no");
		if (found.IsValidRef())
		{
			context.Fail("a name nothing registered came back as a valid ref");
		}
	});

	// The allow_defensive parameter, tested the only way it can be: against a
	// convar we registered with FCVAR_DEFENSIVE for this purpose.
	//
	// Only one direction is an SDK guarantee. The header says "Allows finding
	// convars with FCVAR_DEFENSIVE flag", which constrains what true does and
	// says nothing about false. The first version of this asserted the
	// contrapositive - false must hide it - and that is an inference, not a
	// contract. Measured: false finds it too. So this half records, and the
	// true half below still judges.
	CS2ST_CALL(context, "ICvar::FindConVar (defensive, allow_defensive=false)", {
		const ConVarRef found = cvar_->FindConVar("cs2sdktest_defensive", false);
		context.Detail("-> valid=%s%s", found.IsValidRef() ? "yes" : "no",
			found.IsValidRef()
				? "  (found without asking; the flag gates nothing on this build)"
				: "  (hidden unless asked for)");
	});

	CS2ST_CALL(context, "ICvar::FindConVar (defensive, allow_defensive=true)", {
		const ConVarRef found = cvar_->FindConVar("cs2sdktest_defensive", true);
		context.Detail("-> valid=%s", found.IsValidRef() ? "yes" : "no");
		if (!found.IsValidRef())
		{
			context.Fail("allow_defensive=true should find it");
		}
	});

	CS2ST_CALL(context, "ICvar::FindFirstConVar", {
		const ConVarRef first = cvar_->FindFirstConVar();
		context.Detail("-> access=%u valid=%s",
			static_cast<unsigned>(first.GetAccessIndex()),
			first.IsValidRef() ? "yes" : "no");
		if (!first.IsValidRef())
		{
			context.Fail("this plugin alone registered %zu convars, so the list "
			             "cannot be empty", kProbeCvarCount);
		}
	});

	// The iteration is its own oracle: whatever else is registered, all eight
	// of ours have to turn up in it, because we know they exist.
	CS2ST_CALL(context, "ICvar::FindNextConVar (walk the whole list)", {
		int walked = 0;
		int available = 0;
		int hidden = 0;
		std::size_t probes_seen = 0;
		int type_counts[EConVarType_MAX + 1] = {};

		for (ConVarRef iter = cvar_->FindFirstConVar();
		     iter.IsValidRef() && walked < kListWalkCap;
		     iter = cvar_->FindNextConVar(iter), ++walked)
		{
			ConVarRefAbstract ref(iter);

			const EConVarType type = ref.GetType();
			if (type >= EConVarType_Invalid && type < EConVarType_MAX)
			{
				// Invalid is -1, so shift by one to index the tally.
				++type_counts[static_cast<int>(type) + 1];
			}

			if (ref.IsConVarDataAvailable())
			{
				++available;
			}
			if (ref.ShouldBeHidden())
			{
				++hidden;
			}
			if (IsProbeCvarName(ref.GetName()))
			{
				++probes_seen;
			}
		}

		context.Detail("-> %d convars (%d available, %d hidden), %zu/%zu of ours seen%s",
			walked, available, hidden, probes_seen, kProbeCvarCount,
			walked >= kListWalkCap ? " (capped)" : "");

		context.Note("  by type: bool=%d int32=%d uint32=%d int64=%d uint64=%d "
		             "float32=%d float64=%d string=%d color=%d vec3=%d",
			type_counts[EConVarType_Bool + 1], type_counts[EConVarType_Int32 + 1],
			type_counts[EConVarType_UInt32 + 1], type_counts[EConVarType_Int64 + 1],
			type_counts[EConVarType_UInt64 + 1], type_counts[EConVarType_Float32 + 1],
			type_counts[EConVarType_Float64 + 1], type_counts[EConVarType_String + 1],
			type_counts[EConVarType_Color + 1], type_counts[EConVarType_Vector3 + 1]);

		if (walked >= kListWalkCap)
		{
			context.Fail("the walk hit its %d cap; either the game really has that "
			             "many convars or FindNextConVar is cycling", kListWalkCap);
		}
		else if (probes_seen != kProbeCvarCount)
		{
			// Defensive cvars may legitimately be filtered out of a plain walk,
			// which is the one benign explanation for a short count.
			context.Suspect("the walk saw %zu of our %zu convars; defensive ones "
			                "may be filtered from iteration", probes_seen, kProbeCvarCount);
		}
	});

	CS2ST_CALL(context, "ICvar::GetConVarData (vs ConVarRefAbstract::GetConVarData)", {
		const ConVarRef found = cvar_->FindConVar("cs2sdktest_int", false);
		ConVarData* from_icvar = cvar_->GetConVarData(found);
		ConVarData* from_ref = g_probe_int.GetConVarData();

		context.Detail("ICvar -> %p, our CConVar holds %p",
			static_cast<void*>(from_icvar), static_cast<void*>(from_ref));

		if (from_icvar == nullptr)
		{
			context.Fail("a registered convar has data; the SDK returns null only "
			             "when the ref is unknown");
		}
		else if (from_icvar != from_ref)
		{
			context.Fail("the interface and our own registration point at different "
			             "ConVarData objects for the same convar");
		}
	});

	CS2ST_CALL(context, "ICvar::GetConVarData (invalid ref)", {
		ConVarData* data = cvar_->GetConVarData(ConVarRef());
		context.Detail("-> %p (the SDK says null when not found)",
			static_cast<void*>(data));
	});
}

// -----------------------------------------------------------------------------
//  ConVarRef / ConVarRefAbstract
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseConVarRefApi(TestContext& context)
{
	context.Note("[ConVarRef / ConVarRefAbstract] readers, on cs2sdktest_int "
	             "(declared 7, min 0, max 100)");

	CS2ST_CALL(context, "ConVarRef handle round-trip", {
		// The handle is returned by value from engine functions, so its two
		// fields land in registers rather than in memory. Constructing one
		// with known halves and reading them back is what pins that layout.
		const ConVarRef built(static_cast<uint16>(0x1234), 0x5678);
		context.Detail("access=%u registered=%d valid=%s",
			static_cast<unsigned>(built.GetAccessIndex()),
			built.GetRegisteredIndex(), built.IsValidRef() ? "yes" : "no");

		if (built.GetAccessIndex() != 0x1234 || built.GetRegisteredIndex() != 0x5678)
		{
			context.Fail("a ConVarRef does not read back the halves it was built from");
		}

		ConVarRef invalidated(built);
		invalidated.InvalidateRef();
		if (invalidated.IsValidRef())
		{
			context.Fail("InvalidateRef left the ref valid");
		}
	});

	CS2ST_CALL(context, "ConVarRef (by-name constructor)", {
		// A separate code path from ICvar::FindConVar even though it ends up
		// there: tier1 reaches the interface through its own g_pCVar global, so
		// this is the check that a plugin built against the SDK gets the same
		// answer as one that holds the interface pointer itself.
		const ConVarRef by_name("cs2sdktest_int");
		const ConVarRef by_interface = cvar_->FindConVar("cs2sdktest_int", false);

		context.Detail("tier1 -> access=%u, interface -> access=%u",
			static_cast<unsigned>(by_name.GetAccessIndex()),
			static_cast<unsigned>(by_interface.GetAccessIndex()));

		if (!by_name.IsValidRef())
		{
			context.Fail("the by-name constructor did not resolve a registered "
			             "convar; tier1's g_pCVar may be unset");
		}
		else if (by_name.GetAccessIndex() != by_interface.GetAccessIndex())
		{
			context.Fail("tier1 and the interface disagree about the handle");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract (by-name and copy constructors)", {
		ConVarRefAbstract by_name("cs2sdktest_int");
		ConVarRefAbstract copied(by_name);

		context.Detail("by name: \"%s\" = %d | copied: \"%s\" = %d",
			by_name.GetName(), by_name.GetInt(),
			copied.GetName(), copied.GetInt());

		if (!by_name.IsConVarDataAvailable())
		{
			context.Fail("the by-name constructor should resolve a registered convar");
		}
		// CopyRef copies all three members, so a copy has to be
		// indistinguishable from its source.
		if (copied.GetConVarData() != by_name.GetConVarData()
			|| copied.GetAccessIndex() != by_name.GetAccessIndex()
			|| copied.GetRegisteredIndex() != by_name.GetRegisteredIndex())
		{
			context.Fail("the copy constructor did not carry all three members over");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::GetName / GetHelpText / HasHelpText", {
		context.Detail("name=\"%s\" help=\"%.60s\"",
			g_probe_int.GetName(),
			g_probe_int.HasHelpText() ? g_probe_int.GetHelpText() : "(none)");
	});

	CS2ST_CALL(context, "ConVarRefAbstract::HasDefault / HasMin / HasMax", {
		context.Detail("default=%s min=%s max=%s",
			g_probe_int.HasDefault() ? "yes" : "no",
			g_probe_int.HasMin() ? "yes" : "no",
			g_probe_int.HasMax() ? "yes" : "no");

		// All three were supplied at registration, so all three are ours to
		// assert on: the engine had to allocate storage for the bounds and
		// record that they exist.
		if (!g_probe_int.HasDefault())
		{
			context.Fail("registered with default 7");
		}
		if (!g_probe_int.HasMin())
		{
			context.Fail("registered with min 0");
		}
		if (!g_probe_int.HasMax())
		{
			context.Fail("registered with max 100");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::GetDefaultAsString / GetMinAsString / GetMaxAsString", {
		CBufferStringN<64> default_value(false);
		CBufferStringN<64> min_value(false);
		CBufferStringN<64> max_value(false);

		g_probe_int.GetDefaultAsString(default_value);
		g_probe_int.GetMinAsString(min_value);
		g_probe_int.GetMaxAsString(max_value);

		context.Detail("default=\"%s\" min=\"%s\" max=\"%s\"",
			default_value.Get(), min_value.Get(), max_value.Get());

		if (std::strcmp(default_value.Get(), "7") != 0)
		{
			context.Fail("registered with default 7");
		}
		if (std::strcmp(min_value.Get(), "0") != 0)
		{
			context.Fail("registered with min 0");
		}
		if (std::strcmp(max_value.Get(), "100") != 0)
		{
			context.Fail("registered with max 100");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::GetMinAsString (convar with no min)", {
		// The SDK's MinValueToString inserts an empty string when there is no
		// min, so this is a declaration-level guarantee, not an observation.
		CBufferStringN<64> min_value(false);
		g_probe_bool.GetMinAsString(min_value);
		context.Detail("cs2sdktest_bool min -> \"%s\"", min_value.Get());
		if (min_value.Get()[0] != '\0')
		{
			context.Fail("a convar registered without a min should stringify to empty");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::GetValueAsString / GetInt / GetBool / GetFloat / GetString", {
		CBufferStringN<64> as_string(false);
		g_probe_int.GetValueAsString(as_string);
		const CUtlString via_getstring = g_probe_int.GetString();

		context.Detail("value=\"%s\" GetInt=%d GetBool=%s GetFloat=%.3f GetString=\"%s\"",
			as_string.Get(), g_probe_int.GetInt(),
			g_probe_int.GetBool() ? "true" : "false",
			static_cast<double>(g_probe_int.GetFloat()),
			via_getstring.Get());

		// GetString goes through GetAs<CUtlString>, which goes through
		// GetValueAsString - so these two must agree by construction, and a
		// disagreement means one of the two paths is reading a different field.
		if (std::strcmp(as_string.Get(), via_getstring.Get()) != 0)
		{
			context.Fail("GetValueAsString and GetString disagree");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::GetAs (typed) vs GetInt", {
		const int32 typed = g_probe_int.GetAs<int32>();
		const int untyped = g_probe_int.GetInt();
		context.Detail("GetAs<int32>=%d GetInt=%d", typed, untyped);
		if (typed != untyped)
		{
			context.Fail("the typed and generic readers disagree on an int32 convar");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::GetAs (cross-type, the primitive "
	                    "conversion path)", {
		// Asking an int32 convar for a float goes through
		// ConvertFromPrimitiveTo rather than a straight read, which is the
		// mirror of the SetInt-on-a-float-convar check further down.
		const int32 exact = g_probe_int.Get();
		const float as_float = g_probe_int.GetAs<float>();
		const int64 as_int64 = g_probe_int.GetAs<int64>();

		context.Detail("int32 %d read as float %.3f and as int64 %lld",
			exact, static_cast<double>(as_float),
			static_cast<long long>(as_int64));

		if (std::fabs(static_cast<double>(as_float) - static_cast<double>(exact)) > 1e-4)
		{
			context.Fail("the int32-to-float conversion produced %.3f",
				static_cast<double>(as_float));
		}
		if (as_int64 != exact)
		{
			context.Fail("the int32-to-int64 conversion produced %lld",
				static_cast<long long>(as_int64));
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::IsValuePrimitive", {
		context.Detail("int32=%s color=%s vec3=%s",
			g_probe_int.IsValuePrimitive() ? "yes" : "no",
			g_probe_color.IsValuePrimitive() ? "yes" : "no",
			g_probe_vec3.IsValuePrimitive() ? "yes" : "no");

		// CTypePOD decides this, and it is our own declaration talking, so the
		// check is that the engine handed back the type we registered rather
		// than anything about the trait itself.
		if (!g_probe_int.IsValuePrimitive())
		{
			context.Fail("int32 is a primitive convar type");
		}
		if (g_probe_color.IsValuePrimitive())
		{
			context.Fail("Color is not a primitive convar type");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::IsSetToDefault / IsAllSetToDefault", {
		context.Detail("int32: set-to-default=%s all-slots=%s",
			g_probe_int.IsSetToDefault() ? "yes" : "no",
			g_probe_int.IsAllSetToDefault() ? "yes" : "no");
	});

	CS2ST_CALL(context, "ConVarRefAbstract::IsFlagSet / GetFlags", {
		const uint64 flags = g_probe_int.GetFlags();
		context.Detail("flags=0x%llx gamedll=%s release=%s defensive=%s",
			static_cast<unsigned long long>(flags),
			g_probe_int.IsFlagSet(FCVAR_GAMEDLL) ? "yes" : "no",
			g_probe_int.IsFlagSet(FCVAR_RELEASE) ? "yes" : "no",
			g_probe_int.IsFlagSet(FCVAR_DEFENSIVE) ? "yes" : "no");

		// We passed these at registration and Metamod's META_CONVAR_REGISTER
		// passes them again as the additional flags, so they cannot be absent
		// unless the flag word moved.
		if (!g_probe_int.IsFlagSet(FCVAR_GAMEDLL))
		{
			context.Fail("registered with FCVAR_GAMEDLL");
		}
		if (g_probe_int.IsFlagSet(FCVAR_DEFENSIVE))
		{
			context.Fail("this one was not registered as defensive");
		}
	});

	// The two flags this SDK revision named, on the convars registered for
	// exactly this purpose. Round-trip only: the behaviors the flags gate
	// (snapshot skip, gameinfo override block) touch every convar in the
	// process, but the flag word itself is this plugin's to verify.
	CS2ST_CALL(context, "ConVarRefAbstract::IsFlagSet (FCVAR_SNAPSHOT_IGNORED, "
		"FCVAR_GAMEINFO_CANNOT_OVERRIDE)", {
		context.Detail("snapshot_ignored=%s gameinfo_cannot_override=%s",
			g_probe_snapshot_ignored.IsFlagSet(FCVAR_SNAPSHOT_IGNORED) ? "yes" : "no",
			g_probe_gameinfo_locked.IsFlagSet(FCVAR_GAMEINFO_CANNOT_OVERRIDE)
				? "yes" : "no");

		if (!g_probe_snapshot_ignored.IsFlagSet(FCVAR_SNAPSHOT_IGNORED))
		{
			context.Fail("registered with FCVAR_SNAPSHOT_IGNORED; the flag word moved");
		}
		if (!g_probe_gameinfo_locked.IsFlagSet(FCVAR_GAMEINFO_CANNOT_OVERRIDE))
		{
			context.Fail("registered with FCVAR_GAMEINFO_CANNOT_OVERRIDE; "
			             "the flag word moved");
		}
	});

	// AddFlags and RemoveFlags on a convar this plugin owns, round-tripped so
	// the sweep leaves no trace. FCVAR_MENUBAR_ITEM is the flag chosen on
	// purpose: it means something only to a client menu, so a server that
	// briefly sees it set on cs2sdktest_int does nothing differently.
	CS2ST_CALL(context, "ConVarRefAbstract::AddFlags / RemoveFlags (round-trip)", {
		const bool before = g_probe_int.IsFlagSet(FCVAR_MENUBAR_ITEM);
		g_probe_int.AddFlags(FCVAR_MENUBAR_ITEM);
		const bool after_add = g_probe_int.IsFlagSet(FCVAR_MENUBAR_ITEM);
		g_probe_int.RemoveFlags(FCVAR_MENUBAR_ITEM);
		const bool after_remove = g_probe_int.IsFlagSet(FCVAR_MENUBAR_ITEM);

		context.Detail("before=%d after add=%d after remove=%d",
			before ? 1 : 0, after_add ? 1 : 0, after_remove ? 1 : 0);

		if (!after_add)
		{
			context.Fail("AddFlags did not set the flag");
		}
		if (after_remove)
		{
			context.Fail("RemoveFlags did not clear the flag");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::ShouldBeHidden", {
		context.Detail("int32=%s defensive=%s",
			g_probe_int.ShouldBeHidden() ? "yes" : "no",
			g_probe_defensive.ShouldBeHidden() ? "yes" : "no");
	});

	CS2ST_CALL(context, "ConVarRefAbstract::TypeTraits", {
		const CVarTypeTraits* traits = g_probe_int.TypeTraits();
		if (traits == nullptr)
		{
			context.Fail("TypeTraits is never null for a valid type");
			return;
		}
		context.Detail("-> %s, %d bytes, primitive=%s, global default \"%s\"",
			traits->m_TypeName, traits->m_ByteSize,
			traits->m_IsPrimitive ? "yes" : "no",
			traits->m_DefaultValue != nullptr ? traits->m_DefaultValue : "(null)");
	});

	CS2ST_CALL(context, "ConVarRefAbstract::IsConVarDataValid / IsConVarDataAvailable", {
		// An unbound ref is the interesting case, and the two predicates do not
		// answer it the same way.
		//
		// ConVarRefAbstract::Init only falls back to the per-type placeholder
		// when g_pCVar->GetConVarData(ref) returns null - and for an invalid
		// handle the engine returns a non-null ConVarData carrying
		// FCVAR_REFERENCE instead. So IsConVarDataValid, which is defined as
		// "not the placeholder", says yes, while IsConVarDataAvailable, defined
		// as "the cvar was registered", correctly says no.
		//
		// The first version of this asserted the placeholder path and failed.
		// The assertion now sits on the predicate whose documented meaning
		// actually holds; IsConVarDataValid saying yes is its own definition
		// ("not the placeholder") applied to what the engine hands over.
		ConVarRefAbstract unbound(ConVarRef(), EConVarType_Int32);
		const bool unbound_valid = unbound.IsConVarDataValid();
		const bool unbound_available = unbound.IsConVarDataAvailable();

		context.Detail("ours: valid=%s available=%s | unbound: valid=%s available=%s",
			g_probe_int.IsConVarDataValid() ? "yes" : "no",
			g_probe_int.IsConVarDataAvailable() ? "yes" : "no",
			unbound_valid ? "yes" : "no",
			unbound_available ? "yes" : "no");

		if (unbound_available)
		{
			context.Fail("an unbound ref reports its convar data as available, so "
			             "nothing distinguishes it from a registered convar");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract (by name, string type on an engine convar)", {
		// EConVarType_String is the one type this plugin will not register (it
		// would put a heap buffer from this module into engine-owned storage),
		// so it is covered read-only against whatever the game has.
		ConVarRef string_cvar;
		if (!FindEngineCvarOfType(cvar_, EConVarType_String, &string_cvar))
		{
			context.Detail("no engine string convar found in the list");
			return;
		}

		ConVarRefAbstract ref(string_cvar);
		CBufferStringN<256> value(false);
		ref.GetValueAsString(value);

		context.Detail("\"%s\" = \"%.120s\" (type=%s, default=%s)",
			ref.GetName(), value.Get(), ConVarTypeName(ref.GetType()),
			ref.IsSetToDefault() ? "yes" : "no");

		if (ref.GetType() != EConVarType_String)
		{
			context.Fail("the scan selected on type and then the type changed");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::Clamp", {
		// On our own convar, and with the value already inside 0..100, so this
		// checks that Clamp reaches the engine-allocated bounds without
		// disturbing anything.
		const int32 before = g_probe_int.Get();
		g_probe_int.Clamp();
		const int32 after = g_probe_int.Get();
		context.Detail("%d -> %d (declared bounds 0..100)", before, after);
		if (before != after)
		{
			context.Fail("clamping a value already in range changed it");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::Clamp (engine convars)", {
		// Clamping a convar the game owns can move its value if the game left
		// it outside its own bounds. That is a state change, but not one any
		// later row reads, so what is checked is the part that must hold
		// either way: a second Clamp finds nothing left to move.
		int clamped = 0;
		for (std::size_t i = 0; i < kClampKindCount; ++i)
		{
			const EConVarType kind = kClampKinds[i];
			ConVarRef found;
			if (!FindEngineCvarOfType(cvar_, kind, &found))
			{
				continue;
			}

			ConVarRefAbstract ref(found);
			CBufferStringN<256> before_value(false);
			CBufferStringN<256> first_value(false);
			CBufferStringN<256> second_value(false);

			ref.GetValueAsString(before_value);
			char before[128];
			std::snprintf(before, sizeof(before), "%.127s", before_value.Get());

			ref.Clamp();
			ref.GetValueAsString(first_value);
			char first[128];
			std::snprintf(first, sizeof(first), "%.127s", first_value.Get());

			ref.Clamp();
			ref.GetValueAsString(second_value);

			context.Detail("%s \"%s\": \"%s\" -> \"%s\"", ConVarTypeName(kind),
				ref.GetName(), before, first);
			++clamped;

			if (std::strcmp(first, second_value.Get()) != 0)
			{
				context.Fail("a second Clamp moved \"%s\" again, so the first one "
				             "did not reach a fixed point", ref.GetName());
			}
		}

		if (clamped == 0)
		{
			context.Detail("no engine int32 or float32 convar found in the list");
		}
	});
}

// -----------------------------------------------------------------------------
//  ConVarData
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseConVarDataApi(TestContext& context)
{
	context.Note("[ConVarData] the engine-allocated block behind a convar");

	ConVarData* data = g_probe_int.GetConVarData();
	if (data == nullptr)
	{
		context.SkipCall("ConVarData::<all>", "cs2sdktest_int has no ConVarData");
		return;
	}

	CS2ST_CALL(context, "ConVarData::GetName / GetHelpText / HasHelpText / GetType", {
		context.Detail("name=\"%s\" type=%s(%d) help=%s",
			data->GetName(), ConVarTypeName(data->GetType()),
			static_cast<int>(data->GetType()),
			data->HasHelpText() ? "yes" : "no");

		if (data->GetName() == nullptr
			|| std::strcmp(data->GetName(), "cs2sdktest_int") != 0)
		{
			context.Fail("the name field does not read back what we registered");
		}
	});

	CS2ST_CALL(context, "ConVarData::GetDataTypeName / GetDataByteSize / IsPrimitiveType", {
		context.Detail("%s, %d bytes, primitive=%s",
			data->GetDataTypeName(), data->GetDataByteSize(),
			data->IsPrimitiveType() ? "yes" : "no");
		if (data->GetDataByteSize() <= 0)
		{
			context.Fail("a byte size of %d cannot be right for any convar type",
				data->GetDataByteSize());
		}
	});

	CS2ST_CALL(context, "ConVarData::GetFlags / IsFlagSet", {
		context.Detail("flags=0x%llx",
			static_cast<unsigned long long>(data->GetFlags()));
		// Two routes to the same word: through the ref and through the data.
		if (data->GetFlags() != g_probe_int.GetFlags())
		{
			context.Fail("ConVarData and ConVarRefAbstract disagree about the flags");
		}
		if (!data->IsFlagSet(FCVAR_GAMEDLL))
		{
			context.Fail("registered with FCVAR_GAMEDLL");
		}
	});

	CS2ST_CALL(context, "ConVarData::GetVersion / GetGameInfoFlags / GetCallbackIndex / GetUserInfoByteIndex", {
		context.Detail("version=%d gameinfo=0x%x callback=%d userinfo=%d",
			static_cast<int>(data->GetVersion()), data->GetGameInfoFlags(),
			data->GetCallbackIndex(), data->GetUserInfoByteIndex());

		// We registered a change callback, so the engine had to store an index
		// into its callback list. Zero means "none" throughout this SDK.
		if (data->GetCallbackIndex() == 0)
		{
			context.Fail("cs2sdktest_int was registered with a change callback, "
			             "so a callback index should be set");
		}
	});

	CS2ST_CALL(context, "ConVarData::HasEnumInfo", {
		// The Oct 2026 SDK rework named this slot's era: m_fnCustomData /
		// GetCustomDataFn became m_fnGetEnumInfoHandle / HasEnumInfo+GetEnumInfo,
		// a schema-enum binding only a handful of cvars (lb_debug_tiles and
		// friends) set. Ours did not, so false is the only right answer.
		context.Detail("-> %s (only a handful of cvars set one)",
			data->HasEnumInfo() ? "true" : "false");
		if (data->HasEnumInfo())
		{
			context.Fail("cs2sdktest_int was registered with no enum binding");
		}
	});

	// Two independent routes to the split-screen slot count, plus a
	// type-implied answer for the convar that does not opt in.
	CS2ST_CALL(context, "ConVarData::GetMaxSplitScreenSlots (vs ICvar)", {
		const int from_icvar = cvar_->GetMaxSplitScreenSlots();
		const int plain = data->GetMaxSplitScreenSlots();
		ConVarData* per_user = g_probe_peruser.GetConVarData();
		const int per_user_slots = (per_user != nullptr)
			? per_user->GetMaxSplitScreenSlots() : -1;

		context.Detail("ICvar=%d, plain convar=%d, FCVAR_PER_USER convar=%d",
			from_icvar, plain, per_user_slots);

		if (from_icvar < 1 || from_icvar > MAX_SPLITSCREEN_CLIENTS)
		{
			context.Fail("a slot count outside 1..%d cannot be right",
				MAX_SPLITSCREEN_CLIENTS);
		}
		if (plain != 1)
		{
			context.Fail("a convar without FCVAR_PER_USER has exactly one slot "
			             "by the SDK's own implementation");
		}
		if (per_user_slots != from_icvar)
		{
			context.Fail("an FCVAR_PER_USER convar should report the interface's "
			             "slot count");
		}
	});

	CS2ST_CALL(context, "ConVarData::HasDefaultValue / HasMinValue / HasMaxValue "
	                    "and DefaultValue / MinValue / MaxValue", {
		context.Detail("default %s@%p min %s@%p max %s@%p",
			data->HasDefaultValue() ? "yes" : "no",
			static_cast<void*>(data->DefaultValue()),
			data->HasMinValue() ? "yes" : "no",
			static_cast<void*>(data->MinValue()),
			data->HasMaxValue() ? "yes" : "no",
			static_cast<void*>(data->MaxValue()));

		if (!data->HasDefaultValue() || data->DefaultValue() == nullptr)
		{
			context.Fail("the SDK says a convar always has a default value");
		}
		if (!data->HasMinValue() || data->MinValue() == nullptr)
		{
			context.Fail("registered with min 0, so the engine allocated storage "
			             "for it");
		}
		if (!data->HasMaxValue() || data->MaxValue() == nullptr)
		{
			context.Fail("registered with max 100");
		}
	});

	CS2ST_CALL(context, "ConVarData::DefaultValue (contents vs what we registered)", {
		CVValue_t* default_value = data->DefaultValue();
		if (default_value == nullptr)
		{
			context.Fail("no default value to read");
			return;
		}
		// This is the round trip that matters most for CVValue_t: we handed the
		// engine a 7 inside a ConVarValueInfo_t, the engine copied it into
		// storage it allocated, and this reads it back out of that storage.
		context.Detail("default=%d min=%d max=%d",
			default_value->m_i32Value,
			data->MinValue() != nullptr ? data->MinValue()->m_i32Value : 0,
			data->MaxValue() != nullptr ? data->MaxValue()->m_i32Value : 0);

		if (default_value->m_i32Value != 7)
		{
			context.Fail("registered with default 7, storage holds %d",
				default_value->m_i32Value);
		}
		if (data->MinValue() != nullptr && data->MinValue()->m_i32Value != 0)
		{
			context.Fail("registered with min 0");
		}
		if (data->MaxValue() != nullptr && data->MaxValue()->m_i32Value != 100)
		{
			context.Fail("registered with max 100");
		}
	});

	CS2ST_CALL(context, "CVValue_t::InvalidValue (vs ConVarData::HasDefaultValue)", {
		// HasDefaultValue is defined as "the pointer is set and is not the
		// shared invalid sentinel", so the two halves have to agree. This is
		// also the only place the sentinel itself gets looked at, and it is
		// worth knowing where it lives: it is a static in *this* module, so a
		// default value pointing at it would mean the engine handed us back
		// our own placeholder.
		CVValue_t* sentinel = CVValue_t::InvalidValue();
		CVValue_t* actual = data->DefaultValue();

		context.Detail("sentinel=%p default=%p HasDefaultValue=%s",
			static_cast<void*>(sentinel), static_cast<void*>(actual),
			data->HasDefaultValue() ? "yes" : "no");

		if (data->HasDefaultValue() && actual == sentinel)
		{
			context.Fail("HasDefaultValue is true while the pointer is the "
			             "invalid sentinel, which its own definition rules out");
		}
		if (!data->HasDefaultValue())
		{
			context.Fail("a registered convar always has a default value");
		}

		// The conversion operators are how the SDK reads a CVValue_t without
		// naming a member, so they are worth going through once.
		if (actual != nullptr)
		{
			const int32 via_operator = *actual;
			context.Detail("default via operator int32 = %d, via m_i32Value = %d",
				via_operator, actual->m_i32Value);
			if (via_operator != actual->m_i32Value)
			{
				context.Fail("the conversion operator and the member read "
				             "different bytes of the same union");
			}
		}
	});

	CS2ST_CALL(context, "ConVarData::Value / ValueOrDefault (slot bounds)", {
		CVValue_t* slot0 = data->Value(0);
		CVValue_t* implicit = data->Value(-1);
		CVValue_t* out_of_range = data->Value(MAX_SPLITSCREEN_CLIENTS + 1);
		CVValue_t* fallback = data->ValueOrDefault(MAX_SPLITSCREEN_CLIENTS + 1);

		context.Detail("slot0=%p slot-1=%p out-of-range=%p fallback=%p",
			static_cast<void*>(slot0), static_cast<void*>(implicit),
			static_cast<void*>(out_of_range), static_cast<void*>(fallback));

		if (slot0 == nullptr)
		{
			context.Fail("slot 0 is always in range");
		}
		if (implicit != slot0)
		{
			context.Fail("slot -1 means slot 0 in the SDK's implementation");
		}
		if (out_of_range != nullptr)
		{
			context.Fail("the SDK returns null for a slot outside the range");
		}
		if (fallback != data->DefaultValue())
		{
			context.Fail("ValueOrDefault falls back to the default value for an "
			             "invalid slot");
		}
	});

	CS2ST_CALL(context, "ConVarData::IsSetToDefault / IsAllSetToDefault / IsEqual", {
		const bool set_to_default = data->IsSetToDefault(0);
		const bool all_default = data->IsAllSetToDefault();
		const bool equal_to_default = data->IsEqual(0, data->DefaultValue());

		context.Detail("slot0=%s all=%s IsEqual(default)=%s",
			set_to_default ? "yes" : "no", all_default ? "yes" : "no",
			equal_to_default ? "yes" : "no");

		// Two routes to one fact: IsSetToDefault compares the slot against the
		// default, and IsEqual does the same comparison from the other side.
		if (set_to_default != equal_to_default)
		{
			context.Fail("IsSetToDefault and IsEqual(default) cannot disagree "
			             "unless they are reading different memory");
		}
	});

	CS2ST_CALL(context, "ConVarData::ValueToString / DefaultValueToString / "
	                    "MinValueToString / MaxValueToString", {
		CBufferStringN<64> value(false);
		CBufferStringN<64> default_value(false);
		CBufferStringN<64> min_value(false);
		CBufferStringN<64> max_value(false);

		data->ValueToString(0, value);
		data->DefaultValueToString(default_value);
		data->MinValueToString(min_value);
		data->MaxValueToString(max_value);

		context.Detail("value=\"%s\" default=\"%s\" min=\"%s\" max=\"%s\"",
			value.Get(), default_value.Get(), min_value.Get(), max_value.Get());

		if (std::strcmp(default_value.Get(), "7") != 0)
		{
			context.Fail("the default stringifies to \"%s\", not \"7\"",
				default_value.Get());
		}
	});

	// GetTimesChanged, IncrementTimesChanged and SetTimesChanged make a closed
	// loop on a counter this plugin owns, so all three get tested and the
	// convar ends where it started.
	CS2ST_CALL(context, "ConVarData::GetTimesChanged / IncrementTimesChanged / SetTimesChanged", {
		const int before = data->GetTimesChanged();
		data->IncrementTimesChanged();
		const int after_increment = data->GetTimesChanged();
		data->SetTimesChanged(before);
		const int restored = data->GetTimesChanged();

		context.Detail("%d -> %d -> %d", before, after_increment, restored);

		if (after_increment != before + 1)
		{
			context.Fail("IncrementTimesChanged moved the counter from %d to %d",
				before, after_increment);
		}
		if (restored != before)
		{
			context.Fail("SetTimesChanged did not put it back");
		}
	});

	CS2ST_CALL(context, "ConVarData::Clamp (already inside its bounds)", {
		// A no-op by design: the value is in range, so this checks that Clamp
		// reaches the min/max pointers without disturbing anything.
		const int32 before = g_probe_int.Get();
		data->Clamp(0);
		const int32 after = g_probe_int.Get();
		context.Detail("%d -> %d", before, after);
		if (before != after)
		{
			context.Fail("clamping a value already within 0..100 changed it");
		}
	});

	// The default/min/max mutators are called in ExerciseScratchCalls, against
	// cstmp_scratch_int, so the probe's declared shape stays fixed here. These
	// three have no safe target: the engine still holds the block they would
	// reset or destroy.
	context.SkipCall("ConVarData::Construct", "would re-construct a live value in "
		"engine-owned storage");
	context.SkipCall("ConVarData::Destruct", "would destruct a live value the "
		"engine still considers initialised");
	context.SkipCall("ConVarData::Invalidate", "resets the block to its "
		"pre-registration state while the engine still has it in its lists");
}

// -----------------------------------------------------------------------------
//  CConVarRef<T> / CConVar<T>
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseTypedRefs(TestContext& context)
{
	context.Note("[CConVarRef<T>] the typed accessors");

	CS2ST_CALL(context, "CConVarRef<int32>::Get / GetDefault / GetMin / GetMax", {
		const int32 value = g_probe_int.Get();
		const int32 default_value = g_probe_int.GetDefault();
		const int32 min_value = g_probe_int.HasMin() ? g_probe_int.GetMin() : 0;
		const int32 max_value = g_probe_int.HasMax() ? g_probe_int.GetMax() : 0;

		context.Detail("value=%d default=%d min=%d max=%d",
			value, default_value, min_value, max_value);

		if (default_value != 7 || min_value != 0 || max_value != 100)
		{
			context.Fail("registered as default 7, min 0, max 100");
		}
		// The typed reader and the generic one read the same storage through
		// different code, so they cannot differ.
		if (value != g_probe_int.GetInt())
		{
			context.Fail("Get() and GetInt() disagree");
		}
	});

	CS2ST_CALL(context, "CConVarRef<bool/float32/int64>::Get", {
		context.Detail("bool=%s float=%.3f int64=%lld",
			g_probe_bool.Get() ? "true" : "false",
			static_cast<double>(g_probe_float.Get()),
			static_cast<long long>(g_probe_int64.Get()));

		if (g_probe_int64.GetDefault() != -5)
		{
			context.Fail("cs2sdktest_int64 was registered with default -5");
		}
	});

	CS2ST_CALL(context, "CConVarRef<Color/Vector>::Get (non-primitive types)", {
		const Color& color = g_probe_color.Get();
		const Vector& vec = g_probe_vec3.Get();

		context.Detail("color=%d %d %d %d vec3=%.2f %.2f %.2f",
			color.r(), color.g(), color.b(), color.a(),
			static_cast<double>(vec.x), static_cast<double>(vec.y),
			static_cast<double>(vec.z));

		const Color& default_color = g_probe_color.GetDefault();
		if (default_color.r() != 10 || default_color.g() != 20
			|| default_color.b() != 30 || default_color.a() != 40)
		{
			context.Fail("registered with default 10 20 30 40, storage holds "
			             "%d %d %d %d", default_color.r(), default_color.g(),
				default_color.b(), default_color.a());
		}
	});

	// The documented failure mode of the typed ref. The header's own comment on
	// this constructor says "otherwise this would be initialised to invalid
	// convar data!", and the bad-cast branch is literally
	// Init(ConVarRef(), TranslateConVarType<T>()) - the same unbound path the
	// check above just measured, with the same result: the engine's
	// GetConVarData answers a non-null ConVarData for an invalid handle, so the
	// placeholder is never reached and IsConVarDataValid reports yes.
	//
	// What matters for a plugin is that the bad cast is still detectable, and
	// it is - through IsConVarDataAvailable. That is what is asserted; the
	// header's stronger promise is recorded as a SUSPECT rather than a FAIL,
	// because the reason it does not hold is understood and lives on the engine
	// side of a virtual, not in a layout this project can check.
	CS2ST_CALL(context, "CConVarRef<T> (type mismatch falls back to invalid data)", {
		CConVarRef<float32> wrong_type(static_cast<const ConVarRefAbstract&>(g_probe_int));
		const bool bad_cast_valid = wrong_type.IsConVarDataValid();
		const bool bad_cast_available = wrong_type.IsConVarDataAvailable();

		context.Detail("CConVarRef<float32> over an int32 convar: valid=%s available=%s",
			bad_cast_valid ? "yes" : "no", bad_cast_available ? "yes" : "no");

		if (bad_cast_available)
		{
			// This one would be genuinely dangerous: Get() reinterprets the
			// storage as float32, so a bad cast that looks usable reads an
			// int32's bytes as a float.
			context.Fail("a bad typed cast reports usable convar data; Get() would "
			             "reinterpret an int32's bytes as float32");
		}
	});

	CS2ST_CALL(context, "CConVarRef<int32> (from a plain ConVarRef)", {
		const ConVarRef found = cvar_->FindConVar("cs2sdktest_int", false);
		CConVarRef<int32> from_handle(found);
		context.Detail("value=%d valid=%s", from_handle.IsConVarDataValid()
			? from_handle.Get() : 0,
			from_handle.IsConVarDataValid() ? "yes" : "no");

		if (!from_handle.IsConVarDataValid())
		{
			context.Fail("the handle came from FindConVar on a live int32 convar");
		}
		else if (from_handle.Get() != g_probe_int.Get())
		{
			context.Fail("a ref rebuilt from the handle reads a different value");
		}
	});

	// CConVar<T>::CConVar and ~CConVar are called in ExerciseScratchCalls.
}

// -----------------------------------------------------------------------------
//  The value path, on convars this plugin owns
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseValueWrites(TestContext& context)
{
	context.Note("[value path] writes, all of them to cs2sdktest_* convars - "
	             "no engine convar is set anywhere in this suite");

	CS2ST_CALL(context, "CConVarRef<int32>::Set (and read back)", {
		g_probe_int.Set(42);
		const int32 value = g_probe_int.Get();
		context.Detail("set 42 -> %d, set-to-default=%s", value,
			g_probe_int.IsSetToDefault() ? "yes" : "no");

		if (value != 42)
		{
			context.Fail("wrote 42, read %d", value);
		}
		if (g_probe_int.IsSetToDefault())
		{
			context.Fail("42 is not the declared default 7");
		}
	});

	// The bounds we registered have to be enforced by the engine's copy of
	// them, which is the only way to tell that min/max actually made the trip.
	CS2ST_CALL(context, "ConVarRefAbstract::SetInt (clamped to the declared max)", {
		g_probe_int.SetInt(999);
		const int32 value = g_probe_int.Get();
		context.Detail("set 999 -> %d (declared max 100)", value);
		if (value != 100)
		{
			context.Fail("a value above the declared max should clamp to 100");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::SetInt (clamped to the declared min)", {
		g_probe_int.SetInt(-5);
		const int32 value = g_probe_int.Get();
		context.Detail("set -5 -> %d (declared min 0)", value);
		if (value != 0)
		{
			context.Fail("a value below the declared min should clamp to 0");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::SetString (parsed into an int32 convar)", {
		const bool parsed = g_probe_int.SetString("13");
		const int32 value = g_probe_int.Get();
		context.Detail("SetString(\"13\") -> %s, value=%d",
			parsed ? "true" : "false", value);

		if (!parsed)
		{
			context.Fail("\"13\" is a valid int32");
		}
		if (value != 13)
		{
			context.Fail("parsed value landed as %d", value);
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::SetString (unparseable)", {
		const int32 before = g_probe_int.Get();
		const bool parsed = g_probe_int.SetString("not-a-number");
		const int32 after = g_probe_int.Get();
		context.Detail("-> %s, value %d -> %d", parsed ? "true" : "false",
			before, after);
		// V_StringToValue is what decides this; recorded rather than asserted
		// because the SDK does not say what an int32 parse of junk yields.
		if (parsed && after != before)
		{
			context.Suspect("junk parsed as %d", after);
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::SetBool", {
		const bool before = g_probe_bool.Get();
		g_probe_bool.SetBool(!before);
		const bool after = g_probe_bool.Get();
		context.Detail("%s -> %s", before ? "true" : "false", after ? "true" : "false");
		if (after == before)
		{
			context.Fail("the value did not change");
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::SetFloat", {
		g_probe_float.SetFloat(4.25f);
		const float32 value = g_probe_float.Get();
		context.Detail("set 4.25 -> %.4f", static_cast<double>(value));
		if (std::fabs(static_cast<double>(value) - 4.25) > 1e-4)
		{
			context.Fail("wrote 4.25, read %.4f", static_cast<double>(value));
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::SetInt (converted into a float32 convar)", {
		// The primitive-conversion path: an int handed to a float convar goes
		// through ConvertToPrimitiveFrom rather than the typed setter.
		g_probe_float.SetInt(6);
		const float32 value = g_probe_float.Get();
		context.Detail("SetInt(6) on a float32 convar -> %.4f",
			static_cast<double>(value));
		if (std::fabs(static_cast<double>(value) - 6.0) > 1e-4)
		{
			context.Fail("the int-to-float conversion produced %.4f",
				static_cast<double>(value));
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::SetAs<Color> (a non-primitive type)", {
		g_probe_color.SetAs<Color>(Color(1, 2, 3, 4));
		const Color& value = g_probe_color.Get();
		context.Detail("set 1 2 3 4 -> %d %d %d %d",
			value.r(), value.g(), value.b(), value.a());
		if (value.r() != 1 || value.g() != 2 || value.b() != 3 || value.a() != 4)
		{
			context.Fail("the Color round trip did not preserve the value");
		}
	});

	CS2ST_CALL(context, "CConVarRef<Vector>::Set", {
		g_probe_vec3.Set(Vector(9.0f, 8.0f, 7.0f));
		const Vector& value = g_probe_vec3.Get();
		context.Detail("set 9 8 7 -> %.2f %.2f %.2f",
			static_cast<double>(value.x), static_cast<double>(value.y),
			static_cast<double>(value.z));
		if (std::fabs(static_cast<double>(value.x) - 9.0) > 1e-4)
		{
			context.Fail("the Vector round trip did not preserve the value");
		}
	});

	CS2ST_CALL(context, "CConVarRef<int32>::Set (per-splitscreen-slot storage)", {
		// FCVAR_PER_USER gives this one more than one slot, so slot 1 has to be
		// writable without disturbing slot 0 - which is a check on the stride
		// the engine used when it sized m_Values.
		const int slots = cvar_->GetMaxSplitScreenSlots();
		if (slots < 2)
		{
			context.Detail("the server reports %d slot(s); nothing to compare", slots);
			return;
		}

		g_probe_peruser.Set(11, 0);
		g_probe_peruser.Set(22, 1);
		const int32 slot0 = g_probe_peruser.Get(0);
		const int32 slot1 = g_probe_peruser.Get(1);

		context.Detail("slot0=%d slot1=%d", slot0, slot1);
		if (slot0 != 11 || slot1 != 22)
		{
			context.Fail("per-slot writes bled into each other (wrote 11/22, "
			             "read %d/%d)", slot0, slot1);
		}
	});

	CS2ST_CALL(context, "ConVarRefAbstract::Revert", {
		g_probe_int.SetInt(77);
		g_probe_int.Revert();
		const int32 value = g_probe_int.Get();
		context.Detail("dirtied then reverted -> %d, set-to-default=%s",
			value, g_probe_int.IsSetToDefault() ? "yes" : "no");

		if (value != 7 || !g_probe_int.IsSetToDefault())
		{
			context.Fail("Revert should restore the declared default 7");
		}
	});

	// ResetConVarsToDefaultValuesByName, aimed at a prefix that matches exactly
	// one convar. Narrow on purpose: the prefix is the whole safety argument
	// for calling a method that otherwise reaches every convar whose name
	// starts with whatever string it is given.
	CS2ST_CALL(context, "ICvar::ResetConVarsToDefaultValuesByName (our prefix only)", {
		g_probe_int.SetInt(63);
		const bool dirty_before = !g_probe_int.IsSetToDefault();

		cvar_->ResetConVarsToDefaultValuesByName("cs2sdktest_int");

		const int32 value = g_probe_int.Get();
		context.Detail("dirty=%s, after reset value=%d set-to-default=%s",
			dirty_before ? "yes" : "no", value,
			g_probe_int.IsSetToDefault() ? "yes" : "no");

		if (!dirty_before)
		{
			context.Fail("the convar did not take the value 63, so the reset "
			             "proves nothing");
		}
		else if (value != 7)
		{
			context.Fail("the prefix matched this convar but its value is %d, "
			             "not the declared default 7", value);
		}
	});

	CS2ST_CALL(context, "ICvar::QueueThreadSetValue", {
		// The deferred setter the SDK uses when a value is set from inside a
		// callback. Whether it lands immediately depends on when the engine
		// drains its queue, so the value is recorded rather than asserted.
		const int32 before = g_probe_int.Get();

		CVValue_t queued;
		queued.m_i32Value = 31;
		cvar_->QueueThreadSetValue(&g_probe_int, CSplitScreenSlot(0), nullptr, &queued);

		const int32 after = g_probe_int.Get();
		context.Detail("queued 31: %d -> %d (%s)", before, after,
			after == 31 ? "applied immediately" : "still queued");
	});

	// Whatever the section above did, the probes go back to their declared
	// values - a sweep that leaves them dirty would make the next run's oracles
	// read differently for no reason.
	CS2ST_CALL(context, "restore every probe convar to its declared default", {
		g_probe_int.Revert();
		g_probe_bool.Revert();
		g_probe_float.Revert();
		g_probe_int64.Revert();
		g_probe_color.Revert();
		g_probe_vec3.Revert();
		g_probe_peruser.Revert(0);
		g_probe_peruser.Revert(1);
		g_probe_defensive.Revert();

		const bool all_default = g_probe_int.IsSetToDefault()
			&& g_probe_bool.IsSetToDefault()
			&& g_probe_float.IsSetToDefault()
			&& g_probe_int64.IsSetToDefault()
			&& g_probe_color.IsSetToDefault()
			&& g_probe_vec3.IsSetToDefault();

		context.Detail("all primitives back to default: %s",
			all_default ? "yes" : "no");
		if (!all_default)
		{
			context.Fail("at least one probe convar did not revert; the next "
			             "sweep will read different values");
		}
	});

	// Not a skipped method - every setter above was called. What is being
	// recorded is the boundary the suite keeps: the target is always one of
	// ours.
	context.Note("  every setter above was called only on a cs2sdktest_* convar. "
	             "Setting one the game owns changes how the server plays, and "
	             "the code path is identical, so there is nothing extra to learn "
	             "from doing it");

	context.SkipCall("ConVarRefAbstract::SetString (EConVarType_String convars)",
		"a string value is a heap buffer, and this module does not share tier0's "
		"allocator (CS2SDKTEST_USE_MEMOVERRIDE is off), so the engine would end "
		"up freeing memory this plugin allocated");
	context.SkipCall("ConVarRefAbstract::SetAs<CUtlString>",
		"same allocator boundary as SetString on a string convar");
	context.SkipCall("ConVarRefAbstract::SetOrQueueValueInternal (from inside a callback)",
		"the SDK warns that the typed setters crash when called from a change "
		"callback because of the CConVarRef cast they do; the queueing branch is "
		"reached through ICvar::QueueThreadSetValue above instead");
}

// -----------------------------------------------------------------------------
//  Change, filter and global callbacks
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseChangeCallbacks(TestContext& context)
{
	context.Note("[callbacks] cs2sdktest_int carries a change callback and a "
	             "filter callback, both this plugin's");

	// A correct count here proves the whole chain: the engine found the
	// callback index in the ConVarData it allocated, walked its callback list,
	// called our generated provider thunk with five arguments, and the thunk
	// cast them back to the typed signature.
	CS2ST_CALL(context, "change and filter callbacks fire on a write", {
		const int changes_before = g_probe.int_changes;
		const int filters_before = g_probe.int_filters;

		g_probe_int.Set(58);

		const int changes = g_probe.int_changes - changes_before;
		const int filters = g_probe.int_filters - filters_before;

		context.Detail("change fired %d time(s) (new=%d old=%d slot=%d), "
		               "filter fired %d time(s)",
			changes, g_probe.int_change_new, g_probe.int_change_old,
			g_probe.int_change_slot, filters);

		if (changes != 1)
		{
			context.Fail("one write should produce exactly one change callback");
		}
		if (filters != 1)
		{
			context.Fail("one write should produce exactly one filter callback");
		}
		if (changes == 1 && g_probe.int_change_new != 58)
		{
			context.Fail("the callback was handed %d as the new value, not 58",
				g_probe.int_change_new);
		}

		g_probe_int.Revert();
	});

	CS2ST_CALL(context, "ICvar::IterateConVarCallbacks", {
		int seen = 0;
		bool found_ours = false;
		FnGenericChangeCallback_t expected =
			reinterpret_cast<FnGenericChangeCallback_t>(&ProbeIntChanged);

		cvar_->IterateConVarCallbacks(cvar_->FindConVar("cs2sdktest_int", false),
			[&](FnGenericChangeCallbackProvider_t provider, FnGenericChangeCallback_t callback) {
				++seen;
				if (callback == expected)
				{
					found_ours = true;
				}
				(void)provider;
			});

		context.Detail("-> %d callback(s), ours among them: %s",
			seen, found_ours ? "yes" : "no");

		if (seen == 0)
		{
			context.Fail("cs2sdktest_int was registered with a change callback");
		}
		else if (!found_ours)
		{
			context.Fail("the callback list does not contain the function pointer "
			             "we registered");
		}
	});

	CS2ST_CALL(context, "ICvar::CallChangeCallback", {
		// Invoked directly rather than through a write, on our own convar, so
		// the slot is exercised even if the value path above changed shape.
		const int before = g_probe.int_changes;

		CVValue_t new_value;
		CVValue_t old_value;
		new_value.m_i32Value = 21;
		old_value.m_i32Value = 7;

		cvar_->CallChangeCallback(cvar_->FindConVar("cs2sdktest_int", false),
			CSplitScreenSlot(0), &new_value, &old_value, nullptr);

		const int fired = g_probe.int_changes - before;
		context.Detail("-> our callback fired %d time(s), saw new=%d",
			fired, g_probe.int_change_new);

		if (fired != 1)
		{
			context.Fail("a direct call should invoke the one registered callback "
			             "exactly once");
		}
	});

	CS2ST_CALL(context, "ICvar::CallFilterCallback (convar with a filter)", {
		const int before = g_probe.int_filters;

		CVValue_t new_value;
		CVValue_t old_value;
		new_value.m_i32Value = 21;
		old_value.m_i32Value = 7;

		const bool allowed = cvar_->CallFilterCallback(
			cvar_->FindConVar("cs2sdktest_int", false), CSplitScreenSlot(0),
			&new_value, &old_value, nullptr);

		context.Detail("-> %s, our filter fired %d time(s)",
			allowed ? "allowed" : "rejected", g_probe.int_filters - before);

		// Our filter returns true unconditionally, so anything else means the
		// engine did not reach it.
		if (!allowed)
		{
			context.Fail("the registered filter always allows the write");
		}
	});

	CS2ST_CALL(context, "ICvar::CallFilterCallback (convar with no filter)", {
		CVValue_t new_value;
		CVValue_t old_value;
		new_value.m_bValue = true;
		old_value.m_bValue = false;

		const bool allowed = cvar_->CallFilterCallback(
			cvar_->FindConVar("cs2sdktest_bool", false), CSplitScreenSlot(0),
			&new_value, &old_value, nullptr);

		context.Detail("-> %s", allowed ? "allowed" : "rejected");
		if (!allowed)
		{
			context.Fail("with no filter registered the write must be allowed - "
			             "the SDK says a false return means the value should not "
			             "be modified");
		}
	});

	// InstallGlobalChangeCallback and RemoveGlobalChangeCallback, closed on
	// themselves: install, prove it fires, remove, prove it stopped. Nothing is
	// left registered, so the sweep can be run repeatedly.
	CS2ST_CALL(context, "ICvar::InstallGlobalChangeCallback / RemoveGlobalChangeCallback", {
		const int before_install = g_probe.probe_global_changes;

		cvar_->InstallGlobalChangeCallback(&ProbeGlobalChanged);
		g_probe_int.SetInt(64);
		const int while_installed = g_probe.probe_global_changes - before_install;

		cvar_->RemoveGlobalChangeCallback(&ProbeGlobalChanged);
		const int after_remove_baseline = g_probe.probe_global_changes;
		g_probe_int.SetInt(65);
		const int after_remove = g_probe.probe_global_changes - after_remove_baseline;

		g_probe_int.Revert();

		context.Detail("installed: %d call(s) (name=\"%s\" new=\"%s\"); "
		               "after remove: %d call(s)",
			while_installed, g_probe.probe_global_name, g_probe.probe_global_new,
			after_remove);

		if (while_installed < 1)
		{
			context.Fail("an installed global callback should see a convar change");
		}
		if (after_remove != 0)
		{
			context.Fail("the callback still fires after RemoveGlobalChangeCallback");
		}
		if (while_installed >= 1
			&& std::strcmp(g_probe.probe_global_name, "cs2sdktest_int") != 0)
		{
			context.Fail("the callback was handed \"%s\" as the changing convar",
				g_probe.probe_global_name);
		}
	});

	// ICvar::CallGlobalChangeCallbacks is called directly in ExerciseScratchCalls.
}

// -----------------------------------------------------------------------------
//  Creation listeners, and the registrations they observe
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseCreationListeners(TestContext& context)
{
	context.Note("[IConVarListener] creation listeners, checked against a "
	             "registration made while one is installed");

	// The direct RegisterConVar/RegisterConCommand calls happen here, inside
	// the listener's window, because that is the only way to prove the listener
	// fires: the callbacks need a creation to observe, and a creation is
	// permanent, so there is exactly one per plugin load to spend.
	if (dynamic_registered_)
	{
		CS2ST_CALL(context, "ICvar::RegisterCreationListeners / RemoveCreationListeners", {
			cvar_->RegisterCreationListeners(&g_creation_probe);
			cvar_->RemoveCreationListeners(&g_creation_probe);
			context.Detail("registered and removed; no creation to observe "
			               "(the one direct registration for this load is spent)");
		});
		context.Note("  re-run after a plugin reload to exercise the callbacks; "
		             "cvars=%d cmds=%d seen so far this load",
			g_creation_probe.cvars, g_creation_probe.commands);
		return;
	}

	CS2ST_CALL(context, "ICvar::RegisterCreationListeners", {
		cvar_->RegisterCreationListeners(&g_creation_probe);
		context.Detail("installed the probe listener (cvars=%d cmds=%d so far)",
			g_creation_probe.cvars, g_creation_probe.commands);
	});

	const int cvars_before = g_creation_probe.cvars;
	const int commands_before = g_creation_probe.commands;

	CS2ST_CALL(context, "ICvar::RegisterConVar (direct, once per plugin load)", {
		// Built by hand rather than through CConVar, so the ConVarCreation_t
		// and ConVarValueInfo_t layouts are exercised as data the engine reads
		// - the opposite direction from everything above.
		ConVarCreation_t setup;
		setup.m_pszName = "cs2sdktest_dynamic";
		setup.m_pszHelpString = "cs2sdktest: registered through ICvar::RegisterConVar";
		setup.m_nFlags = kProbeFlags;

		// Filled in as completely as the struct allows, so the whole of
		// ConVarValueInfo_t travels rather than just its default-value slot:
		// the bounds land in one buffer each and the two callbacks land as a
		// generated provider thunk plus the raw function pointer.
		ConVarValueInfo_t value_info(TranslateConVarType<int32>());
		value_info.SetDefaultValue<int32>(1234);
		value_info.SetMinValue<int32>(0);
		value_info.SetMaxValue<int32>(5000);
		value_info.SetCallback<int32>(ProbeIntChanged);
		value_info.SetFilterCallback<int32>(ProbeIntFilter);
		setup.m_valueInfo = value_info;

		dynamic_cvar_.InvalidateRef();
		dynamic_cvar_data_ = nullptr;

		cvar_->RegisterConVar(setup, 0, &dynamic_cvar_, &dynamic_cvar_data_);

		context.Detail("-> access=%u registered=%d data=%p",
			static_cast<unsigned>(dynamic_cvar_.GetAccessIndex()),
			dynamic_cvar_.GetRegisteredIndex(),
			static_cast<void*>(dynamic_cvar_data_));

		if (!dynamic_cvar_.IsValidRef())
		{
			context.Fail("the out ConVarRef was not filled in; tier1 treats that "
			             "as a fatal error at startup");
		}
		if (dynamic_cvar_data_ == nullptr)
		{
			context.Fail("the out ConVarData pointer was not filled in");
		}
	});

	CS2ST_CALL(context, "ICvar::RegisterConCommand (direct, once per plugin load)", {
		ConCommandCreation_t setup;
		setup.m_pszName = "cs2sdktest_dynamic_cmd";
		setup.m_pszHelpString = "cs2sdktest: registered through ICvar::RegisterConCommand";
		setup.m_nFlags = kProbeFlags;
		setup.m_CBInfo = ConCommandCallbackInfo_t(ProbeCommandCallback);

		// The union-plus-bitfields struct has four constructors picked by
		// callback shape; the one above is the context-taking form, so all
		// three discriminator bits should be clear and IsValid should hold.
		if (!setup.m_CBInfo.IsValid())
		{
			context.Fail("a ConCommandCallbackInfo_t built from a real function "
			             "pointer reports itself invalid");
		}

		dynamic_command_ = cvar_->RegisterConCommand(setup, 0);

		context.Detail("-> access=%u registered=%d valid=%s",
			static_cast<unsigned>(dynamic_command_.GetAccessIndex()),
			dynamic_command_.GetRegisteredIndex(),
			dynamic_command_.IsValidRef() ? "yes" : "no");

		if (!dynamic_command_.IsValidRef())
		{
			context.Fail("the returned ConCommandRef is invalid; tier1 treats "
			             "that as a fatal error at startup");
		}
	});

	dynamic_registered_ = true;

	CS2ST_CALL(context, "IConVarListener::OnConVarCreated / OnConCommandCreated", {
		const int cvars = g_creation_probe.cvars - cvars_before;
		const int commands = g_creation_probe.commands - commands_before;

		context.Detail("-> %d convar creation(s), %d command creation(s) observed",
			cvars, commands);

		if (cvars < 1)
		{
			context.Fail("a convar was registered while the listener was "
			             "installed and OnConVarCreated did not fire");
		}
		if (commands < 1)
		{
			context.Fail("a command was registered while the listener was "
			             "installed and OnConCommandCreated did not fire");
		}
	});

	CS2ST_CALL(context, "ICvar::RemoveCreationListeners", {
		cvar_->RemoveCreationListeners(&g_creation_probe);
		context.Detail("removed the probe listener; the engine no longer holds "
		               "a pointer to it");
	});
}

// -----------------------------------------------------------------------------
//  Reading back the direct registration
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseDynamicRegistration(TestContext& context)
{
	context.Note("[direct registration] reading back what RegisterConVar and "
	             "RegisterConCommand produced");

	CS2ST_CALL(context, "ICvar::FindConVar (the directly registered convar)", {
		if (!dynamic_cvar_.IsValidRef())
		{
			context.Detail("no direct registration to read back this run");
			return;
		}

		const ConVarRef found = cvar_->FindConVar("cs2sdktest_dynamic", true);
		context.Detail("registration gave access=%u, lookup gives access=%u",
			static_cast<unsigned>(dynamic_cvar_.GetAccessIndex()),
			static_cast<unsigned>(found.GetAccessIndex()));

		if (!found.IsValidRef())
		{
			context.Fail("the name we registered is not findable");
		}
		else if (found.GetAccessIndex() != dynamic_cvar_.GetAccessIndex())
		{
			context.Fail("the handle from registration and the handle from "
			             "lookup are different");
		}
	});

	CS2ST_CALL(context, "ICvar::GetConVarData (vs the RegisterConVar out-param)", {
		if (!dynamic_cvar_.IsValidRef() || dynamic_cvar_data_ == nullptr)
		{
			context.Detail("no direct registration to read back this run");
			return;
		}

		ConVarData* looked_up = cvar_->GetConVarData(dynamic_cvar_);
		context.Detail("out-param=%p, GetConVarData=%p, name=\"%s\", default=%d",
			static_cast<void*>(dynamic_cvar_data_), static_cast<void*>(looked_up),
			dynamic_cvar_data_->GetName(),
			dynamic_cvar_data_->DefaultValue() != nullptr
				? dynamic_cvar_data_->DefaultValue()->m_i32Value : 0);

		if (looked_up != dynamic_cvar_data_)
		{
			context.Fail("the pointer RegisterConVar wrote and the one "
			             "GetConVarData returns are different blocks");
		}
		if (dynamic_cvar_data_->GetName() == nullptr
			|| std::strcmp(dynamic_cvar_data_->GetName(), "cs2sdktest_dynamic") != 0)
		{
			context.Fail("the name in the block is not the one we passed");
		}
		if (dynamic_cvar_data_->DefaultValue() == nullptr
			|| dynamic_cvar_data_->DefaultValue()->m_i32Value != 1234)
		{
			context.Fail("we passed 1234 as the default through "
			             "ConVarValueInfo_t::SetDefaultValue");
		}
	});

	// The rest of the creation struct. Every field below was set through a
	// different ConVarValueInfo_t setter, so this is the readback that says
	// whether the whole struct crossed intact rather than just its first
	// member.
	CS2ST_CALL(context, "ConVarValueInfo_t (bounds and callbacks that crossed)", {
		if (dynamic_cvar_data_ == nullptr)
		{
			context.Detail("no direct registration to read back this run");
			return;
		}

		const bool has_min = dynamic_cvar_data_->HasMinValue();
		const bool has_max = dynamic_cvar_data_->HasMaxValue();
		const int32 min_value = has_min ? dynamic_cvar_data_->MinValue()->m_i32Value : 0;
		const int32 max_value = has_max ? dynamic_cvar_data_->MaxValue()->m_i32Value : 0;

		context.Detail("min=%s(%d) max=%s(%d) callback index=%d type=%s",
			has_min ? "yes" : "no", min_value,
			has_max ? "yes" : "no", max_value,
			dynamic_cvar_data_->GetCallbackIndex(),
			ConVarTypeName(dynamic_cvar_data_->GetType()));

		if (!has_min || min_value != 0)
		{
			context.Fail("SetMinValue passed 0");
		}
		if (!has_max || max_value != 5000)
		{
			context.Fail("SetMaxValue passed 5000");
		}
		if (dynamic_cvar_data_->GetCallbackIndex() == 0)
		{
			context.Fail("SetCallback passed a change callback, so the engine "
			             "should have stored an index for it");
		}
		if (dynamic_cvar_data_->GetType() != TranslateConVarType<int32>())
		{
			context.Fail("the type in the creation struct did not survive");
		}
	});

	// ConVarValueInfo_t::SetCompletionCallback is called in ExerciseScratchCalls.

	CS2ST_CALL(context, "ICvar::GetConCommandData (the directly registered command)", {
		if (!dynamic_command_.IsValidRef())
		{
			context.Detail("no direct registration to read back this run");
			return;
		}

		ConCommandData* data = cvar_->GetConCommandData(dynamic_command_);
		if (data == nullptr)
		{
			context.Fail("the SDK says this never returns null");
			return;
		}
		context.Detail("name=\"%s\" flags=0x%llx callback=%s", data->GetName(),
			static_cast<unsigned long long>(data->GetFlags()),
			data->HasCallback() ? "yes" : "no");

		if (data->GetName() == nullptr
			|| std::strcmp(data->GetName(), "cs2sdktest_dynamic_cmd") != 0)
		{
			context.Fail("the block holds a different name than we registered");
		}
		if (!data->HasCallback())
		{
			context.Fail("we passed a callback in the ConCommandCreation_t");
		}
	});

	// A cross-module reference to a convar that is already registered. The
	// documented use is reaching another module's convar without seeing its
	// CConVar; pointing it at one of ours means it resolves to an existing
	// entry instead of creating a new permanent one.
	CS2ST_CALL(context, "CConVarRef<int32> (by name, the FCVAR_REFERENCE path)", {
		CConVarRef<int32> reference("cs2sdktest_int");
		const bool available = reference.IsConVarDataAvailable();

		context.Detail("value=%d valid=%s available=%s; the real convar is still "
		               "available=%s",
			reference.IsConVarDataValid() ? reference.Get() : 0,
			reference.IsConVarDataValid() ? "yes" : "no",
			available ? "yes" : "no",
			g_probe_int.IsConVarDataAvailable() ? "yes" : "no");

		if (!reference.IsConVarDataValid())
		{
			context.Fail("a reference to an already-registered int32 convar "
			             "should resolve to it");
		}
		if (!g_probe_int.IsConVarDataAvailable())
		{
			context.Fail("taking a reference left FCVAR_REFERENCE set on the "
			             "real convar, which makes it look unregistered");
		}
	});

	// UnregisterConVarCallbacks and UnregisterConCommandCallbacks, aimed at the
	// two directly-registered entries rather than at anything the plugin needs
	// afterwards. Per the SDK these leave the entries in the lists, so old refs
	// keep working - which is what the read-back above already used.
	CS2ST_CALL(context, "ICvar::UnregisterConVarCallbacks", {
		if (!dynamic_cvar_.IsValidRef())
		{
			context.Detail("no direct registration to unregister this run");
			return;
		}

		cvar_->UnregisterConVarCallbacks(dynamic_cvar_);

		const ConVarRef still_there = cvar_->FindConVar("cs2sdktest_dynamic", true);
		context.Detail("callbacks unregistered; the convar is %s",
			still_there.IsValidRef() ? "still in the list" : "gone from the list");

		if (!still_there.IsValidRef())
		{
			context.Fail("the SDK says this leaves the convar in the lists so "
			             "existing refs stay valid");
		}
	});

	CS2ST_CALL(context, "ICvar::UnregisterConCommandCallbacks", {
		if (!dynamic_command_.IsValidRef())
		{
			context.Detail("no direct registration to unregister this run");
			return;
		}

		cvar_->UnregisterConCommandCallbacks(dynamic_command_);

		const ConCommandRef still_there =
			cvar_->FindConCommand("cs2sdktest_dynamic_cmd", true);
		context.Detail("callbacks unregistered; the command is %s",
			still_there.IsValidRef() ? "still in the list" : "gone from the list");

		if (!still_there.IsValidRef())
		{
			context.Fail("the SDK says this leaves the command in the lists");
		}
	});

	// The lock is taken and released in two separate guarded calls on purpose:
	// if the first one faults nothing was locked, and if anything between them
	// faults the release still runs, because a caught fault returns to the
	// sweep rather than unwinding out of it.
	CS2ST_CALL(context, "ICvar::LockConVarValueInitialisation (true)", {
		cvar_->LockConVarValueInitialisation(true);
		context.Detail("default-value initialisation locked; convars registered "
		               "from here on are queued instead of initialised");
	});

	CS2ST_CALL(context, "ICvar::LockConVarValueInitialisation (false)", {
		cvar_->LockConVarValueInitialisation(false);
		context.Detail("unlocked, and any queued convars initialised");

		// The probes still have to read correctly afterwards: if the lock
		// pairing were wrong, the queue would have been drained over them.
		if (g_probe_int.Get() != 7)
		{
			context.Fail("cs2sdktest_int reads %d after the lock round trip, "
			             "not its default 7", g_probe_int.Get());
		}
	});
}

// -----------------------------------------------------------------------------
//  Finding and iterating commands
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseConCommandLookup(TestContext& context)
{
	context.Note("[ICvar] command lookup, plus ConCommandRef and ConCommandData");

	CS2ST_CALL(context, "ICvar::FindConCommand (this plugin's own commands)", {
		const ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		const ConCommandRef meta = cvar_->FindConCommand("cst", false);

		context.Detail("cs2sdktest_cmd: access=%u valid=%s | cst: valid=%s",
			static_cast<unsigned>(probe.GetAccessIndex()),
			probe.IsValidRef() ? "yes" : "no",
			meta.IsValidRef() ? "yes" : "no");

		if (!probe.IsValidRef())
		{
			context.Fail("this suite registered cs2sdktest_cmd at load");
		}
		else if (probe.GetAccessIndex() != g_probe_command.GetAccessIndex())
		{
			context.Fail("by-name lookup gives access index %u, our registration "
			             "produced %u",
				static_cast<unsigned>(probe.GetAccessIndex()),
				static_cast<unsigned>(g_probe_command.GetAccessIndex()));
		}
		if (!meta.IsValidRef())
		{
			context.Fail("cst is the command this plugin is driven by, so "
			             "it is registered by definition");
		}
	});

	CS2ST_CALL(context, "ConCommandRef (by-name constructor)", {
		// Reaches ICvar through tier1's own g_pCVar rather than through the
		// pointer this suite holds, so the two have to agree.
		const ConCommandRef by_name("cs2sdktest_cmd");
		const ConCommandRef by_interface = cvar_->FindConCommand("cs2sdktest_cmd", false);

		context.Detail("tier1 -> access=%u, interface -> access=%u",
			static_cast<unsigned>(by_name.GetAccessIndex()),
			static_cast<unsigned>(by_interface.GetAccessIndex()));

		if (!by_name.IsValidRef())
		{
			context.Fail("the by-name constructor did not resolve a registered "
			             "command");
		}
		else if (by_name.GetAccessIndex() != by_interface.GetAccessIndex())
		{
			context.Fail("tier1 and the interface disagree about the handle");
		}
	});

	CS2ST_CALL(context, "ICvar::FindConCommand (nonexistent name)", {
		const ConCommandRef found = cvar_->FindConCommand("cs2sdktest_no_such_cmd", true);
		context.Detail("-> valid=%s", found.IsValidRef() ? "yes" : "no");
		if (found.IsValidRef())
		{
			context.Fail("a name nothing registered came back as a valid ref");
		}
	});

	CS2ST_CALL(context, "ICvar::FindConCommand (defensive, both parameter values)", {
		const ConCommandRef without =
			cvar_->FindConCommand("cs2sdktest_cmd_defensive", false);
		const ConCommandRef with =
			cvar_->FindConCommand("cs2sdktest_cmd_defensive", true);

		context.Detail("allow_defensive=false -> %s, true -> %s%s",
			without.IsValidRef() ? "found" : "not found",
			with.IsValidRef() ? "found" : "not found",
			without.IsValidRef() ? "  (false does not hide it on this build)" : "");

		// Same asymmetry as FindConVar above: "Allows finding commands with
		// FCVAR_DEFENSIVE flag" pins down true and leaves false unspecified,
		// so only the true half is a contract worth failing on.
		if (!with.IsValidRef())
		{
			context.Fail("allow_defensive=true should find it");
		}
	});

	CS2ST_CALL(context, "ICvar::FindFirstConCommand / FindNextConCommand", {
		int walked = 0;
		int with_callback = 0;
		int with_completion = 0;
		bool saw_probe = false;
		bool saw_meta = false;

		for (ConCommandRef iter = cvar_->FindFirstConCommand();
		     iter.IsValidRef() && walked < kListWalkCap;
		     iter = cvar_->FindNextConCommand(iter), ++walked)
		{
			if (iter.HasCallback())
			{
				++with_callback;
			}
			if (iter.HasCompletionCallback())
			{
				++with_completion;
			}

			const char* name = iter.GetName();
			if (name == nullptr)
			{
				continue;
			}
			if (std::strcmp(name, "cs2sdktest_cmd") == 0)
			{
				saw_probe = true;
			}
			else if (std::strcmp(name, "cst") == 0)
			{
				saw_meta = true;
			}
		}

		context.Detail("-> %d commands (%d with a callback, %d with completion), "
		               "cs2sdktest_cmd=%s cst=%s%s",
			walked, with_callback, with_completion,
			saw_probe ? "seen" : "missing", saw_meta ? "seen" : "missing",
			walked >= kListWalkCap ? " (capped)" : "");

		if (walked >= kListWalkCap)
		{
			context.Fail("the walk hit its %d cap; FindNextConCommand may be "
			             "cycling", kListWalkCap);
		}
		else if (!saw_probe || !saw_meta)
		{
			context.Fail("the iteration missed a command this plugin registered");
		}
	});

	CS2ST_CALL(context, "ConCommandRef handle round-trip", {
		// Same argument as for ConVarRef: this is returned by value from
		// engine calls, so its two halves travel in registers.
		const ConCommandRef built(static_cast<uint16>(0x0abc), 0x0def);
		context.Detail("access=%u registered=%d valid=%s",
			static_cast<unsigned>(built.GetAccessIndex()),
			built.GetRegisteredIndex(), built.IsValidRef() ? "yes" : "no");

		if (built.GetAccessIndex() != 0x0abc || built.GetRegisteredIndex() != 0x0def)
		{
			context.Fail("a ConCommandRef does not read back the halves it was "
			             "built from");
		}

		ConCommandRef invalidated(built);
		invalidated.InvalidateRef();
		if (invalidated.IsValidRef())
		{
			context.Fail("InvalidateRef left the ref valid");
		}
	});

	CS2ST_CALL(context, "ConCommandRef::GetRawData (vs ICvar::GetConCommandData)", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		ConCommandData* from_ref = probe.GetRawData();
		ConCommandData* from_icvar = cvar_->GetConCommandData(probe);

		context.Detail("GetRawData=%p GetConCommandData=%p",
			static_cast<void*>(from_ref), static_cast<void*>(from_icvar));

		if (from_ref == nullptr || from_icvar == nullptr)
		{
			context.Fail("the SDK says the command data is never null");
		}
		else if (from_ref != from_icvar)
		{
			context.Fail("the two routes to the command data disagree");
		}
	});

	CS2ST_CALL(context, "ICvar::GetConCommandData (invalid ref)", {
		// Documented as returning an "<unknown>" placeholder rather than null,
		// which makes it one of the few no-argument-needed guarantees here.
		ConCommandData* data = cvar_->GetConCommandData(ConCommandRef());
		if (data == nullptr)
		{
			context.Fail("the SDK says this returns an empty command struct, "
			             "never null");
			return;
		}

		char scratch[48];
		context.Detail("-> %p name=\"%.60s\" flags=0x%llx",
			static_cast<void*>(data), SafeStringArg(data->GetName(), scratch),
			static_cast<unsigned long long>(data->GetFlags()));
	});

	CS2ST_CALL(context, "ConCommandRef::GetName / GetHelpText / HasHelpText / GetFlags", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		if (!probe.IsValidRef())
		{
			context.Fail("cs2sdktest_cmd is registered");
			return;
		}

		context.Detail("name=\"%s\" help=%s flags=0x%llx gamedll=%s",
			probe.GetName(), probe.HasHelpText() ? "yes" : "no",
			static_cast<unsigned long long>(probe.GetFlags()),
			probe.IsFlagSet(FCVAR_GAMEDLL) ? "yes" : "no");

		if (probe.GetName() == nullptr
			|| std::strcmp(probe.GetName(), "cs2sdktest_cmd") != 0)
		{
			context.Fail("the name does not read back as registered");
		}
		if (!probe.HasHelpText())
		{
			context.Fail("a help string was supplied at registration");
		}
		if (!probe.IsFlagSet(FCVAR_GAMEDLL))
		{
			context.Fail("registered with FCVAR_GAMEDLL");
		}
	});

	CS2ST_CALL(context, "ConCommandRef::HasCallback / HasCompletionCallback / "
	                    "GetCallbackIndex / GetCompletionCommandIndex", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		if (!probe.IsValidRef())
		{
			context.Fail("cs2sdktest_cmd is registered");
			return;
		}

		context.Detail("callback=%s (index %u) completion=%s (index %d)",
			probe.HasCallback() ? "yes" : "no",
			static_cast<unsigned>(probe.GetCallbackIndex()),
			probe.HasCompletionCallback() ? "yes" : "no",
			probe.GetCompletionCommandIndex());

		if (!probe.HasCallback())
		{
			context.Fail("registered with a command callback");
		}
		// No completion callback is registered on purpose - see the comment on
		// g_probe_command - so this is a declared fact about our own command.
		if (probe.HasCompletionCallback())
		{
			context.Fail("no completion callback was registered for this command");
		}
	});

	CS2ST_CALL(context, "ConCommandRef::AddFlags / RemoveFlags (round-trip)", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		if (!probe.IsValidRef())
		{
			context.Fail("cs2sdktest_cmd is registered");
			return;
		}

		probe.AddFlags(FCVAR_MENUBAR_ITEM);
		const bool after_add = probe.IsFlagSet(FCVAR_MENUBAR_ITEM);
		probe.RemoveFlags(FCVAR_MENUBAR_ITEM);
		const bool after_remove = probe.IsFlagSet(FCVAR_MENUBAR_ITEM);

		context.Detail("after add=%d after remove=%d",
			after_add ? 1 : 0, after_remove ? 1 : 0);

		if (!after_add)
		{
			context.Fail("AddFlags did not set the flag");
		}
		if (after_remove)
		{
			context.Fail("RemoveFlags did not clear the flag");
		}
	});

	CS2ST_CALL(context, "ConCommandData::GetName / GetHelpText / HasHelpText / "
	                    "GetFlags / IsFlagSet / HasCallback / HasCompletionCallback", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		ConCommandData* data = cvar_->GetConCommandData(probe);
		if (data == nullptr)
		{
			context.Fail("no command data for a registered command");
			return;
		}

		context.Detail("name=\"%s\" help=%s flags=0x%llx callback=%s completion=%s",
			data->GetName(), data->HasHelpText() ? "yes" : "no",
			static_cast<unsigned long long>(data->GetFlags()),
			data->HasCallback() ? "yes" : "no",
			data->HasCompletionCallback() ? "yes" : "no");

		// Two routes to the same fields, one through the ref and one through
		// the block it points at.
		if (data->GetFlags() != probe.GetFlags())
		{
			context.Fail("ConCommandData and ConCommandRef disagree about flags");
		}
		if (data->HasCallback() != probe.HasCallback())
		{
			context.Fail("the two routes disagree about whether a callback exists");
		}

		// m_CompletionCB is a public member, so the predicate can be checked
		// against the accessor that wraps it - and this command was registered
		// without a completion callback, so both must say so.
		if (data->m_CompletionCB.IsValid() != data->HasCompletionCallback())
		{
			context.Fail("CompletionCallbackInfo_t::IsValid and "
			             "HasCompletionCallback disagree");
		}
		if (data->m_CompletionCB.IsValid())
		{
			context.Fail("no completion callback was registered for this command");
		}
	});

	CS2ST_CALL(context, "ConCommandData::AddFlags / RemoveFlags (round-trip)", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		ConCommandData* data = cvar_->GetConCommandData(probe);
		if (data == nullptr)
		{
			context.Fail("no command data for a registered command");
			return;
		}

		data->AddFlags(FCVAR_MENUBAR_ITEM);
		const bool after_add = data->IsFlagSet(FCVAR_MENUBAR_ITEM);
		data->RemoveFlags(FCVAR_MENUBAR_ITEM);
		const bool after_remove = data->IsFlagSet(FCVAR_MENUBAR_ITEM);

		context.Detail("after add=%d after remove=%d",
			after_add ? 1 : 0, after_remove ? 1 : 0);
		if (!after_add || after_remove)
		{
			context.Fail("the flag round trip through ConCommandData did not hold");
		}
	});

	// ConCommand::Create, Destroy and the destructor are called in
	// ExerciseScratchCalls, on a command built and torn down there.
}

// -----------------------------------------------------------------------------
//  The callback-info structs
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseCallbackInfoShapes(TestContext& context)
{
	context.Note("[ConCommandCallbackInfo_t] the four callback shapes - built and "
	             "dispatched locally, never registered");

	// This struct is a union of four pointer types plus three one-bit
	// discriminators, and it goes into the engine inside ConCommandCreation_t.
	// Which branch Dispatch takes is decided entirely by those bits, so a
	// packing change would silently call the wrong signature - the kind of
	// failure that looks like a working plugin until a command is typed.
	CS2ST_CALL(context, "ConCommandCallbackInfo_t (context-taking form)", {
		const ConCommandCallbackInfo_t info(
			static_cast<FnCommandCallback_t>(ProbeCommandCallback));

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cs2sdktest_cmd shapes");

		const int before = g_probe.command_dispatches;
		info.Dispatch(ctx, args);
		const int fired = g_probe.command_dispatches - before;

		context.Detail("valid=%s, dispatched %d time(s)",
			info.IsValid() ? "yes" : "no", fired);

		if (!info.IsValid())
		{
			context.Fail("built from a real function pointer");
		}
		if (fired != 1)
		{
			context.Fail("the context-taking branch should have been taken once");
		}
	});

	CS2ST_CALL(context, "ConCommandCallbackInfo_t (void form)", {
		const ConCommandCallbackInfo_t info(
			static_cast<FnCommandCallbackVoid_t>(ProbeVoidCallback));

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cs2sdktest_cmd shapes");

		const int before = g_void_callbacks;
		const int others_before = g_probe.command_dispatches + g_contextless_callbacks
			+ g_interface_callbacks;
		info.Dispatch(ctx, args);
		const int others_after = g_probe.command_dispatches + g_contextless_callbacks
			+ g_interface_callbacks;

		context.Detail("valid=%s, void callback fired %d time(s)",
			info.IsValid() ? "yes" : "no", g_void_callbacks - before);

		if (g_void_callbacks - before != 1)
		{
			context.Fail("the void branch should have been taken once");
		}
		if (others_after != others_before)
		{
			context.Fail("dispatch took a branch other than the void one, so the "
			             "discriminator bits are not being read as declared");
		}
	});

	CS2ST_CALL(context, "ConCommandCallbackInfo_t (contextless form)", {
		const ConCommandCallbackInfo_t info(
			static_cast<FnCommandCallbackNoContext_t>(ProbeContextlessCallback));

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cs2sdktest_cmd shapes");

		const int before = g_contextless_callbacks;
		info.Dispatch(ctx, args);

		context.Detail("valid=%s, contextless callback fired %d time(s)",
			info.IsValid() ? "yes" : "no", g_contextless_callbacks - before);

		if (g_contextless_callbacks - before != 1)
		{
			context.Fail("the contextless branch should have been taken once");
		}
	});

	CS2ST_CALL(context, "ConCommandCallbackInfo_t (ICommandCallback form)", {
		const ConCommandCallbackInfo_t info(&g_callback_object);

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cs2sdktest_cmd shapes");

		const int before = g_interface_callbacks;
		info.Dispatch(ctx, args);

		context.Detail("valid=%s, ICommandCallback::CommandCallback fired %d time(s)",
			info.IsValid() ? "yes" : "no", g_interface_callbacks - before);

		if (g_interface_callbacks - before != 1)
		{
			context.Fail("the interface branch should have been taken once; this "
			             "is the shape CConCommandMemberAccessor uses, so the "
			             "game's own built-in commands depend on it");
		}
	});

	CS2ST_CALL(context, "ConCommandCallbackInfo_t (default constructed)", {
		const ConCommandCallbackInfo_t info;
		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cs2sdktest_cmd shapes");

		const int before = g_probe.command_dispatches + g_void_callbacks
			+ g_contextless_callbacks + g_interface_callbacks;
		info.Dispatch(ctx, args);
		const int after = g_probe.command_dispatches + g_void_callbacks
			+ g_contextless_callbacks + g_interface_callbacks;

		context.Detail("valid=%s, dispatch fired %d callback(s)",
			info.IsValid() ? "yes" : "no", after - before);

		if (info.IsValid())
		{
			context.Fail("a default-constructed info holds no callback");
		}
		if (after != before)
		{
			context.Fail("Dispatch returns early when IsValid is false");
		}
	});

	context.Note("[CompletionCallbackInfo_t] both completion shapes, dispatched "
	             "into a vector this suite owns");

	CS2ST_CALL(context, "CompletionCallbackInfo_t (function form)", {
		const CompletionCallbackInfo_t info(
			static_cast<FnCommandCompletionCallback>(ProbeCompletionFn));

		CCommand args;
		args.Tokenize("cs2sdktest_cmd ");
		CUtlVector<CUtlString> completions;
		UtlVectorGuard<decltype(completions)> completions_guard(completions);

		const int before = g_completion_fn_calls;
		const int returned = info.Dispatch(args, completions);

		context.Detail("valid=%s, fired %d time(s), returned %d, vector holds %d",
			info.IsValid() ? "yes" : "no", g_completion_fn_calls - before,
			returned, completions.Count());

		if (g_completion_fn_calls - before != 1)
		{
			context.Fail("the function branch should have been taken once");
		}
		// The function form has no return value of its own, so Dispatch
		// reports the vector's count - which our callback filled with two.
		if (returned != completions.Count() || completions.Count() != 2)
		{
			context.Fail("Dispatch should report the two entries the callback "
			             "appended");
		}
	});

	CS2ST_CALL(context, "CompletionCallbackInfo_t (ICommandCompletionCallback form)", {
		const CompletionCallbackInfo_t info(&g_completion_object);

		CCommand args;
		args.Tokenize("cs2sdktest_cmd ");
		CUtlVector<CUtlString> completions;
		UtlVectorGuard<decltype(completions)> completions_guard(completions);

		const int before = g_completion_interface_calls;
		const int returned = info.Dispatch(args, completions);

		context.Detail("valid=%s, ICommandCompletionCallback::CommandCompletionCallback "
		               "fired %d time(s), returned %d, vector holds %d",
			info.IsValid() ? "yes" : "no",
			g_completion_interface_calls - before, returned, completions.Count());

		if (g_completion_interface_calls - before != 1)
		{
			context.Fail("the interface branch should have been taken once");
		}
		if (returned != 1)
		{
			context.Fail("the interface form returns whatever the callback "
			             "returned, which was the vector count of 1");
		}
	});

	CS2ST_CALL(context, "CompletionCallbackInfo_t (default constructed)", {
		const CompletionCallbackInfo_t info;
		CCommand args;
		args.Tokenize("cs2sdktest_cmd ");
		CUtlVector<CUtlString> completions;
		UtlVectorGuard<decltype(completions)> completions_guard(completions);

		const int returned = info.Dispatch(args, completions);
		context.Detail("valid=%s, returned %d, vector holds %d",
			info.IsValid() ? "yes" : "no", returned, completions.Count());

		if (info.IsValid() || returned != 0 || completions.Count() != 0)
		{
			context.Fail("an empty completion info dispatches to nothing and "
			             "returns 0");
		}
	});

	// The member accessor constructor and SetOwner are called in
	// ExerciseScratchCalls.
}

// -----------------------------------------------------------------------------
//  CCommand
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseCommandTokenizer(TestContext& context)
{
	context.Note("[CCommand] %zu bytes; built here and read by the engine, so "
	             "its layout is an ABI contract in the other direction",
		sizeof(CCommand));

	CS2ST_CALL(context, "CCommand (default constructed)", {
		CCommand command;
		context.Detail("ArgC=%d ArgV=%p ArgS=\"%s\" string=\"%s\"",
			command.ArgC(), static_cast<const void*>(command.ArgV()),
			command.ArgS(), command.GetCommandString());

		if (command.ArgC() != 0)
		{
			context.Fail("a fresh CCommand has no arguments");
		}
		if (command.ArgV() != nullptr)
		{
			context.Fail("ArgV is null while ArgC is zero, by the SDK's own "
			             "inline definition");
		}
	});

	CS2ST_CALL(context, "CCommand::MaxCommandLength / DefaultBreakSet", {
		// DefaultBreakSet forwards to ICvar::GetCharacterSet, so the two have
		// to return the same object - one of the cheapest two-path checks in
		// this suite, and it pins tier1's global against the interface.
		characterset_t* from_command = CCommand::DefaultBreakSet();
		characterset_t* from_icvar = cvar_->GetCharacterSet();

		context.Detail("MaxCommandLength=%d, break set: CCommand=%p ICvar=%p",
			CCommand::MaxCommandLength(), static_cast<void*>(from_command),
			static_cast<void*>(from_icvar));

		if (from_command != from_icvar)
		{
			context.Fail("CCommand::DefaultBreakSet and ICvar::GetCharacterSet "
			             "must be the same object");
		}
	});

	CS2ST_CALL(context, "CCommand::Tokenize (and every accessor it feeds)", {
		CCommand command;
		const bool tokenized = command.Tokenize("cs2sdktest_cmd alpha beta 7");

		context.Detail("-> %s, ArgC=%d, arg0=\"%s\" arg3=\"%s\", "
		               "ArgS=\"%s\", string=\"%s\"",
			tokenized ? "true" : "false", command.ArgC(),
			command.Arg(0), command.Arg(3), command.ArgS(),
			command.GetCommandString());

		if (!tokenized)
		{
			context.Fail("a non-empty command string tokenizes");
			return;
		}
		if (command.ArgC() != 4)
		{
			context.Fail("four space-separated words should give ArgC 4, not %d",
				command.ArgC());
			return;
		}
		if (std::strcmp(command.Arg(0), "cs2sdktest_cmd") != 0
			|| std::strcmp(command.Arg(3), "7") != 0)
		{
			context.Fail("the arguments did not come back in order");
		}
		// operator[] is documented as an alias for Arg.
		if (std::strcmp(command[2], command.Arg(2)) != 0)
		{
			context.Fail("operator[] and Arg disagree");
		}
		// ArgS is everything after the 0th argument.
		if (std::strcmp(command.ArgS(), "alpha beta 7") != 0)
		{
			context.Fail("ArgS should be the arguments after the command name");
		}
	});

	CS2ST_CALL(context, "CCommand::Arg (out of range)", {
		CCommand command;
		command.Tokenize("cs2sdktest_cmd alpha");

		context.Detail("Arg(-1)=\"%s\" Arg(99)=\"%s\"",
			command.Arg(-1), command.Arg(99));

		// The SDK's inline definition returns "" rather than reading past the
		// end, and says so in a comment; a non-empty answer means the bounds
		// check is not the one we compiled against.
		if (command.Arg(-1)[0] != '\0' || command.Arg(99)[0] != '\0')
		{
			context.Fail("an out-of-range argument index must give an empty string");
		}
	});

	CS2ST_CALL(context, "CCommand::FindArg / FindArgInt", {
		CCommand command;
		command.Tokenize("cs2sdktest_cmd alpha beta 7");

		const int alpha = command.FindArg("alpha");
		const int beta = command.FindArg("beta");
		const int missing = command.FindArg("gamma");
		const int beta_int = command.FindArgInt("beta", -1);
		const int missing_int = command.FindArgInt("gamma", -1);

		context.Detail("FindArg alpha=%d beta=%d gamma=%d, "
		               "FindArgInt beta=%d gamma=%d",
			alpha, beta, missing, beta_int, missing_int);

		// FindArg returns the index *after* the match, so "beta" at index 2
		// yields 3, and FindArgInt reads the "7" that sits there.
		if (alpha != 2 || beta != 3)
		{
			context.Fail("FindArg returns the index following the match; expected "
			             "2 and 3");
		}
		if (missing != -1)
		{
			context.Fail("a missing argument gives -1");
		}
		if (beta_int != 7)
		{
			context.Fail("FindArgInt should read the 7 that follows \"beta\"");
		}
		if (missing_int != -1)
		{
			context.Fail("FindArgInt returns the default for a missing argument");
		}
	});

	CS2ST_CALL(context, "CCommand::Tokenize (empty string) / Reset", {
		CCommand command;
		const bool empty = command.Tokenize("");
		command.Tokenize("cs2sdktest_cmd alpha");
		const int before_reset = command.ArgC();
		command.Reset();

		context.Detail("Tokenize(\"\")=%s, ArgC %d -> %d after Reset",
			empty ? "true" : "false", before_reset, command.ArgC());

		if (empty)
		{
			context.Fail("the SDK returns false for an empty command string");
		}
		if (command.ArgC() != 0)
		{
			context.Fail("Reset should empty the argument list");
		}
	});

	CS2ST_CALL(context, "CCommand::Tokenize (with the engine's break set)", {
		CCommand command;
		characterset_t* break_set = cvar_->GetCharacterSet();
		const bool tokenized = command.Tokenize("cs2sdktest_cmd alpha", break_set);

		context.Detail("-> %s, ArgC=%d with the engine's own break set",
			tokenized ? "true" : "false", command.ArgC());

		if (!tokenized || command.ArgC() != 2)
		{
			context.Fail("passing the engine's break set explicitly should give "
			             "the same two arguments as the default");
		}
	});

	CS2ST_CALL(context, "CCommand (argc/argv constructor)", {
		const char** argv = kProbeArgv;
		CCommand command(3, argv);

		context.Detail("ArgC=%d arg2=\"%s\" string=\"%s\"",
			command.ArgC(), command.Arg(2), command.GetCommandString());

		if (command.ArgC() != 3)
		{
			context.Fail("three arguments in, %d out", command.ArgC());
		}
		else if (std::strcmp(command.Arg(2), "with space") != 0)
		{
			context.Fail("an argument containing a space was not preserved");
		}
	});
}

// -----------------------------------------------------------------------------
//  Dispatch
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseCommandDispatch(TestContext& context)
{
	context.Note("[ICvar] dispatch, aimed at this plugin's own probe command");

	CS2ST_CALL(context, "CCommandContext round-trip", {
		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		context.Detail("target=%d slot=%d",
			static_cast<int>(ctx.GetTarget()), ctx.GetPlayerSlot().Get());

		if (ctx.GetTarget() != CT_NO_TARGET || ctx.GetPlayerSlot().Get() != -1)
		{
			context.Fail("the context does not read back what it was built from");
		}
	});

	// The best oracle in the suite. The engine is handed a ConCommandRef by
	// value, a CCommandContext and a CCommand we built, and it has to arrive at
	// a function pointer we registered, with the arguments we tokenized. A
	// counter that moves by exactly one covers the whole path at once.
	CS2ST_CALL(context, "ICvar::DispatchConCommand", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		if (!probe.IsValidRef())
		{
			context.Fail("cs2sdktest_cmd is registered");
			return;
		}

		CCommand args;
		if (!args.Tokenize("cs2sdktest_cmd probe 1"))
		{
			context.Fail("could not tokenize the dispatch arguments");
			return;
		}

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		const int before = g_probe.command_dispatches;

		cvar_->DispatchConCommand(probe, ctx, args);

		const int fired = g_probe.command_dispatches - before;
		context.Detail("-> callback fired %d time(s), saw argc=%d arg1=\"%s\" "
		               "target=%d",
			fired, g_probe.command_argc, g_probe.command_arg1,
			g_probe.command_target);

		if (fired != 1)
		{
			context.Fail("one dispatch should reach the callback exactly once");
			return;
		}
		if (g_probe.command_argc != 3)
		{
			context.Fail("the callback saw ArgC %d; three words were tokenized",
				g_probe.command_argc);
		}
		if (std::strcmp(g_probe.command_arg1, "probe") != 0)
		{
			context.Fail("the callback saw \"%s\" as its first argument, not "
			             "\"probe\"", g_probe.command_arg1);
		}
		if (g_probe.command_target != static_cast<int>(CT_NO_TARGET))
		{
			context.Fail("the callback saw target %d, not the CT_NO_TARGET we "
			             "passed", g_probe.command_target);
		}
	});

	CS2ST_CALL(context, "ConCommandRef::Dispatch (the tier1 wrapper)", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		if (!probe.IsValidRef())
		{
			context.Fail("cs2sdktest_cmd is registered");
			return;
		}

		CCommand args;
		args.Tokenize("cs2sdktest_cmd wrapper");

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		const int before = g_probe.command_dispatches;

		probe.Dispatch(ctx, args);

		const int fired = g_probe.command_dispatches - before;
		context.Detail("-> callback fired %d time(s), arg1=\"%s\"",
			fired, g_probe.command_arg1);

		if (fired != 1)
		{
			context.Fail("the wrapper forwards to ICvar::DispatchConCommand, so "
			             "it should reach the callback once");
		}
	});

	context.SkipCall("ICvar::DispatchConCommand (invalid ConCommandRef)",
		"an out-of-range access index would be used to index the engine's own "
		"command list; the fault guard catches a fault but not the silent "
		"corruption a partial write would leave behind");
}

// -----------------------------------------------------------------------------
//  Completion
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseCompletion(TestContext& context)
{
	context.Note("[ICvar] completion, against a prefix that matches only our own "
	             "callback-less registrations");

	CS2ST_CALL(context, "ICvar::GetCompletionResults", {
		// The prefix is the safety argument. GetCompletionResults invokes the
		// completion callbacks of everything it matches, and a callback filling
		// this vector would allocate CUtlStrings from this module's heap for
		// the caller to free. Matching only cs2sdktest_* - none of which has a
		// completion callback - keeps the call to the vtable slot itself.
		CCommand command;
		if (!command.Tokenize("cs2sdktest_"))
		{
			context.Fail("could not tokenize the completion prefix");
			return;
		}

		CUtlVector<CUtlString> completions;
		UtlVectorGuard<decltype(completions)> completions_guard(completions);
		bool successful = false;
		cvar_->GetCompletionResults(command, completions, &successful);

		context.Detail("-> %d completion(s), successful=%s",
			completions.Count(), successful ? "true" : "false");

		if (completions.Count() != 0)
		{
			context.Fail("nothing matching this prefix has a completion callback, "
			             "so nothing should have been appended");
		}
		// The SDK ties `successful` to FCVAR_VCONSOLE_FUZZY_MATCHING plus an
		// available callback; neither holds here.
		if (successful)
		{
			context.Fail("successful should stay false with no completion "
			             "callbacks available");
		}
	});

	CS2ST_CALL(context, "ICvar::GetCompletionResults (without the successful out-param)", {
		CCommand command;
		command.Tokenize("cs2sdktest_");

		CUtlVector<CUtlString> completions;
		UtlVectorGuard<decltype(completions)> completions_guard(completions);
		cvar_->GetCompletionResults(command, completions, nullptr);
		context.Detail("-> %d completion(s), null out-param accepted",
			completions.Count());
	});

	CS2ST_CALL(context, "ConCommandRef::GetAutoCompleteSuggestions / "
	                    "ConCommandData::GetAutoCompleteSuggestions", {
		ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
		ConCommandData* data = cvar_->GetConCommandData(probe);
		if (data == nullptr)
		{
			context.Fail("no command data for a registered command");
			return;
		}

		CCommand command;
		command.Tokenize("cs2sdktest_cmd ");

		CUtlVector<CUtlString> from_ref;
		UtlVectorGuard<decltype(from_ref)> from_ref_guard(from_ref);
		CUtlVector<CUtlString> from_data;
		UtlVectorGuard<decltype(from_data)> from_data_guard(from_data);
		const int ref_count = probe.GetAutoCompleteSuggestions(command, from_ref);
		const int data_count = data->GetAutoCompleteSuggestions(command, from_data);

		context.Detail("ref -> %d, data -> %d (no completion callback registered)",
			ref_count, data_count);

		// CompletionCallbackInfo_t::Dispatch returns 0 when it holds nothing,
		// which is a declaration-level fact about a command we registered
		// without a completion callback.
		if (ref_count != 0 || data_count != 0)
		{
			context.Fail("a command with no completion callback yields no "
			             "suggestions");
		}
		if (ref_count != data_count)
		{
			context.Fail("the ref and the data disagree");
		}
	});

	// GetCompletionResults against a command that does have a completion
	// callback is in ExerciseScratchCalls.
}

// -----------------------------------------------------------------------------
//  Snapshots
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseSnapshots(TestContext& context)
{
	context.Note("[ICvar] convar snapshots - ConVarSnapshot_t is opaque in the "
	             "SDK, so only the take/destroy pair can be checked");

	CS2ST_CALL(context, "ICvar::TakeConVarSnapshot / ICvar::DestroyConVarSnapshot", {
		// A matched pair, so the process ends where it started. Take allocates
		// inside the engine and Destroy frees it there, which is the one shape
		// of allocation this suite can safely exercise across the boundary.
		ConVarSnapshot_t* snapshot = cvar_->TakeConVarSnapshot();
		context.Detail("-> %p", static_cast<void*>(snapshot));

		if (snapshot == nullptr)
		{
			context.Fail("a snapshot of a populated convar list should not be null");
			return;
		}

		cvar_->DestroyConVarSnapshot(snapshot);
		context.Detail("-> %p, destroyed", static_cast<void*>(snapshot));
	});

	// ResetConVarsToSnapshot is called in ExerciseScratchCalls, straight after
	// a fresh snapshot so there is nothing for it to roll back.
}

// -----------------------------------------------------------------------------
//  Userinfo convars
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseUserInfoCvars(TestContext& context)
{
	context.Note("[ICvar] the FCVAR_USERINFO default block");

	int total_size = 0;

	CS2ST_CALL(context, "ICvar::GetTotalUserInfoCvarsByteSize", {
		total_size = cvar_->GetTotalUserInfoCvarsByteSize();
		context.Detail("-> %d bytes", total_size);
		if (total_size < 0)
		{
			context.Fail("a byte size cannot be negative");
		}
	});

	CS2ST_CALL(context, "ICvar::CopyUserInfoCvarDefaults", {
		if (total_size <= 0)
		{
			context.Detail("nothing to copy (%d bytes reported)", total_size);
			return;
		}

		// A stack buffer, so nothing is allocated and nothing leaks if the call
		// faults halfway through. The range is clamped to what we actually own:
		// the engine writes into this, and `to` is the only thing stopping it.
		//
		// The 2026-10 SDK turned the first parameter from a bare uint8* into a
		// ConVarUserInfoSet_t by value - a one-pointer wrapper class, so the
		// register the callee reads is the same one our buffer's address lands
		// in either way.
		constexpr int kBufferSize = 2048;
		uint8 buffer[kBufferSize] = {};
		const int copy_to = (total_size < kBufferSize) ? total_size : kBufferSize;

		cvar_->CopyUserInfoCvarDefaults(ConVarUserInfoSet_t{buffer}, 0, copy_to, true);

		int nonzero = 0;
		for (int i = 0; i < copy_to; ++i)
		{
			if (buffer[i] != 0)
			{
				++nonzero;
			}
		}
		context.Detail("copied bytes 0..%d of %d, %d nonzero%s",
			copy_to, total_size, nonzero,
			copy_to < total_size ? " (buffer capped)" : "");

		// The cleanup half of the same call, so the buffer is left the way the
		// engine expects rather than half-initialised.
		cvar_->CopyUserInfoCvarDefaults(ConVarUserInfoSet_t{buffer}, 0, copy_to, false);
	});
}

// -----------------------------------------------------------------------------
//  The break set
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseCharacterSet(TestContext& context)
{
	context.Note("[ICvar] the command tokenizer break set - engine memory with a "
	             "very small legal value set per byte");

	CS2ST_CALL(context, "ICvar::GetCharacterSet", {
		characterset_t* set = cvar_->GetCharacterSet();
		context.Detail("-> %p", static_cast<void*>(set));
		if (set == nullptr)
		{
			context.Fail("CCommand::Tokenize dereferences this on every command "
			             "with no break set of its own");
		}
	});

	CS2ST_CALL(context, "characterset_t contents", {
		characterset_t* set = cvar_->GetCharacterSet();
		if (set == nullptr)
		{
			context.Fail("no break set to inspect");
			return;
		}

		// A break set marks separators. Letters and digits are never
		// separators in a console command - a tokenizer that split on them
		// could not parse its own convar names - so this is a property of what
		// the table is, not of how the game is configured.
		int letters_marked = 0;
		for (int c = 'a'; c <= 'z'; ++c)
		{
			if (IN_CHARACTERSET(*set, c) != 0)
			{
				++letters_marked;
			}
		}
		for (int c = '0'; c <= '9'; ++c)
		{
			if (IN_CHARACTERSET(*set, c) != 0)
			{
				++letters_marked;
			}
		}

		int marked = 0;
		for (int c = 0; c < 256; ++c)
		{
			if (set->set[c] != 0)
			{
				++marked;
			}
		}

		context.Detail("%d of 256 bytes marked; quote=%d letters/digits marked=%d",
			marked, IN_CHARACTERSET(*set, '"') != 0 ? 1 : 0, letters_marked);

		if (letters_marked != 0)
		{
			context.Fail("%d alphanumeric character(s) are marked as separators; "
			             "this is probably not the break set", letters_marked);
		}
		if (marked == 0)
		{
			context.Fail("a break set with nothing in it would make Tokenize "
			             "return the whole string as one argument");
		}
	});
}

// -----------------------------------------------------------------------------
//  Struct layouts
// -----------------------------------------------------------------------------

void ConVarSuite::InspectCvarObjectLayout(TestContext& context)
{
	context.Note("[CCvar] the cvar system object itself - %zu bytes as the SDK "
	             "declares it. icvar.h calls this definition \"mostly for "
	             "reference and ... reverse engineered\", which makes it the "
	             "highest-value layout in this suite and the least trustworthy",
		sizeof(CCvar));

	// That cvar_ points at a CCvar is a hypothesis. The SDK names the concrete
	// class but never promises the interface is one, so the layout is measured
	// against answers the interface itself can give - and the sharpest of those
	// goes first: GetCharacterSet returns the address of a member, so if it
	// equals base + offsetof then the object really is shaped as declared.
	CCvar* cvar_object = static_cast<CCvar*>(cvar_);
	bool identity_confirmed = false;

	CS2ST_CALL(context, "CCvar::m_CharacterSet (address vs ICvar::GetCharacterSet)", {
		const void* from_call = cvar_->GetCharacterSet();
		const void* from_layout = reinterpret_cast<const unsigned char*>(cvar_object)
			+ offsetof(CCvar, m_CharacterSet);

		context.Detail("call -> %p, offsetof says %p (+0x%zx)", from_call, from_layout,
			static_cast<std::size_t>(offsetof(CCvar, m_CharacterSet)));

		if (from_call == from_layout)
		{
			identity_confirmed = true;
		}
		else
		{
			context.Fail("the interface returns a different address than the "
			             "declared member offset, so either the object is not a "
			             "CCvar or a field moved ahead of m_CharacterSet");
		}
	});

	// There is no getter for this field, so the second path is the iteration:
	// whatever the number means, it cannot contradict the convars the interface
	// hands out one at a time.
	//
	// Both assertions below are the weakest form that holds under either
	// reading of the field - the length of m_ConVarList, or the counter that
	// ConVarRef::m_ConVarRegisteredIndex is handed out from. The SDK does not
	// say which, so nothing stronger is claimed. Larger than the walk is normal
	// and not flagged: iteration may filter defensive convars.
	//
	// Worth knowing how strong this is: an int has no illegal bit patterns, so
	// only a negative value or one below the walk can falsify the offset. If
	// the field moved and its neighbour happens to hold a plausible larger
	// number, this row passes. It is cumulative evidence, not proof - the
	// m_CharacterSet address check and the m_LockDefaultValueInit toggle are
	// the two rows in this section that can stand alone.
	CS2ST_CALL(context, "CCvar::m_ConVarCount (vs walking the convar list)", {
		int walked = 0;
		for (ConVarRef iter = cvar_->FindFirstConVar();
		     iter.IsValidRef() && walked < kListWalkCap;
		     iter = cvar_->FindNextConVar(iter))
		{
			++walked;
		}

		// Reported, not asserted on. A registered index looks like it comes
		// from this counter, but the SDK never says so, and guessing semantics
		// is how a layout check turns into a false alarm.
		int highest_registered = 0;
		for (std::size_t i = 0; i < kProbeCvarCount; ++i)
		{
			const ConVarRef probe = cvar_->FindConVar(kProbeCvarNames[i], true);
			if (probe.IsValidRef() && probe.GetRegisteredIndex() > highest_registered)
			{
				highest_registered = probe.GetRegisteredIndex();
			}
		}

		const int field = cvar_object->m_ConVarCount;
		context.Detail("field=%d, walk reached %d, highest registered index "
		               "among our own convars=%d", field, walked, highest_registered);

		if (field < 0)
		{
			context.Fail("a registration count cannot be negative");
		}
		else if (field < walked)
		{
			context.Fail("the list handed out %d convars but the count field says "
			             "%d", walked, field);
		}
	});

	CS2ST_CALL(context, "CCvar::m_ConCommandCount (vs walking the command list)", {
		int walked = 0;
		for (ConCommandRef iter = cvar_->FindFirstConCommand();
		     iter.IsValidRef() && walked < kListWalkCap;
		     iter = cvar_->FindNextConCommand(iter))
		{
			++walked;
		}

		const int field = cvar_object->m_ConCommandCount;
		context.Detail("field=%d, walk reached %d", field, walked);

		if (field < 0)
		{
			context.Fail("a registration count cannot be negative");
		}
		else if (field < walked)
		{
			context.Fail("the list handed out %d commands but the count field "
			             "says %d", walked, field);
		}
	});

	CS2ST_CALL(context, "CCvar::m_UserInfoCvarsTotalByteSize (vs the getter)", {
		const int field = cvar_object->m_UserInfoCvarsTotalByteSize;
		const int from_call = cvar_->GetTotalUserInfoCvarsByteSize();
		context.Detail("field=%d, getter=%d", field, from_call);
		if (field != from_call)
		{
			context.Fail("the getter reads this field, so they cannot differ "
			             "unless the offset is wrong");
		}
	});

	// A bool field this suite can drive through the vtable, which makes it the
	// only layout check here with a controlled input rather than an observation.
	CS2ST_CALL(context, "CCvar::m_LockDefaultValueInit (driven through the interface)", {
		const unsigned char before = *(reinterpret_cast<const unsigned char*>(cvar_object)
			+ offsetof(CCvar, m_LockDefaultValueInit));

		cvar_->LockConVarValueInitialisation(true);
		const unsigned char locked = *(reinterpret_cast<const unsigned char*>(cvar_object)
			+ offsetof(CCvar, m_LockDefaultValueInit));

		cvar_->LockConVarValueInitialisation(false);
		const unsigned char unlocked = *(reinterpret_cast<const unsigned char*>(cvar_object)
			+ offsetof(CCvar, m_LockDefaultValueInit));

		context.Detail("raw byte %u -> %u (locked) -> %u (unlocked)",
			static_cast<unsigned>(before), static_cast<unsigned>(locked),
			static_cast<unsigned>(unlocked));

		// Read as a raw byte, so a value outside 0/1 is a layout verdict rather
		// than a state observation.
		if (locked > 1 || unlocked > 1)
		{
			context.Fail("a bool holds 0 or 1; this offset is not that bool");
		}
		else if (locked != 1 || unlocked != 0)
		{
			context.Fail("the field does not follow "
			             "LockConVarValueInitialisation, so it is either a "
			             "different field or the call does not set it");
		}
	});

	// The two listener containers, checked by moving them. Both calls have a
	// proper inverse, so the count has to come back to where it started.
	CS2ST_CALL(context, "CCvar::m_GlobalChangeCBList (count follows install/remove)", {
		const int before = cvar_object->m_GlobalChangeCBList.Count();
		cvar_->InstallGlobalChangeCallback(&ProbeGlobalChanged);
		const int installed = cvar_object->m_GlobalChangeCBList.Count();
		cvar_->RemoveGlobalChangeCallback(&ProbeGlobalChanged);
		const int removed = cvar_object->m_GlobalChangeCBList.Count();

		context.Detail("%d -> %d (installed) -> %d (removed)",
			before, installed, removed);

		if (before < 0 || installed < 0 || removed < 0)
		{
			context.Fail("a container count cannot be negative; this offset is "
			             "not a CUtlVector");
		}
		else if (installed != before + 1 || removed != before)
		{
			context.Fail("installing and removing one callback should move the "
			             "count by exactly one and back");
		}
	});

	CS2ST_CALL(context, "CCvar::m_CvarCreationListeners (count follows register/remove)", {
		const int before = cvar_object->m_CvarCreationListeners.Count();
		cvar_->RegisterCreationListeners(&g_creation_probe);
		const int registered = cvar_object->m_CvarCreationListeners.Count();
		cvar_->RemoveCreationListeners(&g_creation_probe);
		const int removed = cvar_object->m_CvarCreationListeners.Count();

		context.Detail("%d -> %d (registered) -> %d (removed)",
			before, registered, removed);

		if (before < 0 || registered < 0 || removed < 0)
		{
			context.Fail("a container count cannot be negative; this offset is "
			             "not a CUtlVector");
		}
		else if (registered != before + 1 || removed != before)
		{
			context.Fail("registering and removing one listener should move the "
			             "count by exactly one and back");
		}
	});

	context.Note("  identity oracle: %s",
		identity_confirmed
			? "GetCharacterSet agrees with offsetof, so the rows below are "
			  "being read out of a CCvar"
			: "GetCharacterSet disagrees with offsetof - treat every row below "
			  "as unanchored");

	// Three classes on the chain, so up to three passes - base first, because a
	// divergence in a base makes every row after it meaningless.
	//
	// IAppSystem is the exception: it declares no data members at all, so there
	// is nothing to lay out. Its eleven vtable slots are covered as calls in
	// ExerciseAppSystem instead.
	context.Note("  IAppSystem declares no data members; its vtable is covered "
	             "as calls, not as layout");

	// ICvar's own pass. The cast is what names the subobject rather than
	// assuming it starts at the CCvar base, and sizeof(ICvar) keeps the bounds
	// checks honest - this pass reads only bytes ICvar declares.
	StructInspector icvar(context, "ICvar", static_cast<const ICvar*>(cvar_object),
		sizeof(ICvar));

	// Offset 0 is not a guessed field position - it is where the Itanium ABI
	// puts the vtable pointer of a polymorphic class, and ICvar is polymorphic
	// by way of IAppSystem. The vptr row belongs to this pass because ICvar is
	// where the chain first becomes polymorphic. A vtable pointer is also a
	// good subject for the pointer check: canonical, aligned, pointing into a
	// mapped image.
	icvar.Pointer("__vftable (ABI: offset 0)", 0);

	// m_MaxSplitScreenSlots is private, and its offset is captured the same way
	// ConVarData's fields are - so the compiler computes it from the
	// declaration rather than this file assuming "vptr, then the first member".
	// The assumption happened to be right, but it would have gone quietly wrong
	// the day IAppSystem grew a data member.
	const std::size_t max_slots_offset =
		PrivateFieldOffset<ICvarMaxSplitScreenSlots>(cvar_);

	CS2ST_CALL(context, "ICvar::m_MaxSplitScreenSlots (captured offset)", {
		context.Detail("+0x%zx within ICvar, and the ICvar subobject %s the "
		               "CCvar base",
			max_slots_offset,
			static_cast<const void*>(cvar_) == static_cast<const void*>(cvar_object)
				? "coincides with" : "is offset from");

		if (max_slots_offset == static_cast<std::size_t>(-1))
		{
			context.Fail("the pointer-to-member was never captured; the explicit "
			             "instantiation in this file is missing or was optimised "
			             "away");
		}
		else if (static_cast<const void*>(cvar_) != static_cast<const void*>(cvar_object))
		{
			context.Fail("ICvar is no longer the primary base of CCvar, so this "
			             "offset cannot be used against the CCvar base address");
		}
	});

	// The range is the whole falsifier for the value, and it has to be: this
	// field has no independent second path. GetMaxSplitScreenSlots is a
	// non-virtual inline that reads this very offset, so comparing the two
	// would be comparing the declaration against itself, and the only external
	// way to drive the field is SetMaxSplitScreenSlots - which this suite
	// refuses to call because it resizes the value storage of every convar in
	// the process. What is left is that a slot count must be between one and
	// MAX_SPLITSCREEN_CLIENTS, and a misread int rarely lands in a range that
	// narrow.
	icvar.Int32  ("m_MaxSplitScreenSlots (private, captured)",
			  max_slots_offset, 1, MAX_SPLITSCREEN_CLIENTS);

	icvar.Summarize();

	// CCvar's own pass, carrying only what CCvar declares. The containers have
	// no fields this can judge, so they are Raw rows: what they contribute is a
	// fixed position, which is what makes the judged fields after them
	// meaningful.
	StructInspector inspect(context, "CCvar", cvar_object, sizeof(CCvar));
	inspect.Raw    ("m_StringsBuffer", offsetof(CCvar, m_StringsBuffer),
				  sizeof(CUtlVector<char*>));
	inspect.Raw    ("m_MemoryBuffer", offsetof(CCvar, m_MemoryBuffer),
				  sizeof(CUtlVector<uint8*>));

	// Both are byte counts into a fixed-size chunk, so the type bounds them:
	// negative is impossible and so is larger than the chunk they index.
	inspect.Int32  ("m_CurrentStringsBufferSize",
				  offsetof(CCvar, m_CurrentStringsBufferSize),
				  0, CCvar::kStringBufferChunkMaxSize);
	inspect.Int32  ("m_CurrentMemoryBufferSize",
				  offsetof(CCvar, m_CurrentMemoryBufferSize),
				  0, CCvar::kMemoryBufferChunkMaxSize);

	inspect.Raw    ("m_ConVarList", offsetof(CCvar, m_ConVarList), 24);
	inspect.Raw    ("m_ConVarHashes", offsetof(CCvar, m_ConVarHashes), 24);
	inspect.Raw    ("m_ConVarChangeCBList", offsetof(CCvar, m_ConVarChangeCBList), 24);
	inspect.Raw    ("m_ConVarFilterCBList", offsetof(CCvar, m_ConVarFilterCBList), 24);
	inspect.Raw    ("m_ConVarCompletionCBList",
				  offsetof(CCvar, m_ConVarCompletionCBList), 24);
	inspect.Int32  ("m_ConVarCount", offsetof(CCvar, m_ConVarCount), 0, INT32_MAX);

	inspect.Raw    ("m_CvarCreationListeners",
				  offsetof(CCvar, m_CvarCreationListeners),
				  sizeof(CUtlVector<IConVarListener*>));
	inspect.Raw    ("m_GlobalChangeCBList", offsetof(CCvar, m_GlobalChangeCBList),
				  sizeof(CUtlVector<FnChangeCallbackGlobal_t>));

	inspect.Raw    ("m_ConCommandList", offsetof(CCvar, m_ConCommandList), 24);
	inspect.Raw    ("m_ConCommandHashes", offsetof(CCvar, m_ConCommandHashes), 24);
	inspect.Raw    ("m_CallbackInfoList", offsetof(CCvar, m_CallbackInfoList), 24);
	inspect.Int32  ("m_ConCommandCount", offsetof(CCvar, m_ConCommandCount), 0, INT32_MAX);

	inspect.Raw    ("m_Mutex", offsetof(CCvar, m_Mutex), sizeof(CAtomicMutex));

	// The break set gets its own inspector below; this row is here so the
	// reader can see where the 256 bytes start.
	inspect.Raw    ("m_CharacterSet (head)", offsetof(CCvar, m_CharacterSet), 16);
	inspect.Pointer("m_GameInfoKV", offsetof(CCvar, m_GameInfoKV));

	// 2 KB of per-type default value storage; only the head is printed.
	inspect.Raw    ("m_CvarDefaultValues (head of %zu bytes)",
				  offsetof(CCvar, m_CvarDefaultValues), 16);

	inspect.Raw    ("m_SetValueQueue", offsetof(CCvar, m_SetValueQueue),
				  sizeof(CUtlVector<CCvar::QueuedConVarSet_t>));
	inspect.Raw    ("m_SetToDefaultValueQueue",
				  offsetof(CCvar, m_SetToDefaultValueQueue),
				  sizeof(CUtlVector<ConVarRef>));
	inspect.Bool   ("m_LockDefaultValueInit", offsetof(CCvar, m_LockDefaultValueInit));
	inspect.Int32  ("m_UserInfoCvarsTotalByteSize",
				  offsetof(CCvar, m_UserInfoCvarsTotalByteSize), 0, INT32_MAX);

	// The ten built-in commands the cvar system registers for itself. Each is a
	// CConCommandMemberAccessor, which is two vtable pointers plus a
	// ConCommandRef plus three members - so the first eight bytes of each is a
	// vtable pointer, and that is the judged part.
	inspect.Pointer("m_FindCmd (vtable)", offsetof(CCvar, m_FindCmd));
	inspect.Pointer("m_DumpChannelsCmd (vtable)", offsetof(CCvar, m_DumpChannelsCmd));
	inspect.Pointer("m_LogLevelCmd (vtable)", offsetof(CCvar, m_LogLevelCmd));
	inspect.Pointer("m_LogVerbosityCmd (vtable)", offsetof(CCvar, m_LogVerbosityCmd));
	inspect.Pointer("m_LogColorCmd (vtable)", offsetof(CCvar, m_LogColorCmd));
	inspect.Pointer("m_LogFlagsCmd (vtable)", offsetof(CCvar, m_LogFlagsCmd));
	inspect.Pointer("m_DifferencesCmd (vtable)", offsetof(CCvar, m_DifferencesCmd));
	inspect.Pointer("m_CvarListCmd (vtable)", offsetof(CCvar, m_CvarListCmd));
	inspect.Pointer("m_HelpCmd (vtable)", offsetof(CCvar, m_HelpCmd));
	inspect.Pointer("m_FindFlagsCmd (vtable)", offsetof(CCvar, m_FindFlagsCmd));

	inspect.Summarize();
	inspect.HexDump(64, 768);
}

void ConVarSuite::InspectCharacterSetLayout(TestContext& context)
{
	characterset_t* set = cvar_->GetCharacterSet();
	if (set == nullptr)
	{
		context.SkipCall("characterset_t",
			"ICvar::GetCharacterSet returned null, so there is nothing to read");
		return;
	}

	context.Note("[characterset_t] %zu bytes of engine memory - the tokenizer "
	             "break set every console command is split on",
		sizeof(characterset_t));

	// A 256-entry lookup table has no typed fields to falsify, so this is a
	// byte record rather than a set of checks - the semantic check that letters
	// and digits are not separators lives in ExerciseCharacterSet. What the
	// record buys is a diff after a game update.
	StructInspector inspect(context, "characterset_t", set, sizeof(characterset_t));

	inspect.Raw("set[0..15] (control)", offsetof(characterset_t, set), 16);
	inspect.Raw("set[32..47] (space and punctuation)",
		offsetof(characterset_t, set) + 32, 16);
	inspect.Raw("set[48..63] (digits)", offsetof(characterset_t, set) + 48, 16);
	inspect.Raw("set[64..79] (uppercase)", offsetof(characterset_t, set) + 64, 16);
	inspect.Raw("set[96..111] (lowercase)", offsetof(characterset_t, set) + 96, 16);

	inspect.Summarize();
	inspect.HexDump(0, 256);
}

void ConVarSuite::InspectConVarRefLayout(TestContext& context)
{
	context.Note("[ConVarRefAbstract] %zu bytes. Compiled into this plugin, but "
	             "not private to it: ICvar::QueueThreadSetValue takes one of "
	             "these, stores it in the engine, and reads m_ConVarData back "
	             "out later - so the field order is a shared contract",
		sizeof(ConVarRefAbstract));

	CS2ST_CALL(context, "ConVarRefAbstract member offsets (getter vs offsetof)", {
		// The three members are protected, so ConVarRefAbstractProbe is what
		// lets offsetof name them. Each row is a getter answer compared against
		// the bytes at the offset the declaration puts that field at.
		const ConVarRefAbstract* as_base = static_cast<const ConVarRefAbstract*>(&g_probe_int);
		const unsigned char* base = reinterpret_cast<const unsigned char*>(as_base);

		ConVarData* data_from_layout = nullptr;
		std::memcpy(&data_from_layout, base + ConVarRefAbstractProbe::DataOffset(),
			sizeof(data_from_layout));

		uint16 access_from_layout = 0;
		std::memcpy(&access_from_layout,
			base + ConVarRefAbstractProbe::AccessIndexOffset(),
			sizeof(access_from_layout));

		int registered_from_layout = 0;
		std::memcpy(&registered_from_layout,
			base + ConVarRefAbstractProbe::RegisteredIndexOffset(),
			sizeof(registered_from_layout));

		context.Detail("m_ConVarData +0x%zx -> %p (getter %p), "
		               "m_ConVarAccessIndex +0x%zx -> %u (getter %u), "
		               "m_ConVarRegisteredIndex +0x%zx -> %d (getter %d)",
			ConVarRefAbstractProbe::DataOffset(),
			static_cast<void*>(data_from_layout),
			static_cast<void*>(g_probe_int.GetConVarData()),
			ConVarRefAbstractProbe::AccessIndexOffset(),
			static_cast<unsigned>(access_from_layout),
			static_cast<unsigned>(g_probe_int.GetAccessIndex()),
			ConVarRefAbstractProbe::RegisteredIndexOffset(),
			registered_from_layout, g_probe_int.GetRegisteredIndex());

		if (data_from_layout != g_probe_int.GetConVarData())
		{
			context.Fail("GetConVarData and the declared m_ConVarData offset read "
			             "different memory, which cannot happen unless the two "
			             "were compiled against different declarations");
		}
		if (access_from_layout != g_probe_int.GetAccessIndex())
		{
			context.Fail("the access index does not sit where the declaration "
			             "puts it");
		}
		if (registered_from_layout != g_probe_int.GetRegisteredIndex())
		{
			context.Fail("the registered index does not sit where the declaration "
			             "puts it");
		}
	});

	CS2ST_CALL(context, "handle type sizes", {
		context.Detail("ConVarRef=%zu ConVarRefAbstract=%zu (probe %zu) "
		               "ConCommandRef=%zu ConCommand=%zu",
			sizeof(ConVarRef), sizeof(ConVarRefAbstract),
			sizeof(ConVarRefAbstractProbe), sizeof(ConCommandRef),
			sizeof(ConCommand));

		if (sizeof(ConVarRefAbstractProbe) != sizeof(ConVarRefAbstract))
		{
			context.Fail("the probe grew the type, so its offsets are not the "
			             "real ones");
		}
		// ConVarRefAbstract adds one pointer to ConVarRef, and ConCommand adds
		// nothing at all to ConCommandRef - both are facts of the declaration.
		if (sizeof(ConVarRefAbstract) != sizeof(ConVarRef) + sizeof(ConVarData*))
		{
			context.Fail("ConVarRefAbstract should be ConVarRef plus one pointer");
		}
		if (sizeof(ConCommand) != sizeof(ConCommandRef))
		{
			context.Fail("ConCommand declares no members of its own");
		}
	});

	// The rows below add one thing the oracles above cannot: a plausibility
	// verdict on the ConVarData pointer, and a hex record of the 16 bytes a
	// plugin hands the engine whenever it queues a threaded set.
	//
	// Two passes, because ConVarRef declares the two index fields and
	// ConVarRefAbstract only adds the data pointer. Splitting them is also what
	// gets ConVarRef into the struct-coverage registry: it is the type every
	// engine call returns by value, so a future suite has good reason to look
	// it up.
	StructInspector base(context, "ConVarRef",
		static_cast<const ConVarRef*>(&g_probe_int), sizeof(ConVarRef));

	// Neither index has a range the type implies - 0xFFFF is the documented
	// "invalid" access index, so even that is legal - which is why they are
	// reported rather than judged. Offsets come from the probe, not a number.
	base.Raw    ("m_ConVarAccessIndex",
		ConVarRefAbstractProbe::AccessIndexOffset(), sizeof(uint16));
	base.Int32  ("m_ConVarRegisteredIndex",
		ConVarRefAbstractProbe::RegisteredIndexOffset());
	base.Summarize();

	StructInspector inspect(context, "ConVarRefAbstract",
		static_cast<const ConVarRefAbstract*>(&g_probe_int),
		sizeof(ConVarRefAbstract));
	inspect.Pointer("m_ConVarData", ConVarRefAbstractProbe::DataOffset());

	inspect.Summarize();
	inspect.HexDump(16, 64);
}

void ConVarSuite::InspectConCommandDataLayout(TestContext& context)
{
	context.Note("[ConCommandData] struct layout - %zu bytes as the SDK declares "
	             "it, allocated by tier0's CCvar", sizeof(ConCommandData));

	ConCommandRef probe = cvar_->FindConCommand("cs2sdktest_cmd", false);
	ConCommandData* data = cvar_->GetConCommandData(probe);
	if (data == nullptr || !probe.IsValidRef())
	{
		context.SkipCall("ConCommandData",
			"cs2sdktest_cmd has no command data to inspect");
		return;
	}

	StructInspector inspect(context, "ConCommandData", data, sizeof(ConCommandData));

	// Declaration order, offsets from offsetof. Every field here is public, so
	// unlike ConVarData below this one can be addressed by name.
	inspect.String ("m_pszName",       offsetof(ConCommandData, m_pszName));
	inspect.String ("m_pszHelpString", offsetof(ConCommandData, m_pszHelpString));
	inspect.UInt64 ("m_nFlags",        offsetof(ConCommandData, m_nFlags));

	// CompletionCallbackInfo_t is a union of two pointers plus a bool. Neither
	// is set for this command, so the pointer half should read as null - which
	// is still a check: a misaligned read here would almost certainly not be.
	inspect.Pointer("m_CompletionCB.callback",
	                offsetof(ConCommandData, m_CompletionCB)
	                    + offsetof(CompletionCallbackInfo_t, m_fnCompletionCallback));
	inspect.Bool   ("m_CompletionCB.m_bIsInterface",
	                offsetof(ConCommandData, m_CompletionCB)
	                    + offsetof(CompletionCallbackInfo_t, m_bIsInterface));

	// No range on either index: "no callback" is encoded as zero here, but the
	// SDK does not say a registered index cannot be negative, and inventing a
	// bound would make this check about semantics rather than layout.
	inspect.Int32  ("m_CompletionCBCmdIndex",
	                offsetof(ConCommandData, m_CompletionCBCmdIndex));
	inspect.Raw    ("m_CallbackInfoIndex",
	                offsetof(ConCommandData, m_CallbackInfoIndex), sizeof(uint16));

	inspect.Summarize();
	inspect.HexDump();
}

void ConVarSuite::InspectConVarDataLayout(TestContext& context)
{
	ConVarData* data = g_probe_int.GetConVarData();
	if (data == nullptr)
	{
		context.SkipCall("ConVarData layout", "cs2sdktest_int has no ConVarData");
		return;
	}

	context.Note("[ConVarData] struct layout - %zu bytes as the SDK declares it",
		sizeof(ConVarData));

	// Every data member of ConVarData is private, which for a long time meant
	// this struct could only be checked through its getters. It no longer does:
	// the offsets below are captured with CS2ST_PRIVATE_FIELD, which reaches a
	// private member through the explicit-instantiation access hole and still
	// derives the offset from the header declaration. testkit/PrivateField.h
	// has the reasoning.
	//
	// m_Values is the one field left out - it is an array, and its address is
	// what Value() returns anyway, so it is derived from the accessor instead.
	context.Note("  all sixteen fields are private; offsets below come from "
	             "captured pointers-to-member, not from written-down numbers");

	// A second, completely independent way to find the same five offsets: each
	// of those fields has a getter, so its value is known, and the block can be
	// searched for it. That makes this a cross-check rather than a fallback -
	// the captured pointers-to-member say where the declaration puts each
	// field, this says where the value actually is, and the two have to agree.
	// The declaration also says the five sit consecutively, so the located
	// offsets must come out eight bytes apart in order.
	CS2ST_CALL(context, "ConVarData pointer chain (located by value, vs captured "
	                    "offsets)", {
		const unsigned char* base = reinterpret_cast<const unsigned char*>(data);

		std::size_t declared[kConVarDataPointerCount];
		declared[0] = PrivateFieldOffset<ConVarDataName>(data);
		declared[1] = PrivateFieldOffset<ConVarDataDefaultValue>(data);
		declared[2] = PrivateFieldOffset<ConVarDataMinValue>(data);
		declared[3] = PrivateFieldOffset<ConVarDataMaxValue>(data);
		declared[4] = PrivateFieldOffset<ConVarDataHelpString>(data);

		const void* wanted[kConVarDataPointerCount];
		wanted[0] = static_cast<const void*>(data->GetName());
		wanted[1] = static_cast<const void*>(data->DefaultValue());
		wanted[2] = static_cast<const void*>(data->MinValue());
		wanted[3] = static_cast<const void*>(data->MaxValue());
		wanted[4] = static_cast<const void*>(data->GetHelpText());

		std::size_t found[kConVarDataPointerCount];
		int unlocated = 0;

		for (std::size_t i = 0; i < kConVarDataPointerCount; ++i)
		{
			found[i] = SIZE_MAX;
			if (wanted[i] == nullptr)
			{
				++unlocated;
				continue;
			}
			for (std::size_t off = 0;
			     off + sizeof(void*) <= sizeof(ConVarData);
			     off += sizeof(void*))
			{
				const void* candidate = nullptr;
				std::memcpy(&candidate, base + off, sizeof(candidate));
				if (candidate == wanted[i])
				{
					found[i] = off;
					break;
				}
			}
			if (found[i] == SIZE_MAX)
			{
				++unlocated;
			}
		}

		for (std::size_t i = 0; i < kConVarDataPointerCount; ++i)
		{
			if (found[i] == SIZE_MAX)
			{
				context.Note("  %-16s not located (value %p)",
					kConVarDataPointerFields[i], wanted[i]);
			}
			else
			{
				context.Note("  %-16s located +0x%zx, declaration says +0x%zx%s",
					kConVarDataPointerFields[i], found[i], declared[i],
					found[i] == declared[i] ? "" : "  <-- disagree");
			}
		}

		int disagreements = 0;
		for (std::size_t i = 0; i < kConVarDataPointerCount; ++i)
		{
			if (found[i] != SIZE_MAX && found[i] != declared[i])
			{
				++disagreements;
			}
		}

		int gaps = 0;
		for (std::size_t i = 1; i < kConVarDataPointerCount; ++i)
		{
			if (found[i] == SIZE_MAX || found[i - 1] == SIZE_MAX)
			{
				continue;
			}
			if (found[i] != found[i - 1] + sizeof(void*))
			{
				++gaps;
			}
		}

		context.Detail("%zu of %zu located, %d disagree with the declaration, "
		               "%d break the declared spacing",
			kConVarDataPointerCount - static_cast<std::size_t>(unlocated),
			kConVarDataPointerCount, disagreements, gaps);

		if (disagreements > 0)
		{
			// The sharpest verdict this struct can produce: the field is not
			// where the header says it is. Both sides are measurements of the
			// same live block, so there is no third explanation.
			context.Fail("%d field(s) hold their value at an offset other than the "
			             "one the SDK declaration puts them at", disagreements);
		}

		if (unlocated > 0)
		{
			// cs2sdktest_int has a name, a default, a min, a max and a help
			// string, so all five are non-null - a value that cannot be found in
			// the block means the getter is not reading this block.
			context.Fail("%d of the five declared pointers is not anywhere in the "
			             "%zu bytes the SDK declares", unlocated, sizeof(ConVarData));
		}
		if (gaps > 0)
		{
			context.Fail("the five pointers are declared consecutively; %d gap(s) "
			             "means something was inserted between them", gaps);
		}
	});

	// The flag word, located the same way. It has to land after the pointer
	// chain, which is the one ordering fact worth asserting about it.
	CS2ST_CALL(context, "ConVarData::m_nFlags (offset located by value)", {
		const unsigned char* base = reinterpret_cast<const unsigned char*>(data);
		const uint64 wanted = data->GetFlags();
		std::size_t found = SIZE_MAX;

		for (std::size_t off = 0; off + sizeof(uint64) <= sizeof(ConVarData);
		     off += sizeof(uint64))
		{
			uint64 candidate = 0;
			std::memcpy(&candidate, base + off, sizeof(candidate));
			if (candidate == wanted)
			{
				found = off;
				break;
			}
		}

		if (found == SIZE_MAX)
		{
			context.Detail("flags 0x%llx not found as an aligned 64-bit field",
				static_cast<unsigned long long>(wanted));
			context.Suspect("the flag word may be unaligned or packed with the "
			                "fields around it");
			return;
		}

		context.Detail("flags 0x%llx at +0x%zx",
			static_cast<unsigned long long>(wanted), found);
	});

	// Before any row: if the capture mechanism ever stops working - a compiler
	// that closes the explicit-instantiation hole, a dropped static initialiser
	// - every offset comes back as SIZE_MAX and the rows below would be
	// garbage. That has to read as a failure, not as a clean struct.
	CS2ST_CALL(context, "ConVarData captured offsets are usable", {
		std::size_t offsets[kConVarDataFieldCount];
		const std::size_t count =
			CollectConVarDataOffsets(data, offsets, kConVarDataFieldCount);

		if (count != kConVarDataFieldCount)
		{
			context.Fail("the offset collector wrote %zu of %zu fields", count,
				kConVarDataFieldCount);
			return;
		}

		int uncaptured = 0;
		int out_of_bounds = 0;
		std::size_t highest = 0;
		for (std::size_t i = 0; i < count; ++i)
		{
			if (offsets[i] == static_cast<std::size_t>(-1))
			{
				++uncaptured;
				continue;
			}
			if (offsets[i] >= sizeof(ConVarData))
			{
				++out_of_bounds;
			}
			if (offsets[i] > highest)
			{
				highest = offsets[i];
			}
		}

		context.Detail("%zu fields captured, highest offset +0x%zx of %zu bytes",
			count - static_cast<std::size_t>(uncaptured), highest, sizeof(ConVarData));

		if (uncaptured > 0)
		{
			context.Fail("%d pointer-to-member capture(s) never ran; every row "
			             "below is meaningless", uncaptured);
		}
		if (out_of_bounds > 0)
		{
			context.Fail("%d captured offset(s) land past sizeof(ConVarData)",
				out_of_bounds);
		}
	});

	StructInspector inspect(context, "ConVarData", data, sizeof(ConVarData));

	// Declaration order, one row per private field.
	inspect.String ("m_pszName", PrivateFieldOffset<ConVarDataName>(data));
	inspect.Pointer("m_defaultValue", PrivateFieldOffset<ConVarDataDefaultValue>(data));
	inspect.Pointer("m_minValue", PrivateFieldOffset<ConVarDataMinValue>(data));
	inspect.Pointer("m_maxValue", PrivateFieldOffset<ConVarDataMaxValue>(data));
	inspect.String ("m_pszHelpString", PrivateFieldOffset<ConVarDataHelpString>(data));

	// EConVarType is int16_t, and StructInspector::Enum reads four bytes, so a
	// Raw row is the honest one here. The type is checked for real elsewhere:
	// GetType has to come back as the type our template argument declared,
	// which is a stronger statement than any range check.
	inspect.Raw    ("m_eVarType", PrivateFieldOffset<ConVarDataVarType>(data),
				  sizeof(EConVarType));
	inspect.Raw    ("m_Version", PrivateFieldOffset<ConVarDataVersion>(data),
				  sizeof(short));

	// A change counter and three list indices. All unsigned, so there is no
	// range to check - reported, and their value is that they hold a fixed
	// position between the fields that can be judged.
	inspect.UInt32 ("m_iTimesChanged", PrivateFieldOffset<ConVarDataTimesChanged>(data));
	inspect.UInt64 ("m_nFlags", PrivateFieldOffset<ConVarDataFlags>(data));
	inspect.UInt32 ("m_iCallbackIndex", PrivateFieldOffset<ConVarDataCallbackIndex>(data));
	inspect.UInt32 ("m_iFilterCBIndex", PrivateFieldOffset<ConVarDataFilterCBIndex>(data));
	inspect.UInt32 ("m_iCompletionCBIndex",
				  PrivateFieldOffset<ConVarDataComplCBIndex>(data));
	inspect.Int32  ("m_GameInfoFlags", PrivateFieldOffset<ConVarDataGameInfoFlags>(data));
	inspect.Int32  ("m_UserInfoByteIndex",
				  PrivateFieldOffset<ConVarDataUserInfoIndex>(data));
	inspect.Pointer("m_fnGetEnumInfoHandle", PrivateFieldOffset<ConVarDataEnumInfoFn>(data));

	CVValue_t* slot0 = data->Value(0);
	if (slot0 == nullptr)
	{
		context.SkipCall("ConVarData::m_Values",
			"Value(0) returned null, so its offset cannot be derived");
	}
	else
	{
		const std::size_t values_offset =
			static_cast<std::size_t>(reinterpret_cast<const unsigned char*>(slot0)
				- reinterpret_cast<const unsigned char*>(data));

		CS2ST_CALL(context, "ConVarData::m_Values (offset derived from Value(0))", {
			context.Detail("+0x%zx, %zu bytes of declared storage after it",
				values_offset,
				values_offset < sizeof(ConVarData)
					? sizeof(ConVarData) - values_offset : 0u);

			if (values_offset >= sizeof(ConVarData))
			{
				context.Fail("the value storage starts at or past the end of the "
				             "struct the SDK declares");
			}
			else if (sizeof(ConVarData) - values_offset
				< sizeof(CVValue_t) * MAX_SPLITSCREEN_CLIENTS)
			{
				context.Fail("there is not room for %d slots of %zu bytes after "
				             "the derived offset", MAX_SPLITSCREEN_CLIENTS,
					sizeof(CVValue_t));
			}
			else if ((values_offset % alignof(CVValue_t)) != 0)
			{
				context.Fail("the value storage is not aligned for CVValue_t");
			}
		});

		if (values_offset < sizeof(ConVarData))
		{
			inspect.Raw("m_Values (derived)", values_offset,
				sizeof(CVValue_t) < sizeof(ConVarData) - values_offset
					? sizeof(CVValue_t) : sizeof(ConVarData) - values_offset);
		}
	}

	inspect.Summarize();

	// The bytes matter more here than anywhere else in this suite, because the
	// per-field rows that would normally point at the first divergence cannot
	// be written. A reader comparing two builds has the dump and the getter
	// results, and between them the boundary is findable.
	inspect.HexDump();
}

// -----------------------------------------------------------------------------
//  The type traits table
// -----------------------------------------------------------------------------

void ConVarSuite::NoteTypeTraitsTable(TestContext& context)
{
	context.Note("[CVarTypeTraits] the per-type table, printed for comparison "
	             "rather than checked");

	// GetCvarTypeTraits is a static function in convar.h, so this table is a
	// copy that belongs to this translation unit - not the engine's. Checking
	// it against itself would prove nothing, and there is no route to the
	// engine's copy. What makes it worth printing is that a convar's behaviour
	// is decided by whichever copy is running: if EConVarType is ever
	// renumbered, our table and the engine's stop agreeing about what type 7
	// is, and this listing is the record of what we thought.
	CS2ST_CALL(context, "CVarTypeTraits (this module's copy of the table)", {
		int inconsistent = 0;

		for (int i = 0; i < EConVarType_MAX; ++i)
		{
			const EConVarType type = static_cast<EConVarType>(i);
			CVarTypeTraits* traits = GetCvarTypeTraits(type);
			if (traits == nullptr)
			{
				++inconsistent;
				continue;
			}

			context.Note("  %-2d %-9s %2d bytes  primitive=%-3s default=\"%s\"",
				i, ConVarTypeName(type), traits->m_ByteSize,
				traits->m_IsPrimitive ? "yes" : "no",
				traits->m_DefaultValue != nullptr ? traits->m_DefaultValue : "");

			// The table names each type as well as sizing it, so a row whose
			// name does not match the enumerator it is indexed by means the
			// table and the enum have drifted apart inside this very build.
			if (traits->m_TypeName == nullptr
				|| std::strcmp(traits->m_TypeName, ConVarTypeName(type)) != 0)
			{
				++inconsistent;
			}
		}

		context.Detail("%d type(s), %d row(s) whose name does not match its "
		               "enumerator", static_cast<int>(EConVarType_MAX), inconsistent);

		if (inconsistent > 0)
		{
			context.Fail("the traits table and EConVarType disagree in this build");
		}
	});

	context.Note("  CVValue_t is %zu bytes, ConVarData %zu, ConCommandData %zu, "
	             "ConVarRef %zu, ConCommandRef %zu, CCommand %zu",
		sizeof(CVValue_t), sizeof(ConVarData), sizeof(ConCommandData),
		sizeof(ConVarRef), sizeof(ConCommandRef), sizeof(CCommand));
}

// -----------------------------------------------------------------------------
//  Scratch calls - what the probes could not take
// -----------------------------------------------------------------------------

void ConVarSuite::ExerciseScratchCalls(TestContext& context)
{
	context.Note("[scratch] calls that rewrite a declared shape or a registration, "
	             "made against cstmp_* objects so the probes keep the shape every "
	             "oracle above assumed");

	// Decided once, before any row runs, so the constructors and the matching
	// destructors agree on whether this sweep is the one that builds them.
	const bool build_runtime = !g_runtime_fixtures_spent;
	g_runtime_fixtures_spent = true;

	ConVarData* data = g_scratch_int.GetConVarData();
	if (data == nullptr)
	{
		context.SkipCall("ConVarData default/min/max mutators",
			"cstmp_scratch_int has no ConVarData");
	}
	else
	{
		CS2ST_CALL(context, "ConVarData::UpdateDefaultValueString / UpdateMinValueString / "
		                    "UpdateMaxValueString", {
			const bool default_ok = data->UpdateDefaultValueString("60");
			const bool min_ok = data->UpdateMinValueString("5");
			const bool max_ok = data->UpdateMaxValueString("95");
			const int32 got_default = g_scratch_int.GetDefault();
			const int32 got_min = g_scratch_int.GetMin();
			const int32 got_max = g_scratch_int.GetMax();

			data->UpdateDefaultValueString("50");
			data->UpdateMinValueString("0");
			data->UpdateMaxValueString("100");
			const bool restored = g_scratch_int.GetDefault() == 50
				&& g_scratch_int.GetMin() == 0 && g_scratch_int.GetMax() == 100;

			context.Detail("default 50 -> %d, min 0 -> %d, max 100 -> %d; restored=%s",
				got_default, got_min, got_max, restored ? "yes" : "no");

			if (!default_ok || !min_ok || !max_ok)
			{
				context.Fail("an Update*String call on a value that exists returned "
				             "false (default=%d min=%d max=%d)",
					default_ok ? 1 : 0, min_ok ? 1 : 0, max_ok ? 1 : 0);
			}
			if (got_default != 60 || got_min != 5 || got_max != 95)
			{
				context.Fail("the strings 60/5/95 did not land in the default, min "
				             "and max (read back %d/%d/%d)", got_default, got_min, got_max);
			}
			if (!restored)
			{
				context.Fail("writing the declared 50/0/100 back did not restore them");
			}
		});

		CS2ST_CALL(context, "ConVarData::SetDefaultValue", {
			CVValue_t replacement(static_cast<int32>(60));
			data->SetDefaultValue(&replacement);
			const int32 changed = g_scratch_int.GetDefault();

			// Put back through UpdateDefaultValueString so the restore does not
			// depend on the call under test.
			const bool restored_ok = data->UpdateDefaultValueString("50");
			const int32 restored = g_scratch_int.GetDefault();

			context.Detail("default 50 -> %d -> %d", changed, restored);
			if (changed != 60)
			{
				context.Fail("SetDefaultValue(&60) left the default at %d", changed);
			}
			if (!restored_ok || restored != 50)
			{
				context.Fail("writing 50 back with UpdateDefaultValueString left the "
				             "default at %d", restored);
			}
		});

		CS2ST_CALL(context, "ConVarData::SetMinValue / SetMaxValue", {
			// The pointers are stored, not copied: the engine's own bounds are
			// kept aside and handed back, so nothing is orphaned.
			CVValue_t* original_min = data->MinValue();
			CVValue_t* original_max = data->MaxValue();

			data->SetMinValue(&g_scratch_min_value);
			data->SetMaxValue(&g_scratch_max_value);
			const int32 got_min = g_scratch_int.GetMin();
			const int32 got_max = g_scratch_int.GetMax();
			const bool swapped = data->MinValue() == &g_scratch_min_value
				&& data->MaxValue() == &g_scratch_max_value;

			data->SetMinValue(original_min);
			data->SetMaxValue(original_max);
			const bool restored = data->MinValue() == original_min
				&& data->MaxValue() == original_max
				&& g_scratch_int.GetMin() == 0 && g_scratch_int.GetMax() == 100;

			context.Detail("min 0 -> %d, max 100 -> %d, pointers swapped=%s, restored=%s",
				got_min, got_max, swapped ? "yes" : "no", restored ? "yes" : "no");

			if (!swapped)
			{
				context.Fail("the stored min/max pointers are not the ones handed in");
			}
			if (got_min != 10 || got_max != 90)
			{
				context.Fail("the caller-owned bounds read back as %d/%d, not 10/90",
					got_min, got_max);
			}
			if (!restored)
			{
				context.Fail("handing the engine's own bounds back did not restore 0/100");
			}
		});

		CS2ST_CALL(context, "ConVarData::RemoveMinValue / RemoveMaxValue / RemoveDefaultValue", {
			CVValue_t* original_min = data->MinValue();
			CVValue_t* original_max = data->MaxValue();

			data->RemoveMinValue();
			data->RemoveMaxValue();
			const bool min_gone = !data->HasMinValue();
			const bool max_gone = !data->HasMaxValue();
			// Put the engine's bounds back straight away: Remove* only drops the
			// pointer, so leaving it dropped would leak the allocation.
			data->SetMinValue(original_min);
			data->SetMaxValue(original_max);

			data->RemoveDefaultValue();
			const int32 after_remove = g_scratch_int.GetDefault();
			// Not SetDefaultValue: see the row above for why it cannot restore.
			data->UpdateDefaultValueString("50");

			const bool restored = data->HasMinValue() && data->HasMaxValue()
				&& g_scratch_int.GetDefault() == 50
				&& g_scratch_int.GetMin() == 0 && g_scratch_int.GetMax() == 100;

			context.Detail("min gone=%s, max gone=%s, default 50 -> %d after remove; "
			               "restored=%s",
				min_gone ? "yes" : "no", max_gone ? "yes" : "no", after_remove,
				restored ? "yes" : "no");

			if (!min_gone || !max_gone)
			{
				context.Fail("Remove*Value left HasMinValue/HasMaxValue true");
			}
			if (after_remove == 50)
			{
				context.Fail("RemoveDefaultValue should reset the default to the "
				             "per-type global one, but it still reads 50");
			}
			if (!restored)
			{
				context.Fail("the declared 50/0/100 were not restored after removal");
			}
		});
	}

	CS2ST_CALL(context, "ConVarValueInfo_t::SetCompletionCallback", {
		// Only the struct is touched: the callback is stored, never dispatched,
		// so nothing is handed a vector this module could allocate into.
		ConVarValueInfo_t info(TranslateConVarType<int32>());
		const bool before = info.m_CompletionCallBack.IsValid();
		info.SetCompletionCallback(CompletionCallbackInfo_t(ScratchCompletionFn));
		const bool after = info.m_CompletionCallBack.IsValid();
		const bool same_fn = info.m_CompletionCallBack.m_fnCompletionCallback == ScratchCompletionFn;

		context.Detail("valid %s -> %s, function pointer kept=%s",
			before ? "yes" : "no", after ? "yes" : "no", same_fn ? "yes" : "no");
		if (before)
		{
			context.Fail("a fresh ConVarValueInfo_t already holds a completion callback");
		}
		if (!after || !same_fn)
		{
			context.Fail("SetCompletionCallback did not store the callback it was given");
		}
	});

	CS2ST_CALL(context, "ICvar::CallGlobalChangeCallbacks", {
		// Called straight at the engine, not through a write: every other
		// plugin's global callback hears a change that did not happen, which is
		// harmless here and is the only way to reach the slot with arguments of
		// our choosing.
		const int before = g_probe.probe_global_changes;
		cvar_->InstallGlobalChangeCallback(&ProbeGlobalChanged);
		cvar_->CallGlobalChangeCallbacks(&g_scratch_int, CSplitScreenSlot(0), "51", "50", nullptr);
		const int seen = g_probe.probe_global_changes - before;
		cvar_->RemoveGlobalChangeCallback(&ProbeGlobalChanged);

		context.Detail("-> %d call(s), name=\"%s\" new=\"%s\"", seen,
			g_probe.probe_global_name, g_probe.probe_global_new);
		if (seen != 1)
		{
			context.Fail("one direct call should reach the installed global callback "
			             "once, it was reached %d time(s)", seen);
		}
		else if (std::strcmp(g_probe.probe_global_name, "cstmp_scratch_int") != 0
			|| std::strcmp(g_probe.probe_global_new, "51") != 0)
		{
			context.Fail("the callback was handed \"%s\" / \"%s\", not the convar "
			             "and value passed in", g_probe.probe_global_name,
				g_probe.probe_global_new);
		}
		if (g_scratch_int.Get() != 50)
		{
			context.Fail("the notification must not write the value, but it is now %d",
				g_scratch_int.Get());
		}
	});

	CS2ST_CALL(context, "ICvar::ResetConVarsToDefaultValuesByFlag", {
		// FCVAR_UNLOGGED is carried by the scratch convar and by no probe, so
		// the reset reaches ours only where it should. Engine convars that carry
		// it are reset too, which changes nothing any row reads.
		g_scratch_int.SetInt(77);
		const bool dirty_before = !g_scratch_int.IsSetToDefault();

		cvar_->ResetConVarsToDefaultValuesByFlag(FCVAR_UNLOGGED);

		const int32 value = g_scratch_int.Get();
		context.Detail("dirty=%s, after reset value=%d set-to-default=%s",
			dirty_before ? "yes" : "no", value,
			g_scratch_int.IsSetToDefault() ? "yes" : "no");

		if (!dirty_before)
		{
			context.Fail("the convar did not take the value 77, so the reset proves "
			             "nothing");
		}
		else if (value != 50)
		{
			context.Fail("the flag matched this convar but its value is %d, not the "
			             "declared default 50", value);
		}
		g_scratch_int.Revert();
	});

	CS2ST_CALL(context, "ICvar::SetMaxSplitScreenSlots (the current value)", {
		// The same number back: this reaches the slot without changing what
		// every convar sized its value storage against.
		const int current = cvar_->GetMaxSplitScreenSlots();
		cvar_->SetMaxSplitScreenSlots(current);
		const int after = cvar_->GetMaxSplitScreenSlots();

		context.Detail("%d -> %d", current, after);
		if (after != current)
		{
			context.Fail("setting the slot count to its own value changed it to %d", after);
		}
	});

	CS2ST_CALL(context, "ICvar::ResetConVarsToSnapshot (fresh snapshot, nothing changed)", {
		// Taken and applied back to back, so the values it restores are the
		// ones already there: the call is reached, and nothing is rolled back.
		ConVarSnapshot_t* snapshot = cvar_->TakeConVarSnapshot();
		if (snapshot == nullptr)
		{
			context.Fail("a snapshot of a populated convar list should not be null");
			return;
		}

		const int32 probe_before = g_probe_int.Get();
		const int32 scratch_before = g_scratch_int.Get();

		cvar_->ResetConVarsToSnapshot(snapshot);
		cvar_->DestroyConVarSnapshot(snapshot);

		const int32 probe_after = g_probe_int.Get();
		const int32 scratch_after = g_scratch_int.Get();

		context.Detail("cs2sdktest_int %d -> %d, cstmp_scratch_int %d -> %d",
			probe_before, probe_after, scratch_before, scratch_after);
		if (probe_before != probe_after || scratch_before != scratch_after)
		{
			context.Fail("resetting to a snapshot taken a moment earlier moved a value");
		}
	});

	CS2ST_CALL(context, "ICvar::GetCompletionResults (a command with a completion callback)", {
		// The callback adds nothing, so the vector the console owns stays empty
		// and no allocation crosses the module boundary. What this reaches is
		// the dispatch to a callback that exists; the contents are not checked
		// because there are none to check.
		CCommand command;
		if (!command.Tokenize("cstmp_scratch_cmd"))
		{
			context.Fail("could not tokenize the completion prefix");
			return;
		}

		CUtlVector<CUtlString> completions;
		UtlVectorGuard<decltype(completions)> completions_guard(completions);
		bool successful = false;
		const int before = g_scratch_completion_calls;
		cvar_->GetCompletionResults(command, completions, &successful);

		context.Detail("-> %d completion(s), callback fired %d time(s), successful=%s",
			completions.Count(), g_scratch_completion_calls - before,
			successful ? "true" : "false");

		if (completions.Count() != 0)
		{
			context.Suspect("the only completion callback in range appends nothing, "
			                "yet the vector holds %d entr%s", completions.Count(),
				completions.Count() == 1 ? "y" : "ies");
		}
	});

	// ---- registrations built and torn down in static storage -----------------

	CS2ST_CALL(context, "CConVar<T>::CConVar (built after load)", {
		if (!build_runtime)
		{
			context.SkipCall("CConVar<T>::CConVar",
				"the runtime fixtures were already built in this load, and a "
				"registration is permanent");
			return;
		}

		g_runtime_cvar = new (g_runtime_cvar_storage) CConVar<int32>(
			"cstmp_runtime_int", kScratchFlags,
			"cstmp: int32 convar constructed after load, then destroyed", 5);

		const ConVarRef found = cvar_->FindConVar("cstmp_runtime_int", false);
		context.Detail("valid=%s, engine finds it=%s, default=%d",
			g_runtime_cvar->IsValidRef() ? "yes" : "no", found.IsValidRef() ? "yes" : "no",
			g_runtime_cvar->IsConVarDataValid() ? static_cast<int>(g_runtime_cvar->GetDefault()) : -1);

		if (!g_runtime_cvar->IsValidRef())
		{
			context.Fail("a CConVar built after load should register at once and "
			             "hold a valid ref");
		}
		if (!found.IsValidRef())
		{
			context.Fail("the engine cannot find a convar the constructor registered");
		}
		if (g_runtime_cvar->IsConVarDataValid() && g_runtime_cvar->GetDefault() != 5)
		{
			context.Fail("the default passed to the constructor did not survive");
		}
	});

	CS2ST_CALL(context, "CConVar<T>::~CConVar", {
		if (g_runtime_cvar == nullptr)
		{
			context.SkipCall("CConVar<T>::~CConVar", "no runtime CConVar was built in this sweep");
			return;
		}

		g_runtime_cvar->~CConVar();
		// The object is trivially laid out static storage and the destructor
		// only unregisters callbacks and invalidates the ref, so reading it
		// back is how the invalidation is observed.
		const bool invalid = !g_runtime_cvar->IsValidRef();
		const ConVarRef found = cvar_->FindConVar("cstmp_runtime_int", false);
		g_runtime_cvar = nullptr;

		context.Detail("ref invalid=%s, still listed by the engine=%s",
			invalid ? "yes" : "no", found.IsValidRef() ? "yes" : "no");
		if (!invalid)
		{
			context.Fail("the destructor should invalidate the ref");
		}
	});

	CS2ST_CALL(context, "ConCommand::Create (via the constructor)", {
		if (!build_runtime)
		{
			context.SkipCall("ConCommand::Create",
				"the runtime fixtures were already built in this load, and a "
				"registration is permanent");
			return;
		}

		g_runtime_command = new (g_runtime_command_storage) ConCommand(
			"cstmp_runtime_cmd", ScratchCommandCallback,
			"cstmp: command constructed after load, then destroyed", kProbeFlags);

		const ConCommandRef found = cvar_->FindConCommand("cstmp_runtime_cmd", false);
		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cstmp_runtime_cmd once");
		const int before = g_scratch_command_calls;
		cvar_->DispatchConCommand(*g_runtime_command, ctx, args);
		const int fired = g_scratch_command_calls - before;

		context.Detail("valid=%s, engine finds it=%s, dispatch fired %d time(s)",
			g_runtime_command->IsValidRef() ? "yes" : "no",
			found.IsValidRef() ? "yes" : "no", fired);

		if (!g_runtime_command->IsValidRef() || !found.IsValidRef())
		{
			context.Fail("a command built after load should register at once");
		}
		if (fired != 1)
		{
			context.Fail("one dispatch should reach the callback once, it was "
			             "reached %d time(s)", fired);
		}
	});

	CS2ST_CALL(context, "ConCommand::Destroy / ConCommand::~ConCommand", {
		if (g_runtime_command == nullptr)
		{
			context.SkipCall("ConCommand::Destroy", "no runtime ConCommand was built in this sweep");
			return;
		}

		g_runtime_command->~ConCommand();
		const bool invalid = !g_runtime_command->IsValidRef();
		g_runtime_command = nullptr;

		context.Detail("ref invalid=%s", invalid ? "yes" : "no");
		if (!invalid)
		{
			context.Fail("Destroy should invalidate the command ref");
		}
	});

	CS2ST_CALL(context, "CConCommandMemberAccessor<T>::CConCommandMemberAccessor", {
		if (!build_runtime)
		{
			context.SkipCall("CConCommandMemberAccessor<T>::CConCommandMemberAccessor",
				"the runtime fixtures were already built in this load, and a "
				"registration is permanent");
			return;
		}

		g_scratch_owner_a.calls = 0;
		g_scratch_owner_b.calls = 0;
		g_runtime_accessor = new (g_runtime_accessor_storage) ScratchAccessor(
			&g_scratch_owner_a, "cstmp_runtime_member_cmd", &ScratchOwner::OnCommand,
			"cstmp: member-function command constructed after load", kProbeFlags);

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cstmp_runtime_member_cmd once");
		cvar_->DispatchConCommand(*g_runtime_accessor, ctx, args);

		context.Detail("valid=%s, owner a reached %d time(s)",
			g_runtime_accessor->IsValidRef() ? "yes" : "no", g_scratch_owner_a.calls);
		if (!g_runtime_accessor->IsValidRef())
		{
			context.Fail("a member accessor built after load should register at once");
		}
		if (g_scratch_owner_a.calls != 1)
		{
			context.Fail("one dispatch should call the member function on the owner "
			             "once, it was called %d time(s)", g_scratch_owner_a.calls);
		}
	});

	CS2ST_CALL(context, "CConCommandMemberAccessor<T>::SetOwner", {
		if (g_runtime_accessor == nullptr)
		{
			context.SkipCall("CConCommandMemberAccessor<T>::SetOwner",
				"no member accessor was built in this sweep");
			return;
		}

		g_runtime_accessor->SetOwner(&g_scratch_owner_b);

		const CCommandContext ctx(CT_NO_TARGET, CPlayerSlot(-1));
		CCommand args;
		args.Tokenize("cstmp_runtime_member_cmd twice");
		cvar_->DispatchConCommand(*g_runtime_accessor, ctx, args);

		context.Detail("owner a=%d, owner b=%d", g_scratch_owner_a.calls, g_scratch_owner_b.calls);
		if (g_scratch_owner_b.calls != 1 || g_scratch_owner_a.calls != 1)
		{
			context.Fail("after SetOwner the call should reach the new owner only "
			             "(a=%d, b=%d)", g_scratch_owner_a.calls, g_scratch_owner_b.calls);
		}

		// Torn down here, not left to plugin unload: its callback pointer is
		// this object's own address inside this module.
		g_runtime_accessor->~ScratchAccessor();
		g_runtime_accessor = nullptr;
	});
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void ConVarSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ICvar] not called - would reach every convar in the process "
	             "and cannot be undone");

	// Listed rather than omitted: a reader has to be able to tell "checked and
	// fine" from "nobody ever looked", and an absent row cannot say which.
	context.SkipCall("ICvar::StripDevelopmentFlags",
		"clears FCVAR_DEVELOPMENTONLY and FCVAR_DEFENSIVE on every defensive "
		"convar and command in the game, which cannot be undone and would take "
		"the allow_defensive oracles above with it");
	context.SkipCall("ICvar::SetConVarsFromGameInfo",
		"the KeyValues schema it expects is unconfirmed, and the flags it sets "
		"(CVARGI_HAS_DEFAULT_VALUE and friends) mark defaults as no longer "
		"overridable for the life of the process");

	// The tally is worth stating because "no row" and "checked and fine" look
	// the same to a reader scanning a log, and this interface is large enough
	// that a missing row is easy to miss.
	context.Note("all 35 ICvar virtuals are accounted for: 33 called, 2 skipped "
	             "with reasons (the two above). ResetConVarsToDefaultValuesByFlag, "
	             "SetMaxSplitScreenSlots, CallGlobalChangeCallbacks and "
	             "ResetConVarsToSnapshot are called in ExerciseScratchCalls. "
	             "GetMaxSplitScreenSlots is non-virtual and is read alongside "
	             "ConVarData::GetMaxSplitScreenSlots");
}

// -----------------------------------------------------------------------------
//  Hooks
// -----------------------------------------------------------------------------

int ConVarSuite::InstallHooks(TestContext& context)
{
	if (cvar_ == nullptr || hooks_installed_)
	{
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before installing so a hook that never fires still shows up in
	// the dump as "never". Half of these only fire when another plugin loads or
	// unloads, so a dump taken right after installing says nothing.
	recorder.Declare(kDispatchConCommand, Id());
	recorder.Declare(kRegisterConVar, Id());
	recorder.Declare(kRegisterConCommand, Id());
	recorder.Declare(kCallGlobalChange, Id());
	recorder.Declare(kUnregisterConVar, Id());
	recorder.Declare(kUnregisterConCmd, Id());
	recorder.Declare(kGlobalChangeCb, Id());
	recorder.Declare(kOnConVarCreated, Id());
	recorder.Declare(kOnConCommandCreated, Id());

	// Each hook object carries its pre and post handler from the constructor;
	// Add installs the detour on the cvar interface, Remove takes it off.
	hook_DispatchConCommand_.Add(cvar_);
	hook_RegisterConVar_.Add(cvar_);
	hook_RegisterConCommand_.Add(cvar_);
	hook_CallGlobalChangeCallbacks_.Add(cvar_);
	hook_UnregisterConVarCallbacks_.Add(cvar_);
	hook_UnregisterConCommandCallbacks_.Add(cvar_);

	// The interface's own two observation mechanisms, which are not KHook
	// detours at all. Both have a proper inverse, so unlike Metamod's AddListener
	// these really do come back out in RemoveHooks.
	cvar_->InstallGlobalChangeCallback(&PersistentGlobalChanged);
	global_callback_installed_ = true;

	cvar_->RegisterCreationListeners(this);
	listener_registered_ = true;

	hooks_installed_ = true;

	context.Note("DispatchConCommand fires on every console command; the "
	             "register and unregister hooks fire when a plugin loads or "
	             "unloads; the global change callback fires on every convar "
	             "change anywhere in the game");
	return kHookedMethodCount;
}

void ConVarSuite::RemoveHooks(TestContext& context)
{
	if (cvar_ == nullptr || !hooks_installed_)
	{
		return;
	}

	hook_DispatchConCommand_.Remove(cvar_);
	hook_RegisterConVar_.Remove(cvar_);
	hook_RegisterConCommand_.Remove(cvar_);
	hook_CallGlobalChangeCallbacks_.Remove(cvar_);
	hook_UnregisterConVarCallbacks_.Remove(cvar_);
	hook_UnregisterConCommandCallbacks_.Remove(cvar_);

	// These two matter more than the KHook detours on unload: the engine
	// holds a raw function pointer and a raw `this`, and both land in unmapped
	// memory the moment this plugin goes away.
	if (global_callback_installed_)
	{
		cvar_->RemoveGlobalChangeCallback(&PersistentGlobalChanged);
		global_callback_installed_ = false;
	}
	if (listener_registered_)
	{
		cvar_->RemoveCreationListeners(this);
		listener_registered_ = false;
	}

	hooks_installed_ = false;
	(void)context;
}

// -----------------------------------------------------------------------------
//  IConVarListener
// -----------------------------------------------------------------------------

void ConVarSuite::OnConVarCreated(ConVarRefAbstract* new_cvar)
{
	char scratch[48];
	const char* name = LooksLikeObjectPointer(new_cvar)
		? SafeStringArg(new_cvar->GetName(), scratch)
		: "<bad ref>";

	CS2ST_HOOK_ENTER_ARGS(kOnConVarCreated, "name=\"%.63s\"", name);
	CS2ST_HOOK_LEAVE(kOnConVarCreated);
}

void ConVarSuite::OnConCommandCreated(ConCommand* new_command)
{
	// The SDK types this as ConCommand*, but a ConCommand is a ConCommandRef
	// plus nothing, so what arrives is a handle-sized object rather than
	// something with a vtable. Vetted as a pointer and not dereferenced beyond
	// the accessor, because this callback has no fault guard.
	CS2ST_HOOK_ENTER_ARGS(kOnConCommandCreated, "command=%p%s",
		static_cast<void*>(new_command),
		LooksLikeObjectPointer(new_command) ? "" : " (implausible)");
	CS2ST_HOOK_LEAVE(kOnConCommandCreated);
}

// -----------------------------------------------------------------------------
//  KHook handlers
// -----------------------------------------------------------------------------
//
// Every one of these answers Action::Ignore: the cvar system is on the path of
// every console command and every convar write, and a handler that supersedes
// the original would stop being an observer.

KHook::Return<void> ConVarSuite::Pre_DispatchConCommand(ICvar*, ConCommandRef cmd, const CCommandContext& ctx,
                                                        const CCommand& args)
{
	char scratch[48];
	// ArgC is read before any argument, and Arg() bounds-checks itself, so a
	// nonsense count cannot turn into an out-of-range read here.
	const int argc = args.ArgC();

	CS2ST_HOOK_ENTER_ARGS(kDispatchConCommand,
		"access=%u argc=%d target=%d slot=%d cmd=\"%.47s\"",
		static_cast<unsigned>(cmd.GetAccessIndex()), argc,
		static_cast<int>(ctx.GetTarget()), ctx.GetPlayerSlot().Get(),
		SafeStringArg(argc > 0 ? args.Arg(0) : "", scratch));
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Post_DispatchConCommand(ICvar*, ConCommandRef cmd, const CCommandContext& ctx,
                                                         const CCommand& args)
{
	(void)cmd;
	(void)ctx;
	(void)args;
	CS2ST_HOOK_LEAVE(kDispatchConCommand);
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Pre_RegisterConVar(ICvar*, const ConVarCreation_t& setup, uint64 flags,
                                                    ConVarRef* cvar_ref, ConVarData** cvar_data)
{
	(void)cvar_ref;
	(void)cvar_data;

	char scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kRegisterConVar, "name=\"%.47s\" type=%d flags=0x%llx",
		SafeStringArg(setup.m_pszName, scratch),
		static_cast<int>(setup.m_valueInfo.m_eVarType),
		static_cast<unsigned long long>(setup.m_nFlags | flags));
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Post_RegisterConVar(ICvar*, const ConVarCreation_t& setup, uint64 flags,
                                                     ConVarRef* cvar_ref, ConVarData** cvar_data)
{
	(void)setup;
	(void)flags;
	(void)cvar_ref;
	(void)cvar_data;
	CS2ST_HOOK_LEAVE(kRegisterConVar);
	return { KHook::Action::Ignore };
}

KHook::Return<ConCommandRef> ConVarSuite::Pre_RegisterConCommand(ICvar*, const ConCommandCreation_t& setup,
                                                                 uint64 flags)
{
	char scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kRegisterConCommand, "name=\"%.47s\" flags=0x%llx",
		SafeStringArg(setup.m_pszName, scratch),
		static_cast<unsigned long long>(setup.m_nFlags | flags));
	// Action::Ignore discards this `ret`; the engine's own registration result
	// is what the caller receives.
	return { KHook::Action::Ignore, ConCommandRef() };
}

KHook::Return<ConCommandRef> ConVarSuite::Post_RegisterConCommand(ICvar*, const ConCommandCreation_t& setup,
                                                                  uint64 flags)
{
	(void)setup;
	(void)flags;
	CS2ST_HOOK_LEAVE(kRegisterConCommand);
	return { KHook::Action::Ignore, ConCommandRef() };
}

KHook::Return<void> ConVarSuite::Pre_CallGlobalChangeCallbacks(ICvar*, ConVarRefAbstract* ref,
                                                               CSplitScreenSlot slot,
                                                               const char* new_value,
                                                               const char* old_value,
                                                               ConVarUserInfoSet_t* userinfo_data)
{
	(void)userinfo_data;

	char name_scratch[48];
	char new_scratch[48];
	char old_scratch[48];

	const char* name = LooksLikeObjectPointer(ref)
		? SafeStringArg(ref->GetName(), name_scratch)
		: "<bad ref>";

	CS2ST_HOOK_ENTER_ARGS(kCallGlobalChange, "slot=%d %.47s: \"%.23s\" -> \"%.23s\"",
		slot.Get(), name,
		SafeStringArg(old_value, old_scratch),
		SafeStringArg(new_value, new_scratch));
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Post_CallGlobalChangeCallbacks(ICvar*, ConVarRefAbstract* ref,
                                                                CSplitScreenSlot slot,
                                                                const char* new_value,
                                                                const char* old_value,
                                                                ConVarUserInfoSet_t* userinfo_data)
{
	(void)ref;
	(void)slot;
	(void)new_value;
	(void)old_value;
	(void)userinfo_data;
	CS2ST_HOOK_LEAVE(kCallGlobalChange);
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Pre_UnregisterConVarCallbacks(ICvar*, ConVarRef cvar)
{
	// Deliberately does not resolve the name: this fires while a plugin is
	// being torn down, and the ConVarData behind the handle may already be
	// half-dismantled. The handle itself is a value, so printing it is safe.
	CS2ST_HOOK_ENTER_ARGS(kUnregisterConVar, "access=%u registered=%d",
		static_cast<unsigned>(cvar.GetAccessIndex()), cvar.GetRegisteredIndex());
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Post_UnregisterConVarCallbacks(ICvar*, ConVarRef cvar)
{
	(void)cvar;
	CS2ST_HOOK_LEAVE(kUnregisterConVar);
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Pre_UnregisterConCommandCallbacks(ICvar*, ConCommandRef cmd)
{
	CS2ST_HOOK_ENTER_ARGS(kUnregisterConCmd, "access=%u registered=%d",
		static_cast<unsigned>(cmd.GetAccessIndex()), cmd.GetRegisteredIndex());
	return { KHook::Action::Ignore };
}

KHook::Return<void> ConVarSuite::Post_UnregisterConCommandCallbacks(ICvar*, ConCommandRef cmd)
{
	(void)cmd;
	CS2ST_HOOK_LEAVE(kUnregisterConCmd);
	return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
//  Hook binding
//
//  The KHook::Virtual constructor resolves the vtable slot from the member
//  pointer, so this list is the plugin's ABI contract with the SDK.
// -----------------------------------------------------------------------------

// convar.h ends with `#pragma warning(default : 4355)`, which re-enables the warning
// for the rest of the translation unit and overrides the /wd4355 on the command
// line. Passing `this` as the hook context is intentional (stored, not used).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4355)
#endif
ConVarSuite::ConVarSuite() :
	hook_DispatchConCommand_(&ICvar::DispatchConCommand, this,
		&ConVarSuite::Pre_DispatchConCommand,
		&ConVarSuite::Post_DispatchConCommand),
	hook_RegisterConVar_(&ICvar::RegisterConVar, this,
		&ConVarSuite::Pre_RegisterConVar,
		&ConVarSuite::Post_RegisterConVar),
	hook_RegisterConCommand_(&ICvar::RegisterConCommand, this,
		&ConVarSuite::Pre_RegisterConCommand,
		&ConVarSuite::Post_RegisterConCommand),
	hook_CallGlobalChangeCallbacks_(&ICvar::CallGlobalChangeCallbacks, this,
		&ConVarSuite::Pre_CallGlobalChangeCallbacks,
		&ConVarSuite::Post_CallGlobalChangeCallbacks),
	hook_UnregisterConVarCallbacks_(&ICvar::UnregisterConVarCallbacks, this,
		&ConVarSuite::Pre_UnregisterConVarCallbacks,
		&ConVarSuite::Post_UnregisterConVarCallbacks),
	hook_UnregisterConCommandCallbacks_(&ICvar::UnregisterConCommandCallbacks, this,
		&ConVarSuite::Pre_UnregisterConCommandCallbacks,
		&ConVarSuite::Post_UnregisterConCommandCallbacks)
{
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace cs2st
