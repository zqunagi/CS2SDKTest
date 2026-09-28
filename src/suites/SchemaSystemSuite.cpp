#include "suites/SchemaSystemSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <appframework/IAppSystem.h>
#include <schemasystem/schemasystem.h>
#include <tier1/bufferstring.h>
#include <tier1/utlvector.h>

#include "testkit/StructInspector.h"

namespace cs2st
{
namespace
{

/**
 * Class names to probe for.
 *
 * Deliberately several, and deliberately not required to all resolve: which
 * classes exist is a property of the game build, and encoding that here would
 * make the suite fail for the wrong reason after a content update. What *is*
 * asserted is that at least one resolves - a server type scope in which none of
 * these exist is not a type scope.
 *
 * Ordered widest-first: CBaseEntity has the most fields of the useful kinds,
 * which is what the CSchemaType probes need.
 */
const char* const kProbeClassNames[] = {
	"CBaseEntity",
	"CBaseModelEntity",
	"CBasePlayerController",
	"CCSPlayerController",
	"CCSPlayerPawn",
};

/** Enum names to try before falling back to one harvested from a class field. */
const char* const kProbeEnumNames[] = {
	"MoveType_t",
	"SolidType_t",
	"MoveCollide_t",
	"gear_slot_t",
};

/** Metadata tags to ask FindClassesByMeta about. Misses are recorded, not failed. */
const char* const kProbeMetaTags[] = {
	"MNetworkEnable",
	"MNetworkVarNames",
};

/**
 * One builtin type: the enum, the width it must report, and schema's name for it.
 *
 * The width column is the point. A builtin's size is fixed by the C++ type it
 * stands for, not by anything about the game, so every row here has exactly one
 * correct answer - which turns the whole table into a fourteen-way vtable and
 * layout check. Sizes of 0 mean "no meaningful width", and only those two rows
 * skip the size assertion.
 *
 * The name column started as a guess at schema's spelling and is still treated
 * as one - a miss on FindBuiltinTypeByName is recorded, never failed. Measured
 * on a live server, thirteen of the fourteen resolve: only "invalid" does not,
 * which is what one would expect of the placeholder at index 0. If a name that
 * used to resolve stops, schema renamed a builtin.
 */
struct BuiltinTypeExpectation
{
	SchemaBuiltinType_t type;
	unsigned expected_size;
	const char* schema_name;
};

const BuiltinTypeExpectation kBuiltinTypes[] = {
	{SCHEMA_BUILTIN_TYPE_INVALID, 0, "invalid"},
	{SCHEMA_BUILTIN_TYPE_VOID,    0, "void"},
	{SCHEMA_BUILTIN_TYPE_CHAR,    1, "char"},
	{SCHEMA_BUILTIN_TYPE_INT8,    1, "int8"},
	{SCHEMA_BUILTIN_TYPE_UINT8,   1, "uint8"},
	{SCHEMA_BUILTIN_TYPE_INT16,   2, "int16"},
	{SCHEMA_BUILTIN_TYPE_UINT16,  2, "uint16"},
	{SCHEMA_BUILTIN_TYPE_INT32,   4, "int32"},
	{SCHEMA_BUILTIN_TYPE_UINT32,  4, "uint32"},
	{SCHEMA_BUILTIN_TYPE_INT64,   8, "int64"},
	{SCHEMA_BUILTIN_TYPE_UINT64,  8, "uint64"},
	{SCHEMA_BUILTIN_TYPE_FLOAT32, 4, "float32"},
	{SCHEMA_BUILTIN_TYPE_FLOAT64, 8, "float64"},
	{SCHEMA_BUILTIN_TYPE_BOOL,    1, "bool"},
};

/**
 * Upper bound on how many entries a schema container could plausibly hold.
 *
 * Not a game fact - it is the "is this even a count" line. CS2's server scope
 * holds a few thousand class bindings; a garbage read off a misplaced field
 * gives something on the order of 2^31.
 */
constexpr int kMaxPlausibleCount = 1000000;

/** Entries printed individually before a walk switches to counting only. */
constexpr int kMaxEntriesPrinted = 8;

/** Cap on any list walk, so a garbage count cannot turn into a hang. */
constexpr int kMaxEntriesScanned = 512;

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

const char* TypeCategoryName(SchemaTypeCategory_t category)
{
	switch (category)
	{
		case SCHEMA_TYPE_BUILTIN:        return "builtin";
		case SCHEMA_TYPE_POINTER:        return "pointer";
		case SCHEMA_TYPE_BITFIELD:       return "bitfield";
		case SCHEMA_TYPE_FIXED_ARRAY:    return "fixed array";
		case SCHEMA_TYPE_ATOMIC:         return "atomic";
		case SCHEMA_TYPE_DECLARED_CLASS: return "class";
		case SCHEMA_TYPE_DECLARED_ENUM:  return "enum";
		case SCHEMA_TYPE_INVALID:        return "invalid";
		default:                         return "<out of range>";
	}
}

const char* AtomicCategoryName(SchemaAtomicCategory_t category)
{
	switch (category)
	{
		case SCHEMA_ATOMIC_PLAIN:           return "plain";
		case SCHEMA_ATOMIC_T:               return "T";
		case SCHEMA_ATOMIC_COLLECTION_OF_T: return "collection of T";
		case SCHEMA_ATOMIC_TT:              return "TT";
		case SCHEMA_ATOMIC_I:               return "I";
		case SCHEMA_ATOMIC_INVALID:         return "n/a";
		default:                            return "<out of range>";
	}
}

/** An alignment is a power of two, and nothing in this engine aligns past 64. */
bool IsPlausibleAlignment(unsigned alignment)
{
	return alignment >= 1 && alignment <= 64 && (alignment & (alignment - 1)) == 0;
}

/** Renders a name that came out of engine memory without trusting it. */
const char* SafeName(const char* name)
{
	return name != nullptr ? name : "(null)";
}

/** Running tally for the IsA<T>() / ReinterpretAs<T>() sweep. */
struct DowncastTally
{
	int matched = 0;
	int disagreements = 0;
	const char* matched_name = "(none)";
};

/**
 * One row of the downcast sweep: asks a live type whether it is T.
 *
 * A template function rather than a macro, and deliberately so. The obvious
 * shape here is a local macro over (T, category, atomic), but the eleven calls
 * live inside a CS2ST_CALL(...) invocation - and a #define inside a macro
 * argument list is undefined behaviour. Clang expands it at the wrong point and
 * the invocations parse as function calls taking a type name, which is not
 * something the diagnostic makes obvious.
 *
 * `expected` is computed from the object's own category fields rather than from
 * what the caller hopes the object is, so each row keeps exactly one correct
 * answer whichever instance was harvested.
 */
template <typename T>
void CheckDowncast(TestContext& context, CSchemaType* type, const char* type_name,
                   SchemaTypeCategory_t category, SchemaAtomicCategory_t atomic,
                   DowncastTally& tally)
{
	const bool expected = (type->m_eTypeCategory == category)
		&& (type->m_eAtomicCategory == atomic);

	const bool is_a = type->IsA<T>();
	const T* as = type->ReinterpretAs<T>();

	if (is_a != expected)
	{
		context.Fail("IsA<%s>() gave %s on a %s/%s type", type_name,
			is_a ? "true" : "false",
			TypeCategoryName(type->m_eTypeCategory),
			AtomicCategoryName(type->m_eAtomicCategory));
		++tally.disagreements;
	}

	// ReinterpretAs is defined in terms of IsA, so a disagreement means the two
	// templates were instantiated against different category constants - which
	// is what a reordered SchemaTypeCategory_t enum does.
	if ((as != nullptr) != is_a)
	{
		context.Fail("ReinterpretAs<%s>() gave %p but IsA said %s", type_name,
			static_cast<const void*>(as), is_a ? "true" : "false");
		++tally.disagreements;
	}
	if (as != nullptr
		&& static_cast<const void*>(as) != static_cast<const void*>(type))
	{
		context.Fail("ReinterpretAs<%s>() moved the pointer", type_name);
		++tally.disagreements;
	}

	if (is_a)
	{
		++tally.matched;
		tally.matched_name = type_name;
	}
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool SchemaSystemSuite::Acquire(TestContext& context)
{
	schema_ = static_cast<ISchemaSystem*>(
		context.QueryInterface("schemasystem", SCHEMASYSTEM_INTERFACE_VERSION));

	if (schema_ == nullptr)
	{
		const Module& module = context.GetModule("schemasystem");
		if (!module.IsValid())
		{
			context.UnavailableCall("ISchemaSystem", "the schemasystem module is not loaded");
		}
		else
		{
			context.UnavailableCall("ISchemaSystem",
				"schemasystem is loaded but does not answer '%s'",
				SCHEMASYSTEM_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(schema_),
		SCHEMASYSTEM_INTERFACE_VERSION);
	return true;
}

void SchemaSystemSuite::ExerciseCalls(TestContext& context)
{
	schema_concrete_ = nullptr;
	global_scope_ = nullptr;
	server_scope_ = nullptr;
	probe_scope_ = nullptr;
	probe_scope_name_ = nullptr;
	resolved_scope_key_ = nullptr;
	probe_class_ = nullptr;
	probe_class_name_ = nullptr;
	probe_fields_ = nullptr;
	probe_field_count_ = 0;
	probe_enum_ = nullptr;
	probe_enum_name_ = nullptr;
	probe_type_ = nullptr;
	probe_builtin_type_ = nullptr;
	probe_class_type_ = nullptr;
	probe_enum_type_ = nullptr;
	probe_atomic_type_ = nullptr;
	probe_pointer_type_ = nullptr;

	// Order matters: each phase harvests the live objects the next one needs.
	// Scopes come from the system, classes from a scope, fields from a class,
	// types from the fields, and the enum probes prefer a type harvested from a
	// field over a guessed enum name.
	ExerciseAppSystem(context);
	ExerciseSystemQueries(context);
	InspectSchemaSystemLayout(context);
	EnumerateTypeScopes(context);
	ExerciseScopeResolution(context);
	ExerciseScopeQueries(context);
	ExerciseBuiltinTypes(context);
	ExerciseClassLookups(context);
	InspectTypeScopeLayout(context);
	InspectInternedTypeMaps(context);
	InspectClassInfoLayout(context);
	InspectClassFields(context);
	InspectBaseClasses(context);
	InspectClassMetadata(context);
	ExerciseSchemaTypeVirtuals(context);
	ExerciseAtomicLookups(context);
	ExerciseEnumLookups(context);
	InspectEnumInfoLayout(context);
	InspectEnumerators(context);
	ExerciseClassGraph(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = schema_->GetTier();
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

	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		const bool singleton = schema_->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the schema system is a singleton; "
			             "false here suggests a slot shift");
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = schema_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = schema_->GetDependencies();
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
		void* self = schema_->QueryInterface(SCHEMASYSTEM_INTERFACE_VERSION);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(schema_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run schema system startup");
	context.SkipCall("IAppSystem::Init", "would re-run schema system startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin schema system shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would discard every schema binding");
	context.SkipCall("IAppSystem::Disconnect", "would tear down schema interfaces");
}

// -----------------------------------------------------------------------------
//  ISchemaSystem - the plain queries
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseSystemQueries(TestContext& context)
{
	context.Note("[ISchemaSystem] system queries");

	// First call past IAppSystem, and the one that decides whether the rest of
	// this suite means anything. See the header on the destructor slot: if the
	// SDK has the vtable shape wrong, this is where it shows.
	CS2ST_CALL(context, "ISchemaSystem::GlobalTypeScope", {
		global_scope_ = schema_->GlobalTypeScope();
		context.Detail("-> %p", static_cast<void*>(global_scope_));
		if (global_scope_ == nullptr)
		{
			context.Fail("there is always a global type scope");
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::SchemaSystemIsReady", {
		const bool ready = schema_->SchemaSystemIsReady();
		context.Detail("-> %s", ready ? "true" : "false");
		if (!ready)
		{
			// The server is running and serving entities, so the schema system
			// finished coming up long before this plugin was asked anything.
			context.Fail("the schema system reports not ready on a running server");
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::DefaultTypeScopeIsLocal", {
		// No single correct answer - it is a configuration bit. Recorded so a
		// future change to it is visible, and cross-checked against the field
		// it must be reading in InspectSchemaSystemLayout.
		context.Detail("-> %s", schema_->DefaultTypeScopeIsLocal() ? "true" : "false");
	});
}

// -----------------------------------------------------------------------------
//  CSchemaSystem - the concrete class behind the interface
//
//  Done early on purpose. Two of its fields are the backing store for virtual
//  getters already called above, which makes them the cheapest oracle available
//  and a reason to trust - or not trust - everything read through this pointer
//  later, including the m_TypeScopes enumeration the next phase depends on.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectSchemaSystemLayout(TestContext& context)
{
	context.Note("[CSchemaSystem] struct layout (SDK sizeof: %zu bytes)",
		sizeof(CSchemaSystem));

	// The SDK says the object behind ISchemaSystem is a CSchemaSystem. That is
	// an assumption, and the two boolean cross-checks below are what test it.
	schema_concrete_ = static_cast<CSchemaSystem*>(schema_);

	CS2ST_CALL(context, "CSchemaSystem::m_bSchemaSystemIsReady vs SchemaSystemIsReady()", {
		const bool from_call = schema_->SchemaSystemIsReady();
		const bool from_field = schema_concrete_->m_bSchemaSystemIsReady;
		context.Detail("call %s vs field@0x%zx %s",
			from_call ? "true" : "false",
			static_cast<std::size_t>(offsetof(CSchemaSystem, m_bSchemaSystemIsReady)),
			from_field ? "true" : "false");

		// A getter and the field it returns cannot disagree. If they do, either
		// the field is not at the offset the SDK computed or the vtable slot is
		// not that getter - and every other field read through this pointer is
		// then worthless.
		if (from_call != from_field)
		{
			context.Fail("the getter and the field it must be reading disagree");
		}
	});

	CS2ST_CALL(context, "CSchemaSystem::m_bDefaultTypeScopeIsLocal vs DefaultTypeScopeIsLocal()", {
		const bool from_call = schema_->DefaultTypeScopeIsLocal();
		const bool from_field = schema_concrete_->m_bDefaultTypeScopeIsLocal;
		context.Detail("call %s vs field@0x%zx %s",
			from_call ? "true" : "false",
			static_cast<std::size_t>(offsetof(CSchemaSystem, m_bDefaultTypeScopeIsLocal)),
			from_field ? "true" : "false");
		if (from_call != from_field)
		{
			context.Fail("the getter and the field it must be reading disagree");
		}
	});

	CS2ST_CALL(context, "CSchemaSystem (registration counters)", {
		context.Detail("registrations=%d ignored=%d redundant=%d ignoredBytes=%zu connections=%d",
			schema_concrete_->m_nRegistrations,
			schema_concrete_->m_nIgnored,
			schema_concrete_->m_nRedundant,
			schema_concrete_->m_nIgnoredBytes,
			schema_concrete_->m_nNumConnections);

		// Counters, so negative is a type-level impossibility rather than a
		// game-knowledge claim. These sit after the two CUtlStringMaps, which
		// makes a sane value here evidence for the middle of the struct.
		if (schema_concrete_->m_nRegistrations < 0
			|| schema_concrete_->m_nIgnored < 0
			|| schema_concrete_->m_nRedundant < 0)
		{
			context.Fail("a registration counter cannot be negative");
		}
		if (schema_concrete_->m_nRegistrations == 0)
		{
			context.Fail("a running server has registered schema bindings");
		}
		if (schema_concrete_->m_nNumConnections <= 0)
		{
			context.Fail("the schema system has at least one connected module");
		}
	});

	CS2ST_CALL(context, "CSchemaSystem (container counts)", {
		const int modules = schema_concrete_->m_LoadedModules.Count();
		const int manifests = schema_concrete_->m_ResourceManifestDescs.Count();
		const int mismatches = schema_concrete_->m_DetectedSchemaMismatches.Count();
		const int callbacks = schema_concrete_->m_CompleteModuleRegistrationCallbacks.Count();
		context.Detail("modules=%d manifests=%d mismatches=%d callbacks=%d",
			modules, manifests, mismatches, callbacks);

		if (modules < 0 || modules > kMaxPlausibleCount
			|| manifests < 0 || manifests > kMaxPlausibleCount
			|| mismatches < 0 || mismatches > kMaxPlausibleCount
			|| callbacks < 0 || callbacks > kMaxPlausibleCount)
		{
			context.Fail("one of these is not a count");
		}

		// Deliberately no "modules must be non-empty" assertion. Measured on a
		// live server: all four of these read 0 while m_nRegistrations was
		// 4109 and m_TypeScopes held 17 scopes - so registration plainly
		// happened and these containers simply do not retain anything once it
		// is done. An earlier version failed here, which was this project's own
		// rule being broken: assert what the *type* implies (a count cannot be
		// negative), never what the semantics ought to be.
		//
		// The layout is not in doubt either, and the reason is worth keeping:
		// m_TypeScopes sits *after* m_LoadedModules and read back exactly 17,
		// matching GetNumStrings(). Misalignment does not repair itself, so a
		// correct field downstream proves the empty one above it is really
		// empty rather than misplaced.

		if (mismatches > 0)
		{
			// Not our failure - it is the engine reporting that two of its own
			// modules disagree about a binding. Worth surfacing rather than
			// leaving in a counter nobody reads.
			context.Suspect("the engine itself detected %d schema binding mismatch(es)",
				mismatches);
		}
	});

	// ISchemaSystem, the base, declares no data members - nothing to lay out.
	// Its vtable is covered as calls.
	context.Note("  ISchemaSystem declares no data members; nothing to inspect "
	             "above CSchemaSystem");

	StructInspector inspect(context, "CSchemaSystem",
		schema_concrete_, sizeof(CSchemaSystem));

	// Only the fields whose type carries a judgement. The ConCommand accessors
	// and the mutex in between are large objects with no single-field check to
	// make; they are covered indirectly, in that the counters after them have
	// to land in the right place for their values to be sane.
	inspect.Raw    ("m_ResourceManifestDescs",
	                                 offsetof(CSchemaSystem, m_ResourceManifestDescs), 16);
	inspect.Int32  ("m_nNumConnections",
	                                 offsetof(CSchemaSystem, m_nNumConnections), 0, INT32_MAX);
	inspect.Raw    ("m_LoadedModules", offsetof(CSchemaSystem, m_LoadedModules), 16);
	inspect.Raw    ("m_DetectedSchemaMismatches",
	                                 offsetof(CSchemaSystem, m_DetectedSchemaMismatches), 16);
	inspect.Bool   ("m_bDefaultTypeScopeIsLocal",
	                                 offsetof(CSchemaSystem, m_bDefaultTypeScopeIsLocal));
	inspect.Bool   ("m_bSchemaSystemIsReady",
	                                 offsetof(CSchemaSystem, m_bSchemaSystemIsReady));
	inspect.Raw    ("m_TypeScopes",  offsetof(CSchemaSystem, m_TypeScopes), 16);
	inspect.Raw    ("m_BindingsAddressRanges",
	                                 offsetof(CSchemaSystem, m_BindingsAddressRanges), 16);
	inspect.Int32  ("m_nRegistrations",
	                                 offsetof(CSchemaSystem, m_nRegistrations), 0, INT32_MAX);
	inspect.Int32  ("m_nIgnored",    offsetof(CSchemaSystem, m_nIgnored), 0, INT32_MAX);
	inspect.Int32  ("m_nRedundant",  offsetof(CSchemaSystem, m_nRedundant), 0, INT32_MAX);
	inspect.UInt64 ("m_nIgnoredBytes",
	                                 offsetof(CSchemaSystem, m_nIgnoredBytes));
	inspect.Raw    ("m_CompleteModuleRegistrationCallbacks",
	                                 offsetof(CSchemaSystem, m_CompleteModuleRegistrationCallbacks), 16);
	inspect.Summarize();
}

// -----------------------------------------------------------------------------
//  m_TypeScopes - the definitive answer to "what is a scope called"
//
//  Which spelling Valve keys type scopes by is undocumented, and a plugin that
//  guesses wrong silently gets no properties at all. Rather than guess, the map
//  is enumerated: every registered scope name is printed, and each one is fed
//  back through FindTypeScopeForModule so the two agree. That list is exactly
//  the kind of content that changes when the game changes while the interface
//  version does not move.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::EnumerateTypeScopes(TestContext& context)
{
	context.Note("[CSchemaSystem::m_TypeScopes] registered type scopes");

	if (schema_concrete_ == nullptr)
	{
		context.SkipCall("CSchemaSystem::m_TypeScopes", "no concrete pointer to read");
		return;
	}

	int scope_count = 0;

	CS2ST_CALL(context, "CSchemaSystem::m_TypeScopes.GetNumStrings", {
		scope_count = schema_concrete_->m_TypeScopes.GetNumStrings();
		context.Detail("-> %d scope(s)", scope_count);
		if (scope_count <= 0)
		{
			context.Fail("a running server has type scopes registered");
		}
		else if (scope_count > kMaxPlausibleCount)
		{
			context.Fail("%d is not a count; m_TypeScopes is misplaced", scope_count);
			scope_count = 0;
		}
	});

	if (scope_count <= 0)
	{
		return;
	}

	const int walked = (scope_count < kMaxEntriesScanned) ? scope_count : kMaxEntriesScanned;

	// Each scope individually, so a fault names the index rather than losing
	// the whole enumeration.
	for (int i = 0; i < walked; ++i)
	{
		char target[80];
		std::snprintf(target, sizeof(target), "m_TypeScopes[%d]", i);

		CS2ST_CALL(context, target, {
			const char* name = schema_concrete_->m_TypeScopes.String(i);
			CSchemaSystemTypeScope* scope = schema_concrete_->m_TypeScopes[i];
			context.Detail("\"%.80s\" -> %p", SafeName(name), static_cast<void*>(scope));

			if (name == nullptr)
			{
				context.Fail("a registered scope always has a name");
				return;
			}
			if (scope == nullptr)
			{
				context.Fail("a registered scope name maps to a null scope");
				return;
			}

			// The oracle. The name came out of the map's own symbol table, so
			// feeding it back through the interface must return the identical
			// pointer. This is also how the "which spelling" question gets a
			// real answer instead of a guess.
			CSchemaSystemTypeScope* found = schema_->FindTypeScopeForModule(name);
			if (found != scope)
			{
				context.Fail("FindTypeScopeForModule(\"%.60s\") gave %p, the map holds %p",
					name, static_cast<void*>(found), static_cast<void*>(scope));
			}

			// Remember the server scope by its own registered name, so every
			// later lookup uses the spelling the engine actually stored.
			if (server_scope_ == nullptr && std::strstr(name, "server") != nullptr)
			{
				server_scope_ = scope;
				resolved_scope_key_ = name;
			}
		});
	}

	if (scope_count > walked)
	{
		context.Note("  ... %d more scope(s) not walked", scope_count - walked);
	}

	if (server_scope_ != nullptr)
	{
		context.Note("  server scope keyed as \"%.80s\" -> %p",
			SafeName(resolved_scope_key_), static_cast<void*>(server_scope_));
	}
	else
	{
		context.Note("  no scope name contained \"server\"; "
		             "class probes will use the global scope");
	}

	probe_scope_ = (server_scope_ != nullptr) ? server_scope_ : global_scope_;
	probe_scope_name_ = (server_scope_ != nullptr) ? "server module" : "global";
}

// -----------------------------------------------------------------------------
//  The three scope-resolution entry points, cross-checked against each other
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseScopeResolution(TestContext& context)
{
	context.Note("[ISchemaSystem] scope resolution");

	if (resolved_scope_key_ == nullptr || server_scope_ == nullptr)
	{
		context.SkipCall("ISchemaSystem::FindOrCreateTypeScopeForModule",
			"no registered scope name to pass; calling it with a name that does "
			"not exist would create a scope on a live server");
		context.SkipCall("ISchemaSystem::GetTypeScopeForBinding", "same reason");
		return;
	}

	CS2ST_CALL(context, "ISchemaSystem::FindOrCreateTypeScopeForModule", {
		// Safe *because* the name is one the enumeration just proved exists:
		// with a hit there is nothing to create, so this reduces to a find and
		// must return the same object. Called with any other name it would add
		// a scope to a running server, which is why it is skipped above when
		// the enumeration produced nothing.
		CSchemaSystemTypeScope* scope =
			schema_->FindOrCreateTypeScopeForModule(resolved_scope_key_);
		context.Detail("-> %p for \"%.60s\" (find gave %p)",
			static_cast<void*>(scope), resolved_scope_key_,
			static_cast<void*>(server_scope_));
		if (scope != server_scope_)
		{
			context.Fail("create-or-find disagrees with find for an existing scope");
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::FindTypeScopeForModule (binding-name out param)", {
		// The second parameter is an out pointer for the binding name schema
		// actually used. Seeded with a sentinel so "the engine did not write
		// it" is distinguishable from "it wrote a null".
		const char* binding_name = reinterpret_cast<const char*>(-1);
		CSchemaSystemTypeScope* scope =
			schema_->FindTypeScopeForModule(resolved_scope_key_, &binding_name);
		const bool written = (binding_name != reinterpret_cast<const char*>(-1));
		context.Detail("-> %p, binding name %s\"%.60s\"",
			static_cast<void*>(scope),
			written ? "" : "NOT WRITTEN ",
			written && binding_name != nullptr ? binding_name : "");
		if (scope != server_scope_)
		{
			context.Fail("the two-argument overload disagrees with the one-argument form");
		}
	});

	// All three SchemaTypeScope_t values, against the same module name.
	//
	// An earlier version asserted that SCHEMA_GLOBAL_TYPE_SCOPE routes to the
	// global scope and that SCHEMA_DEFAULT_TYPE_SCOPE follows the
	// DefaultTypeScopeIsLocal bit. Measured on a live server, neither holds:
	// with a module name supplied, all three values returned that module's
	// scope, including GLOBAL while DefaultTypeScopeIsLocal was false.
	//
	// So the enum does not select which of the two scopes comes back - at least
	// not when a module name is given - and the SDK documents nothing about
	// what it does select. Guessing at it and calling a mismatch a FAIL was
	// inventing a contract; what the calls are worth is the record of what each
	// value actually returns, which is now on file. The assertions kept are the
	// ones that need no guess: a non-null answer, and an answer that is a scope
	// this suite has otherwise seen.
	struct BindingScopeCase
	{
		SchemaTypeScope_t value;
		const char* label;
	};
	const BindingScopeCase binding_cases[] = {
		{SCHEMA_GLOBAL_TYPE_SCOPE, "GLOBAL"},
		{SCHEMA_MODULE_LOCAL_TYPE_SCOPE, "MODULE_LOCAL"},
		{SCHEMA_DEFAULT_TYPE_SCOPE, "DEFAULT"},
	};

	for (const BindingScopeCase& binding_case : binding_cases)
	{
		const std::string target =
			std::string("ISchemaSystem::GetTypeScopeForBinding(") + binding_case.label + ")";
		CS2ST_CALL(context, target, {
			// Safe for the same reason as FindOrCreateTypeScopeForModule: the
			// name is one the enumeration proved exists, so nothing is created.
			CSchemaSystemTypeScope* scope = schema_->GetTypeScopeForBinding(
				binding_case.value, resolved_scope_key_);

			const char* which = "an unrecognised scope";
			if (scope == server_scope_)      { which = "the module scope"; }
			else if (scope == global_scope_) { which = "the global scope"; }

			context.Detail("-> %p (%s); module=%p global=%p, DefaultTypeScopeIsLocal=%s",
				static_cast<void*>(scope), which,
				static_cast<void*>(server_scope_), static_cast<void*>(global_scope_),
				schema_->DefaultTypeScopeIsLocal() ? "true" : "false");

			if (scope == nullptr)
			{
				context.Fail("a binding always resolves to some scope");
			}
			else if (scope != server_scope_ && scope != global_scope_)
			{
				// Type-level, not semantic: whatever the enum means, the answer
				// has to be a scope object, and the only two this suite can
				// identify are the module's and the global one. A third address
				// is either another module's scope or not a scope at all.
				context.Fail("neither the module scope nor the global scope");
			}
		});
	}
}

// -----------------------------------------------------------------------------
//  CSchemaSystemTypeScope - the virtual side
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseScopeQueries(TestContext& context)
{
	context.Note("[CSchemaSystemTypeScope] scope queries (%s scope)",
		probe_scope_name_ != nullptr ? probe_scope_name_ : "no");

	if (probe_scope_ == nullptr)
	{
		context.SkipCall("CSchemaSystemTypeScope::<all>",
			"no type scope was resolved; nothing to ask");
		return;
	}

	CS2ST_CALL(context, "CSchemaSystemTypeScope::GetScopeName", {
		const char* name = probe_scope_->GetScopeName();
		context.Detail("-> \"%.100s\"", SafeName(name));
		if (name == nullptr || name[0] == '\0')
		{
			context.Fail("a scope always carries a name");
		}
	});

	CS2ST_CALL(context, "CSchemaSystemTypeScope::IsGlobalScope", {
		const bool is_global = probe_scope_->IsGlobalScope();
		const bool should_be_global = (probe_scope_ == global_scope_);
		context.Detail("-> %s (asked for the %s scope)",
			is_global ? "true" : "false", probe_scope_name_);

		// We know which scope we asked for, so the object's own answer has to
		// match. Getting this backwards means the pointer is not the scope we
		// think it is, or the slot is not IsGlobalScope.
		if (is_global != should_be_global)
		{
			context.Fail("the %s scope reports IsGlobalScope() == %s",
				probe_scope_name_, is_global ? "true" : "false");
		}
	});

	// Both scopes, not just the probe one: the global scope is the one every
	// promoted type ends up in, and it answers a different set of lookups.
	if (global_scope_ != nullptr && global_scope_ != probe_scope_)
	{
		CS2ST_CALL(context, "CSchemaSystemTypeScope::GetScopeName (global scope)", {
			const char* name = global_scope_->GetScopeName();
			context.Detail("-> \"%.100s\"", SafeName(name));
			if (name == nullptr)
			{
				context.Fail("a scope always carries a name");
			}
		});

		CS2ST_CALL(context, "CSchemaSystemTypeScope::IsGlobalScope (global scope)", {
			const bool is_global = global_scope_->IsGlobalScope();
			context.Detail("-> %s", is_global ? "true" : "false");
			if (!is_global)
			{
				context.Fail("the global scope must report itself as global");
			}
		});
	}

	CS2ST_CALL(context, "CSchemaSystemTypeScope::m_pGlobalTypeScope vs GlobalTypeScope()", {
		// A module scope carries a pointer back to the global one. That field
		// sits early in the struct, and its correct value is known from the
		// interface - so this pins the head of a 20 KB declaration without
		// needing to trust anything else in it.
		CSchemaSystemTypeScope* from_field = probe_scope_->m_pGlobalTypeScope;
		context.Detail("field@0x%zx %p vs GlobalTypeScope() %p",
			static_cast<std::size_t>(offsetof(CSchemaSystemTypeScope, m_pGlobalTypeScope)),
			static_cast<void*>(from_field), static_cast<void*>(global_scope_));
		if (from_field != global_scope_)
		{
			// The global scope's own back-pointer may legitimately be null or
			// self; only a module scope owes a real answer here.
			if (probe_scope_ == global_scope_)
			{
				context.Suspect("the global scope's own back-pointer is %p",
					static_cast<void*>(from_field));
			}
			else
			{
				context.Fail("the scope's global back-pointer is not the global scope");
			}
		}
	});
}

// -----------------------------------------------------------------------------
//  Builtin types - fourteen rows with exactly one correct answer each
//
//  Four independent routes to the same object, and the fourth is the strongest
//  check in this project: an address computed from the SDK's declared layout
//  with offsetof, compared against what a virtual call returns. Agreement
//  confirms the vtable slot and the struct layout simultaneously, from two
//  descriptions that share no code.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseBuiltinTypes(TestContext& context)
{
	context.Note("[builtin types] Type_Builtin vs GetSchemaBuiltinType vs "
	             "FindBuiltinTypeByName vs &m_BuiltinTypes[i]");

	if (probe_scope_ == nullptr)
	{
		context.SkipCall("CSchemaSystemTypeScope::Type_Builtin", "no type scope resolved");
		context.SkipCall("CSchemaSystemTypeScope::FindBuiltinTypeByName", "no type scope resolved");
		context.SkipCall("ISchemaSystem::GetSchemaBuiltinType", "no type scope resolved");
		return;
	}

	// Checked on the global scope, not the probe scope.
	//
	// Measured on a live server: a module scope reports false here, and the
	// reason turns out to be that it has no builtin array of its own - see the
	// address arithmetic in the per-builtin check below. Only the scope that
	// actually owns the array owes a true, so asserting it on a module scope
	// (as an earlier version did) fails for a reason that says nothing.
	if (global_scope_ != nullptr)
	{
		CS2ST_CALL(context, "CSchemaSystemTypeScope::m_bBuiltinTypesInitialized (global)", {
			const bool initialised = global_scope_->m_bBuiltinTypesInitialized;
			context.Detail("-> %s", initialised ? "true" : "false");
			if (!initialised)
			{
				context.Fail("the global scope owns the builtin array and must have "
				             "initialised it");
			}
		});
	}

	if (probe_scope_ != global_scope_)
	{
		CS2ST_CALL(context, "CSchemaSystemTypeScope::m_bBuiltinTypesInitialized (module)", {
			// Recorded, never judged: false is the observed and expected value
			// for a scope that defers to the global array.
			const bool initialised = probe_scope_->m_bBuiltinTypesInitialized;
			context.Detail("-> %s%s", initialised ? "true" : "false",
				initialised ? " (this module scope has its own array)"
				            : " (defers to the global scope's array, as expected)");
		});
	}

	for (const BuiltinTypeExpectation& expectation : kBuiltinTypes)
	{
		char target[96];
		std::snprintf(target, sizeof(target), "builtin[%d] %s",
			static_cast<int>(expectation.type), expectation.schema_name);

		CS2ST_CALL(context, target, {
			// Type_Builtin indexes the scope's pre-initialised array rather
			// than interning anything, which is why it is safe to call where
			// the rest of the Type_ family is not.
			SchemaMetaInfoHandle_t<CSchemaType_Builtin> by_enum =
				probe_scope_->Type_Builtin(expectation.type);
			CSchemaType_Builtin* from_scope = by_enum.Get();

			SchemaMetaInfoHandle_t<CSchemaType_Builtin> by_system =
				schema_->GetSchemaBuiltinType(expectation.type);
			CSchemaType_Builtin* from_system = by_system.Get();

			SchemaMetaInfoHandle_t<CSchemaType_Builtin> by_name =
				probe_scope_->FindBuiltinTypeByName(expectation.schema_name);
			CSchemaType_Builtin* from_name = by_name.Get();

			// The array element address, computed from the SDK's declaration -
			// based on the GLOBAL scope, which is where the array really lives.
			//
			// This is the measurement that made the whole check work. Asking a
			// module scope for a builtin returns a pointer into the *global*
			// scope's array, so an earlier version - which computed the address
			// from the probe scope - reported all fourteen as moved. The live
			// numbers said otherwise:
			//
			//   Type_Builtin(0) on libserver.so  -> 0x7fc0c0db8c98
			//   GlobalTypeScope()                -> 0x7fc0c0db8b80
			//   difference                        = 0x118 = offsetof(m_BuiltinTypes)
			//   stride between consecutive        = 0x28  = sizeof(CSchemaType_Builtin)
			//
			// Both match the SDK exactly, so the declaration is right and only
			// the base object was wrong. Which also explains a module scope
			// reporting m_bBuiltinTypesInitialized == false: it has no array of
			// its own to initialise.
			const CSchemaSystemTypeScope* owner =
				(global_scope_ != nullptr) ? global_scope_ : probe_scope_;
			const CSchemaType_Builtin* from_layout =
				reinterpret_cast<const CSchemaType_Builtin*>(
					reinterpret_cast<const unsigned char*>(owner)
					+ offsetof(CSchemaSystemTypeScope, m_BuiltinTypes)
					+ sizeof(CSchemaType_Builtin) * static_cast<std::size_t>(expectation.type));

			context.Detail("scope=%p system=%p byName=%p layout=%p (global+0x%zx)",
				static_cast<void*>(from_scope), static_cast<void*>(from_system),
				static_cast<void*>(from_name), static_cast<const void*>(from_layout),
				static_cast<std::size_t>(offsetof(CSchemaSystemTypeScope, m_BuiltinTypes)
					+ sizeof(CSchemaType_Builtin) * static_cast<std::size_t>(expectation.type)));

			if (from_scope == nullptr)
			{
				context.Fail("Type_Builtin returned null for a pre-installed builtin");
				return;
			}
			// The oracle: a virtual call's return value against an address
			// computed purely from the SDK's declared layout. Two descriptions
			// that share no code, so agreement confirms the vtable slot, the
			// array offset and sizeof(CSchemaType_Builtin) all at once.
			if (from_scope != from_layout)
			{
				context.Fail("Type_Builtin gave %p, the SDK's layout puts "
				             "m_BuiltinTypes[%d] at %p; offset 0x%zx or "
				             "sizeof(CSchemaType_Builtin)=%zu has moved",
					static_cast<void*>(from_scope), static_cast<int>(expectation.type),
					static_cast<const void*>(from_layout),
					static_cast<std::size_t>(offsetof(CSchemaSystemTypeScope, m_BuiltinTypes)),
					sizeof(CSchemaType_Builtin));
			}
			// Now that the owner is known to be the global scope, these two
			// must agree exactly rather than merely plausibly.
			if (from_system != from_scope)
			{
				context.Fail("GetSchemaBuiltinType gave %p but the scope gave %p",
					static_cast<void*>(from_system), static_cast<void*>(from_scope));
			}
			// The name column is our guess at schema's spelling, so a miss says
			// nothing about the layout and is recorded rather than failed.
			if (from_name == nullptr)
			{
				context.Detail("scope=%p system=%p byName=(none, \"%s\" is our guess) layout=%p",
					static_cast<void*>(from_scope), static_cast<void*>(from_system),
					expectation.schema_name, static_cast<const void*>(from_layout));
			}
			else if (from_name != from_scope)
			{
				context.Fail("FindBuiltinTypeByName(\"%s\") resolved to a different builtin",
					expectation.schema_name);
			}

			// Now the object's own fields, with the enum we asked for and the
			// width that enum implies as the two known-correct answers.
			if (from_scope->m_eBuiltinType != expectation.type)
			{
				context.Fail("asked for builtin %d, the object says %d",
					static_cast<int>(expectation.type),
					static_cast<int>(from_scope->m_eBuiltinType));
			}
			if (expectation.expected_size != 0
				&& from_scope->m_nSize != expectation.expected_size)
			{
				context.Fail("%s is %u bytes; m_nSize says %u",
					expectation.schema_name, expectation.expected_size,
					static_cast<unsigned>(from_scope->m_nSize));
			}
			if (from_scope->m_eTypeCategory != SCHEMA_TYPE_BUILTIN)
			{
				context.Fail("a builtin's category is %s, not %s",
					TypeCategoryName(SCHEMA_TYPE_BUILTIN),
					TypeCategoryName(from_scope->m_eTypeCategory));
			}
		});
	}

	// One builtin also serves as a CSchemaType instance for the virtual sweep,
	// and it is the best possible one: everything about it is known in advance.
	CS2ST_CALL(context, "builtin int32 (kept for the CSchemaType sweep)", {
		SchemaMetaInfoHandle_t<CSchemaType_Builtin> handle =
			probe_scope_->Type_Builtin(SCHEMA_BUILTIN_TYPE_INT32);
		probe_builtin_type_ = handle.Get();
		context.Detail("-> %p", static_cast<void*>(probe_builtin_type_));
	});
}

// -----------------------------------------------------------------------------
//  Class lookups
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseClassLookups(TestContext& context)
{
	context.Note("[CSchemaSystemTypeScope] class lookups");

	if (probe_scope_ == nullptr)
	{
		context.SkipCall("CSchemaSystemTypeScope::FindDeclaredClass", "no type scope resolved");
		return;
	}

	// Every candidate is tried and every result logged, so the log says which
	// of these the running build actually has rather than only the first hit.
	int resolved = 0;
	for (const char* class_name : kProbeClassNames)
	{
		const std::string target =
			std::string("CSchemaSystemTypeScope::FindDeclaredClass(\"") + class_name + "\")";
		CS2ST_CALL(context, target, {
			SchemaMetaInfoHandle_t<CSchemaClassInfo> handle =
				probe_scope_->FindDeclaredClass(class_name);
			CSchemaClassInfo* info = handle.Get();
			if (info == nullptr)
			{
				context.Detail("-> (not in this scope)");
				return;
			}

			++resolved;
			context.Detail("-> %p, %d bytes, %u field(s)", static_cast<void*>(info),
				info->m_nSize, static_cast<unsigned>(info->m_nFieldCount));

			if (probe_class_ == nullptr)
			{
				probe_class_ = info;
				probe_class_name_ = class_name;
			}
		});
	}

	CS2ST_CALL(context, "CSchemaSystemTypeScope (probe classes resolved)", {
		context.Detail("%d of %zu resolved", resolved,
			sizeof(kProbeClassNames) / sizeof(kProbeClassNames[0]));
		if (resolved == 0)
		{
			// Which classes exist is a game fact and not asserted, but a scope
			// holding none of the base entity types is not a working scope.
			context.Fail("no probe class resolved; property access is dead");
		}
	});

	// FindClassByScopedName reaches classes without knowing which scope they
	// live in, which is the API most plugins actually use.
	CS2ST_CALL(context, "ISchemaSystem::FindClassByScopedName", {
		SchemaMetaInfoHandle_t<CSchemaClassInfo> handle =
			schema_->FindClassByScopedName("CBaseEntity");
		CSchemaClassInfo* info = handle.Get();
		context.Detail("-> %p for \"CBaseEntity\"", static_cast<void*>(info));
		if (info != nullptr && info->m_pszName != nullptr
			&& std::strcmp(info->m_pszName, "CBaseEntity") != 0)
		{
			context.Fail("the binding returned is named \"%.60s\"", info->m_pszName);
		}
	});

	if (probe_class_ == nullptr)
	{
		return;
	}

	// FindRawClassBinding is the same lookup without the handle wrapper, so the
	// two must return the same object. Cheap, and it catches a slot swap that
	// neither call alone would.
	CS2ST_CALL(context, "CSchemaSystemTypeScope::FindRawClassBinding(name)", {
		CSchemaClassInfo* raw = probe_scope_->FindRawClassBinding(probe_class_name_);
		context.Detail("-> %p for \"%s\" (FindDeclaredClass gave %p)",
			static_cast<void*>(raw), probe_class_name_, static_cast<void*>(probe_class_));
		if (raw != probe_class_)
		{
			context.Fail("the two lookups disagree about the same class name");
		}
	});

	// FindType_DeclaredClass is the pure-find half of the Type_ family, and the
	// class binding already carries the answer in m_pDeclaredClass - so this is
	// a lookup with a built-in oracle.
	CS2ST_CALL(context, "CSchemaSystemTypeScope::FindType_DeclaredClass(name)", {
		CSchemaType_DeclaredClass* type =
			probe_scope_->FindType_DeclaredClass(probe_class_name_);
		context.Detail("-> %p (the binding's m_pDeclaredClass is %p)",
			static_cast<void*>(type), static_cast<void*>(probe_class_->m_pDeclaredClass));
		if (type == nullptr)
		{
			context.Fail("a declared class always has a declared-class type");
			return;
		}
		if (type != probe_class_->m_pDeclaredClass)
		{
			context.Fail("the interned type is not the one the binding points at");
		}
		// And the type points back at the class it describes.
		if (type->m_pClassInfo != probe_class_)
		{
			context.Fail("the type's m_pClassInfo is %p, not this binding",
				static_cast<void*>(type->m_pClassInfo));
		}
		if (type->m_eTypeCategory != SCHEMA_TYPE_DECLARED_CLASS)
		{
			context.Fail("category is %s, not %s",
				TypeCategoryName(type->m_eTypeCategory),
				TypeCategoryName(SCHEMA_TYPE_DECLARED_CLASS));
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::GetClassModuleName", {
		const char* module = schema_->GetClassModuleName(probe_class_);
		context.Detail("-> \"%.80s\" for %s", SafeName(module), probe_class_name_);
		if (module == nullptr)
		{
			context.Fail("a registered class came from some module");
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::GetClassProjectName", {
		const char* project = schema_->GetClassProjectName(probe_class_);
		context.Detail("-> \"%.80s\" for %s", SafeName(project), probe_class_name_);
	});

	CS2ST_CALL(context, "ISchemaSystem::ScopedNameForClass", {
		// Heap allocation disabled: a fault mid-call skips every destructor
		// between there and the guard, and a stack-only buffer cannot leak.
		CBufferStringN<256> scoped(false);
		schema_->ScopedNameForClass(probe_class_, scoped);
		context.Detail("-> \"%.200s\" (%d/%d bytes)",
			scoped.Get(), scoped.Length(), scoped.AllocatedNum());
		if (scoped.Length() == 0)
		{
			context.Fail("a registered class always has a scoped name");
		}
		else if (std::strstr(scoped.Get(), probe_class_name_) == nullptr)
		{
			// Whatever the separator, the class name has to be in there.
			context.Fail("the scoped name does not contain \"%s\"", probe_class_name_);
		}
		else if (scoped.Length() > scoped.AllocatedNum())
		{
			context.Fail("length exceeds the allocation; CBufferString's layout does not hold");
		}
	});

	// Round trip: the scoped name the system just produced has to resolve back
	// to the same binding through the scoped-name lookup. Two functions, one
	// string, one object - and neither derived from the other.
	CS2ST_CALL(context, "ISchemaSystem::ScopedNameForClass -> FindClassByScopedName", {
		CBufferStringN<256> scoped(false);
		schema_->ScopedNameForClass(probe_class_, scoped);
		if (scoped.Length() == 0)
		{
			context.Detail("-> no scoped name to round trip");
			return;
		}

		SchemaMetaInfoHandle_t<CSchemaClassInfo> handle =
			schema_->FindClassByScopedName(scoped.Get());
		context.Detail("\"%.120s\" -> %p (expected %p)", scoped.Get(),
			static_cast<void*>(handle.Get()), static_cast<void*>(probe_class_));
		if (handle.Get() != probe_class_)
		{
			context.Fail("the scoped name does not resolve back to its own class");
		}
	});
}

// -----------------------------------------------------------------------------
//  Enum lookups - the same set of calls as for classes
//
//  Previously written off as "twins of the class calls", which was wrong: they
//  are separate vtable slots with separate implementations, and a slot shift
//  can hit one family and not the other.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseEnumLookups(TestContext& context)
{
	context.Note("[CSchemaSystemTypeScope] enum lookups");

	if (probe_scope_ == nullptr)
	{
		context.SkipCall("CSchemaSystemTypeScope::FindDeclaredEnum", "no type scope resolved");
		return;
	}

	for (const char* enum_name : kProbeEnumNames)
	{
		const std::string target =
			std::string("CSchemaSystemTypeScope::FindDeclaredEnum(\"") + enum_name + "\")";
		CS2ST_CALL(context, target, {
			SchemaMetaInfoHandle_t<CSchemaEnumInfo> handle =
				probe_scope_->FindDeclaredEnum(enum_name);
			CSchemaEnumInfo* info = handle.Get();
			if (info == nullptr)
			{
				context.Detail("-> (not in this scope)");
				return;
			}
			context.Detail("-> %p, %u byte(s), %u enumerator(s)", static_cast<void*>(info),
				static_cast<unsigned>(info->m_nSize),
				static_cast<unsigned>(info->m_nEnumeratorCount));
			if (probe_enum_ == nullptr)
			{
				probe_enum_ = info;
				probe_enum_name_ = enum_name;
			}
		});
	}

	// The fallback path: an enum harvested from a live class field. Better than
	// a guessed name, because it comes from data the game actually registered -
	// and it cross-validates CSchemaType_DeclaredEnum on the way.
	if (probe_enum_ == nullptr && probe_enum_type_ != nullptr)
	{
		CS2ST_CALL(context, "enum binding from a live class field", {
			CSchemaType_DeclaredEnum* declared =
				static_cast<CSchemaType_DeclaredEnum*>(probe_enum_type_);
			probe_enum_ = declared->m_pEnumInfo;
			probe_enum_name_ = (probe_enum_ != nullptr) ? probe_enum_->m_pszName : nullptr;
			context.Detail("-> %p \"%.60s\" via %s",
				static_cast<void*>(probe_enum_), SafeName(probe_enum_name_),
				SafeName(declared->m_sTypeName.Get()));
			if (probe_enum_ == nullptr)
			{
				context.Fail("a declared-enum type with no enum info behind it");
			}
		});
	}

	if (probe_enum_ == nullptr || probe_enum_name_ == nullptr)
	{
		context.SkipCall("CSchemaSystemTypeScope::FindRawEnumBinding",
			"no enum binding resolved, by name or from a class field");
		context.SkipCall("CSchemaSystemTypeScope::FindType_DeclaredEnum", "same reason");
		context.SkipCall("ISchemaSystem::ScopedNameForEnum", "same reason");
		context.SkipCall("ISchemaSystem::GetEnumModuleName", "same reason");
		context.SkipCall("ISchemaSystem::GetEnumProjectName", "same reason");
		context.SkipCall("ISchemaSystem::FindEnumByScopedName", "same reason");
		return;
	}

	CS2ST_CALL(context, "CSchemaSystemTypeScope::FindRawEnumBinding(name)", {
		CSchemaEnumInfo* raw = probe_scope_->FindRawEnumBinding(probe_enum_name_);
		context.Detail("-> %p for \"%.60s\" (we hold %p)",
			static_cast<void*>(raw), probe_enum_name_, static_cast<void*>(probe_enum_));
		if (raw != probe_enum_)
		{
			context.Fail("the raw lookup disagrees about the same enum name");
		}
	});

	CS2ST_CALL(context, "CSchemaSystemTypeScope::FindType_DeclaredEnum(name)", {
		CSchemaType_DeclaredEnum* type =
			probe_scope_->FindType_DeclaredEnum(probe_enum_name_);
		context.Detail("-> %p for \"%.60s\"", static_cast<void*>(type), probe_enum_name_);
		if (type == nullptr)
		{
			context.Fail("a declared enum always has a declared-enum type");
			return;
		}
		if (type->m_pEnumInfo != probe_enum_)
		{
			context.Fail("the type's m_pEnumInfo is %p, not this binding",
				static_cast<void*>(type->m_pEnumInfo));
		}
		if (type->m_eTypeCategory != SCHEMA_TYPE_DECLARED_ENUM)
		{
			context.Fail("category is %s, not %s",
				TypeCategoryName(type->m_eTypeCategory),
				TypeCategoryName(SCHEMA_TYPE_DECLARED_ENUM));
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::GetEnumModuleName", {
		const char* module = schema_->GetEnumModuleName(probe_enum_);
		context.Detail("-> \"%.80s\" for %.60s", SafeName(module), probe_enum_name_);
		if (module == nullptr)
		{
			context.Fail("a registered enum came from some module");
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::GetEnumProjectName", {
		const char* project = schema_->GetEnumProjectName(probe_enum_);
		context.Detail("-> \"%.80s\" for %.60s", SafeName(project), probe_enum_name_);
	});

	CS2ST_CALL(context, "ISchemaSystem::ScopedNameForEnum", {
		CBufferStringN<256> scoped(false);
		schema_->ScopedNameForEnum(probe_enum_, scoped);
		context.Detail("-> \"%.200s\" (%d/%d bytes)",
			scoped.Get(), scoped.Length(), scoped.AllocatedNum());
		if (scoped.Length() == 0)
		{
			context.Fail("a registered enum always has a scoped name");
		}
		else if (std::strstr(scoped.Get(), probe_enum_name_) == nullptr)
		{
			context.Fail("the scoped name does not contain \"%.60s\"", probe_enum_name_);
		}
	});

	CS2ST_CALL(context, "ISchemaSystem::ScopedNameForEnum -> FindEnumByScopedName", {
		CBufferStringN<256> scoped(false);
		schema_->ScopedNameForEnum(probe_enum_, scoped);
		if (scoped.Length() == 0)
		{
			context.Detail("-> no scoped name to round trip");
			return;
		}

		SchemaMetaInfoHandle_t<CSchemaEnumInfo> handle =
			schema_->FindEnumByScopedName(scoped.Get());
		context.Detail("\"%.120s\" -> %p (expected %p)", scoped.Get(),
			static_cast<void*>(handle.Get()), static_cast<void*>(probe_enum_));
		if (handle.Get() != probe_enum_)
		{
			context.Fail("the scoped name does not resolve back to its own enum");
		}
	});
}

// -----------------------------------------------------------------------------
//  Atomic type lookups
//
//  Previously skipped on the grounds that an atomic ID is unobtainable. It is
//  not: a class field whose type is atomic carries m_nAtomicID, so the ID comes
//  out of live data - and feeding it back gives another lookup with its own
//  oracle.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseAtomicLookups(TestContext& context)
{
	context.Note("[CSchemaSystemTypeScope] atomic type lookups");

	if (probe_scope_ == nullptr || probe_atomic_type_ == nullptr)
	{
		context.SkipCall("CSchemaSystemTypeScope::FindType_Atomic",
			"no atomic type among the probe class's fields, so no atomic ID to pass");
		return;
	}

	CSchemaType_Atomic* atomic = static_cast<CSchemaType_Atomic*>(probe_atomic_type_);

	CS2ST_CALL(context, "CSchemaType_Atomic (fields of a live atomic type)", {
		context.Detail("\"%.60s\" atomicID=%d size=%u alignment=%u info=%p category=%s",
			SafeName(atomic->m_sTypeName.Get()),
			atomic->m_nAtomicID,
			static_cast<unsigned>(atomic->m_nSize),
			static_cast<unsigned>(atomic->m_nAlignment),
			static_cast<void*>(atomic->m_pAtomicInfo),
			AtomicCategoryName(atomic->m_eAtomicCategory));

		if (atomic->m_nAtomicID < 0)
		{
			context.Fail("an atomic ID is an index; negative is impossible");
		}
		if (atomic->m_nSize == 0)
		{
			context.Fail("an atomic type occupies at least one byte");
		}
		if (!IsPlausibleAlignment(atomic->m_nAlignment))
		{
			context.Fail("%u is not a power-of-two alignment",
				static_cast<unsigned>(atomic->m_nAlignment));
		}
	});

	// Only the plain form is fed back. The _T / _TT / _CollectionOfT / _I
	// variants key on a template type as well, and passing the wrong pair would
	// miss and tell us nothing - the plain lookup is the one whose key is fully
	// known from the object in hand.
	if (atomic->m_eAtomicCategory == SCHEMA_ATOMIC_PLAIN)
	{
		CS2ST_CALL(context, "CSchemaSystemTypeScope::FindType_Atomic(id)", {
			SchemaMetaInfoHandle_t<CSchemaType_Atomic> handle =
				probe_scope_->FindType_Atomic(atomic->m_nAtomicID);
			context.Detail("-> %p for atomicID %d (we hold %p)",
				static_cast<void*>(handle.Get()), atomic->m_nAtomicID,
				static_cast<void*>(atomic));
			if (handle.Get() != atomic)
			{
				// A module-local atomic found through a different scope can
				// legitimately be a different object, hence Suspect not Fail.
				context.Suspect("the interned atomic for this ID is a different object");
			}
		});
	}
	else
	{
		context.SkipCall("CSchemaSystemTypeScope::FindType_Atomic",
			"the harvested atomic is category %s, whose lookup key needs a "
			"template type this probe cannot supply",
			AtomicCategoryName(atomic->m_eAtomicCategory));
	}

	if (atomic->m_pAtomicInfo != nullptr)
	{
		CS2ST_CALL(context, "SchemaAtomicTypeInfo_t (fields)", {
			const SchemaAtomicTypeInfo_t* info = atomic->m_pAtomicInfo;
			context.Detail("\"%.60s\" token \"%.40s\" id=%d meta=%d @%p",
				SafeName(info->m_pszName), SafeName(info->m_pszTokenName),
				info->m_nAtomicID, info->m_nStaticMetadataCount,
				static_cast<const void*>(info->m_pStaticMetadata));

			// The type and the info behind it have to agree about the ID.
			if (info->m_nAtomicID != atomic->m_nAtomicID)
			{
				context.Fail("the type says ID %d, its info says %d",
					atomic->m_nAtomicID, info->m_nAtomicID);
			}
			if (info->m_nStaticMetadataCount < 0)
			{
				context.Fail("a metadata count cannot be negative");
			}
			if ((info->m_nStaticMetadataCount > 0) != (info->m_pStaticMetadata != nullptr))
			{
				context.Fail("count %d but array %p", info->m_nStaticMetadataCount,
					static_cast<const void*>(info->m_pStaticMetadata));
			}
		});

		StructInspector inspect(context, "SchemaAtomicTypeInfo_t",
			atomic->m_pAtomicInfo, sizeof(SchemaAtomicTypeInfo_t));
		inspect.String ("m_pszName",         offsetof(SchemaAtomicTypeInfo_t, m_pszName));
		inspect.String ("m_pszTokenName",    offsetof(SchemaAtomicTypeInfo_t, m_pszTokenName));
		inspect.Int32  ("m_nAtomicID",       offsetof(SchemaAtomicTypeInfo_t, m_nAtomicID), 0, INT32_MAX);
		inspect.Int32  ("m_nStaticMetadataCount",
		                                     offsetof(SchemaAtomicTypeInfo_t, m_nStaticMetadataCount),
		                                     0, INT32_MAX);
		inspect.Pointer("m_pStaticMetadata", offsetof(SchemaAtomicTypeInfo_t, m_pStaticMetadata));
		inspect.Summarize();
	}
	else
	{
		context.SkipCall("SchemaAtomicTypeInfo_t", "the atomic type carries no info pointer");
	}
}

// -----------------------------------------------------------------------------
//  The class graph - read-only enumerations that were wrongly skipped
//
//  Both of these fill a CUtlVector we own. That is safe across the module
//  boundary for a non-obvious reason worth writing down: CUtlVector allocates
//  through g_pMemAlloc, which resolves to the one shared tier0 allocator in
//  both the plugin and the engine, so the engine growing a vector this suite
//  later destroys is not a cross-allocator free. It has nothing to do with
//  memoverride.cpp, which only routes global new/delete.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ExerciseClassGraph(TestContext& context)
{
	context.Note("[ISchemaSystem] class graph queries");

	if (probe_class_ == nullptr)
	{
		context.SkipCall("ISchemaSystem::FindDescendentsOfClass", "no probe class resolved");
		context.SkipCall("ISchemaSystem::FindClassesByMeta", "no probe class resolved");
		return;
	}

	CS2ST_CALL(context, "ISchemaSystem::FindDescendentsOfClass", {
		// The second parameter is an unnamed int in the SDK. Both this function
		// and FindClassesByMeta take a hierarchy-iteration argument in the same
		// position, so SCHEMA_ITER_NONE (0) is the conservative reading; the
		// trailing bool is likewise unnamed and passed false.
		CUtlVector<const CSchemaClassInfo*> descendents;
		schema_->FindDescendentsOfClass(probe_class_,
			static_cast<int>(SCHEMA_ITER_NONE), &descendents, false);

		const int count = descendents.Count();
		context.Detail("-> %d descendent(s) of %s", count, probe_class_name_);
		if (count < 0 || count > kMaxPlausibleCount)
		{
			context.Fail("%d is not a count", count);
			return;
		}

		const int shown = (count < kMaxEntriesPrinted) ? count : kMaxEntriesPrinted;
		for (int i = 0; i < shown; ++i)
		{
			const CSchemaClassInfo* info = descendents[i];
			context.Note("    [%d] %.70s (%d bytes)", i,
				info != nullptr ? SafeName(info->m_pszName) : "(null)",
				info != nullptr ? info->m_nSize : -1);
		}
		if (count > shown)
		{
			context.Note("    ... and %d more", count - shown);
		}
	});

	for (const char* tag : kProbeMetaTags)
	{
		const std::string target =
			std::string("ISchemaSystem::FindClassesByMeta(\"") + tag + "\")";
		CS2ST_CALL(context, target, {
			CUtlVector<const CSchemaClassInfo*> classes;
			schema_->FindClassesByMeta(tag, SCHEMA_ITER_SINGLE_PARENT, &classes);

			const int count = classes.Count();
			context.Detail("-> %d class(es) tagged \"%s\"", count, tag);
			if (count < 0 || count > kMaxPlausibleCount)
			{
				context.Fail("%d is not a count", count);
				return;
			}

			// The tag strings are our guess at what the game annotates with, so
			// an empty result is recorded, not failed.
			const int shown = (count < kMaxEntriesPrinted) ? count : kMaxEntriesPrinted;
			for (int i = 0; i < shown; ++i)
			{
				const CSchemaClassInfo* info = classes[i];
				context.Note("    [%d] %.70s", i,
					info != nullptr ? SafeName(info->m_pszName) : "(null)");
			}
			if (count > shown)
			{
				context.Note("    ... and %d more", count - shown);
			}
		});
	}
}

// -----------------------------------------------------------------------------
//  CSchemaType's virtual table
//
//  Fifteen virtuals that were not touched at all before, on objects this suite
//  already has in hand. Several have a single correct answer derived from the
//  object's own fields, which makes them oracle checks rather than liveness
//  checks: a type must recognise its own name, its own category, and itself.
//
//  Same destructor caveat as ISchemaSystem: the SDK declares
//  `virtual ~CSchemaType() = 0` last, and IsValid() is slot 0. If Valve's class
//  declares the destructor first, this is where the process ends.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ProbeSchemaType(TestContext& context, const char* label,
                                        CSchemaType* type)
{
	if (type == nullptr)
	{
		context.SkipCall(std::string("CSchemaType (") + label + ")",
			"no instance of this category among the probe class's fields");
		return;
	}

	const std::string prefix = std::string("CSchemaType[") + label + "]::";

	CS2ST_CALL(context, prefix + "<fields>", {
		context.Detail("\"%.70s\" scope=%p category=%s atomic=%s",
			SafeName(type->m_sTypeName.Get()),
			static_cast<void*>(type->m_pTypeScope),
			TypeCategoryName(type->m_eTypeCategory),
			AtomicCategoryName(type->m_eAtomicCategory));

		if (type->m_sTypeName.Get() == nullptr || type->m_sTypeName.Get()[0] == '\0')
		{
			context.Fail("an interned type always has a name");
		}
		if (static_cast<int>(type->m_eTypeCategory) > static_cast<int>(SCHEMA_TYPE_INVALID))
		{
			context.Fail("category %d is outside the enum",
				static_cast<int>(type->m_eTypeCategory));
		}
		if (static_cast<int>(type->m_eAtomicCategory) > static_cast<int>(SCHEMA_ATOMIC_INVALID))
		{
			context.Fail("atomic category %d is outside the enum",
				static_cast<int>(type->m_eAtomicCategory));
		}
	});

	// Slot 0. If the vtable is shifted by a destructor, this call is the one
	// that does not come back.
	CS2ST_CALL(context, prefix + "IsValid", {
		const bool valid = type->IsValid();
		context.Detail("-> %s", valid ? "true" : "false");
		if (!valid)
		{
			context.Fail("a type the scope handed out describes itself as invalid");
		}
	});

	CS2ST_CALL(context, prefix + "ToString", {
		CBufferStringN<256> text(false);
		const char* returned = type->ToString(text, false);
		context.Detail("-> \"%.150s\" (returned %p, buffer %d/%d bytes)",
			text.Get(), static_cast<const void*>(returned),
			text.Length(), text.AllocatedNum());
		if (text.Length() == 0)
		{
			context.Fail("a type renders to some text");
		}
		if (text.Length() > text.AllocatedNum())
		{
			context.Fail("length exceeds the allocation; CBufferString's layout does not hold");
		}
	});

	CS2ST_CALL(context, prefix + "GetSizeAndAlignment", {
		// Seeded with values the engine would have to overwrite, so "did not
		// write" is distinguishable from "wrote a zero".
		//
		// uint8, not std::uint8_t: GetSizeAndAlignment takes a uint8& and a
		// reference parameter needs the exact type. Everything else in this
		// file uses the <cstdint> names; these two out-params cannot.
		int size = -31337;
		uint8 alignment = 0xAB;
		const bool ok = type->GetSizeAndAlignment(size, alignment);
		context.Detail("-> %s, size=%d alignment=%u",
			ok ? "true" : "false", size, static_cast<unsigned>(alignment));

		if (!ok)
		{
			// Legitimate for some categories - a bitfield or an unresolved
			// type has no standalone size - so this is recorded, not failed.
			context.Detail("-> false (this category has no standalone size)");
			return;
		}
		if (size == -31337 || alignment == 0xAB)
		{
			context.Fail("returned true without writing both out parameters");
		}
		if (size <= 0)
		{
			context.Fail("a sized type occupies at least one byte");
		}
		if (!IsPlausibleAlignment(alignment))
		{
			context.Fail("%u is not a power-of-two alignment", static_cast<unsigned>(alignment));
		}
	});

	// The type's own name, handed back to the name matcher.
	//
	// This was written as "exactly one correct answer" and that was wrong:
	// m_sTypeName is a *rendered* name, not a lookup key. The simple categories
	// carry the key verbatim ("int32", "CBaseEntity") and do match; the
	// composed ones do not - a pointer renders as "CBodyComponent*" and an
	// atomic as "CUtlVector< thinkfunc_t >", while IsTypeByName is keyed on the
	// name underneath. Measured: both composed categories answer false to their
	// own rendered name, which the first version reported as a FAIL.
	//
	// The composed categories now get a second probe with the name the rule
	// predicts, which turns a bogus FAIL into a row saying what the matcher is
	// actually keyed on. A category whose rendered name *is* its key still
	// fails if it does not recognise it - that part was a real check.
	CS2ST_CALL(context, prefix + "IsTypeByName(its own name)", {
		const char* own_name = type->m_sTypeName.Get();
		if (own_name == nullptr)
		{
			context.Detail("-> no name to test with");
			return;
		}

		const bool matches = type->IsTypeByName(own_name);

		// The name the composed renderings are built from. Stays null for
		// every other category, which is what selects the strict check below.
		const char* inner_name = nullptr;
		const char* inner_what = nullptr;

		// Cast on m_eTypeCategory alone rather than through ReinterpretAs<T>().
		// ReinterpretAs goes via SchemaTypeMap<T>::Match, which compares
		// m_eAtomicCategory as well - and CSchemaType_Atomic is registered as
		// SCHEMA_ATOMIC_PLAIN, so asking it about a CUtlVector (which is
		// SCHEMA_ATOMIC_COLLECTION_OF_T) returns null and this probe would fall
		// straight back into the strict branch it exists to avoid. The type
		// category on its own does fix the class family: every SCHEMA_TYPE_ATOMIC
		// object is one of the five CSchemaType_Atomic* classes, all of which
		// derive from CSchemaType_Atomic by single inheritance, and
		// SCHEMA_TYPE_POINTER is CSchemaType_Ptr and nothing else.
		if (type->m_eTypeCategory == SCHEMA_TYPE_POINTER)
		{
			const CSchemaType_Ptr* pointer = static_cast<const CSchemaType_Ptr*>(type);
			if (pointer->m_pObjectType != nullptr)
			{
				inner_name = pointer->m_pObjectType->m_sTypeName.Get();
				inner_what = "pointee";
			}
		}
		else if (type->m_eTypeCategory == SCHEMA_TYPE_ATOMIC)
		{
			const CSchemaType_Atomic* atomic = static_cast<const CSchemaType_Atomic*>(type);
			if (atomic->m_pAtomicInfo != nullptr)
			{
				inner_name = atomic->m_pAtomicInfo->m_pszName;
				inner_what = "atomic";
			}
		}

		if (inner_name == nullptr)
		{
			context.Detail("\"%.70s\" -> %s", own_name, matches ? "true" : "false");
			if (!matches)
			{
				context.Fail("a type whose name is not composed does not recognise it");
			}
			return;
		}

		const bool inner_matches = type->IsTypeByName(inner_name);
		context.Detail("rendered \"%.50s\" -> %s | %s \"%.40s\" -> %s",
			own_name, matches ? "true" : "false",
			inner_what, inner_name, inner_matches ? "true" : "false");

		if (!matches && !inner_matches)
		{
			// Neither the rendered name nor the one the rendering is built
			// from. Suspect rather than Fail: which key IsTypeByName uses is
			// nowhere in the SDK, so a third answer is a gap in what we know
			// before it is a broken matcher. It still gets a coloured row,
			// because the negative probe below is then the only thing left
			// saying the matcher works at all.
			context.Suspect("recognises neither its rendered name nor its %s name",
				inner_what);
		}
	});

	CS2ST_CALL(context, prefix + "IsTypeByName(a name it cannot have)", {
		// The negative case. A matcher that says yes to everything is as broken
		// as one that says no to everything, and only this catches the former.
		const bool matches = type->IsTypeByName("Cs2SdkTestNoSuchTypeName999");
		context.Detail("-> %s", matches ? "true" : "false");
		if (matches)
		{
			context.Fail("the type matched a name that does not exist");
		}
	});

	CS2ST_CALL(context, prefix + "IsA(itself)", {
		// The SDK's comment says IsA returns true if the argument is this
		// object, so passing itself has one correct answer.
		const bool self = type->IsA(type);
		context.Detail("-> %s", self ? "true" : "false");
		if (!self)
		{
			context.Fail("a type is not itself");
		}
	});

	CS2ST_CALL(context, prefix + "IsTypeByType(itself)", {
		const bool same = type->IsTypeByType(type);
		context.Detail("-> %s", same ? "true" : "false");
		if (!same)
		{
			context.Fail("a type's traits do not match its own");
		}
	});

	CS2ST_CALL(context, prefix + "CanReinterpretAs(itself)", {
		// Recorded rather than judged: "reinterpret" has looser semantics than
		// identity and the SDK does not say whether the reflexive case is
		// guaranteed. Writing down the answer now is what makes a change to it
		// visible later.
		context.Detail("-> %s", type->CanReinterpretAs(type) ? "true" : "false");
	});

	CS2ST_CALL(context, prefix + "IsBuiltinType", {
		// For a builtin the object's own m_eBuiltinType is the right answer to
		// pass; for anything else the correct answer is false for every input,
		// and INT32 is as good a probe as any.
		if (type->m_eTypeCategory == SCHEMA_TYPE_BUILTIN)
		{
			CSchemaType_Builtin* builtin = static_cast<CSchemaType_Builtin*>(type);
			const bool matches = type->IsBuiltinType(builtin->m_eBuiltinType);
			context.Detail("(%d) -> %s", static_cast<int>(builtin->m_eBuiltinType),
				matches ? "true" : "false");
			if (!matches)
			{
				context.Fail("a builtin does not recognise its own builtin type");
			}
		}
		else
		{
			const bool matches = type->IsBuiltinType(SCHEMA_BUILTIN_TYPE_INT32);
			context.Detail("(INT32) -> %s on a %s type", matches ? "true" : "false",
				TypeCategoryName(type->m_eTypeCategory));
			if (matches)
			{
				context.Fail("a %s type claims to be a builtin int32",
					TypeCategoryName(type->m_eTypeCategory));
			}
		}
	});

	CS2ST_CALL(context, prefix + "IsDeclaredClass", {
		if (type->m_eTypeCategory == SCHEMA_TYPE_DECLARED_CLASS)
		{
			CSchemaType_DeclaredClass* declared =
				static_cast<CSchemaType_DeclaredClass*>(type);
			const char* name = (declared->m_pClassInfo != nullptr)
				? declared->m_pClassInfo->m_pszName : nullptr;
			if (name == nullptr)
			{
				context.Detail("-> no class name to test with");
				return;
			}
			const bool matches = type->IsDeclaredClass(name);
			context.Detail("(\"%.60s\") -> %s", name, matches ? "true" : "false");
			if (!matches)
			{
				context.Fail("a declared-class type does not recognise its own class");
			}
		}
		else
		{
			const bool matches = type->IsDeclaredClass("CBaseEntity");
			context.Detail("(\"CBaseEntity\") -> %s on a %s type",
				matches ? "true" : "false", TypeCategoryName(type->m_eTypeCategory));
			if (matches)
			{
				context.Fail("a %s type claims to be the class CBaseEntity",
					TypeCategoryName(type->m_eTypeCategory));
			}
		}
	});

	CS2ST_CALL(context, prefix + "IsDeclaredEnum", {
		if (type->m_eTypeCategory == SCHEMA_TYPE_DECLARED_ENUM)
		{
			CSchemaType_DeclaredEnum* declared =
				static_cast<CSchemaType_DeclaredEnum*>(type);
			const char* name = (declared->m_pEnumInfo != nullptr)
				? declared->m_pEnumInfo->m_pszName : nullptr;
			if (name == nullptr)
			{
				context.Detail("-> no enum name to test with");
				return;
			}
			const bool matches = type->IsDeclaredEnum(name);
			context.Detail("(\"%.60s\") -> %s", name, matches ? "true" : "false");
			if (!matches)
			{
				context.Fail("a declared-enum type does not recognise its own enum");
			}
		}
		else
		{
			const bool matches = type->IsDeclaredEnum("MoveType_t");
			context.Detail("(\"MoveType_t\") -> %s on a %s type",
				matches ? "true" : "false", TypeCategoryName(type->m_eTypeCategory));
			if (matches)
			{
				context.Fail("a %s type claims to be the enum MoveType_t",
					TypeCategoryName(type->m_eTypeCategory));
			}
		}
	});

	CS2ST_CALL(context, prefix + "IsAtomicType", {
		if (type->m_eTypeCategory == SCHEMA_TYPE_ATOMIC)
		{
			CSchemaType_Atomic* atomic = static_cast<CSchemaType_Atomic*>(type);
			const bool matches = type->IsAtomicType(atomic->m_nAtomicID);
			context.Detail("(%d) -> %s", atomic->m_nAtomicID, matches ? "true" : "false");
			if (!matches)
			{
				context.Fail("an atomic type does not recognise its own atomic ID");
			}
		}
		else
		{
			// A negative ID is not a valid atomic ID for anything, so false is
			// the only correct answer regardless of category.
			const bool matches = type->IsAtomicType(-1);
			context.Detail("(-1) -> %s", matches ? "true" : "false");
			if (matches)
			{
				context.Fail("the type matched atomic ID -1");
			}
		}
	});

	CS2ST_CALL(context, prefix + "GetInnerType", {
		SchemaMetaInfoHandle_t<CSchemaType> inner = type->GetInnerType();
		CSchemaType* value = inner.Get();
		context.Detail("-> %p%s", static_cast<void*>(value),
			value != nullptr ? "" : " (none, as a scalar type has)");

		// A pointer type wraps something by definition; nothing else owes an
		// inner type. That makes the pointer case an assertion and the rest a
		// recording.
		if (value == nullptr && type->m_eTypeCategory == SCHEMA_TYPE_POINTER)
		{
			context.Fail("a pointer type has nothing inside it");
		}
		if (value == type)
		{
			context.Fail("a type's inner type is itself; that cannot terminate");
		}
	});

	CS2ST_CALL(context, prefix + "GetInnermostType", {
		SchemaMetaInfoHandle_t<CSchemaType> innermost = type->GetInnermostType();
		CSchemaType* value = innermost.Get();
		context.Detail("-> %p \"%.60s\"", static_cast<void*>(value),
			value != nullptr ? SafeName(value->m_sTypeName.Get()) : "");
		if (value == nullptr && type->m_eTypeCategory == SCHEMA_TYPE_POINTER)
		{
			context.Fail("a pointer type has nothing at the bottom of it");
		}
	});

	CS2ST_CALL(context, prefix + "InternalMatchInnerAs(its own categories)", {
		// Internal, and the SDK gives no contract for the reflexive case, so
		// this is recorded rather than judged - the value of the row is that a
		// change to the answer becomes visible.
		CSchemaType* matched = type->InternalMatchInnerAs(
			type->m_eTypeCategory, type->m_eAtomicCategory);
		context.Detail("-> %p%s", static_cast<void*>(matched),
			matched == type ? " (itself)" : "");
	});

	CS2ST_CALL(context, prefix + "DependsOnlyOnUnresolvedOrGlobalTypes", {
		// A predicate about where a type's dependencies live. No single correct
		// answer, so recorded; asked against the scope the type says it belongs
		// to rather than a scope of our choosing.
		context.Detail("-> %s",
			type->DependsOnlyOnUnresolvedOrGlobalTypes(type->m_pTypeScope)
				? "true" : "false");
	});

	context.SkipCall(prefix + "SpewDescription",
		"writes the type's full description to a logging channel; console spew "
		"with nothing this suite can read back");

	ProbeSchemaTypeDowncasts(context, label, type);
}

// -----------------------------------------------------------------------------
//  IsA<T>() / ReinterpretAs<T>()
//
//  The eleven concrete schema types, each asked of one instance. Two things are
//  checked per row, and both have exactly one correct answer:
//
//    - IsA<T>() must be true for precisely the T matching the object's own
//      (m_eTypeCategory, m_eAtomicCategory) pair, and false for the other ten.
//      A downcast helper that says yes to everything is as broken as one that
//      says no to everything, and only testing all eleven catches the former.
//
//    - ReinterpretAs<T>() must return this object when IsA<T>() is true and
//      null when it is false. It is defined in terms of IsA, so a disagreement
//      means the templates were instantiated against different category
//      constants - which is what a reordered SchemaTypeCategory_t enum does.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::ProbeSchemaTypeDowncasts(TestContext& context, const char* label,
                                                 CSchemaType* type)
{
	if (type == nullptr)
	{
		return;
	}

	const std::string target =
		std::string("CSchemaType[") + label + "]::IsA<T> / ReinterpretAs<T>";

	CS2ST_CALL(context, target, {
		DowncastTally tally;

		// One row per SCHEMATYPE_ENTRY registration in schematypes.h. The
		// category pair passed alongside each type is the one that macro
		// registered for it, so these eleven lines mirror that table exactly -
		// and a row that stops agreeing is a row where the table moved.
		CheckDowncast<CSchemaType_Builtin>(context, type, "CSchemaType_Builtin",
			SCHEMA_TYPE_BUILTIN, SCHEMA_ATOMIC_INVALID, tally);
		CheckDowncast<CSchemaType_Ptr>(context, type, "CSchemaType_Ptr",
			SCHEMA_TYPE_POINTER, SCHEMA_ATOMIC_INVALID, tally);
		CheckDowncast<CSchemaType_Atomic>(context, type, "CSchemaType_Atomic",
			SCHEMA_TYPE_ATOMIC, SCHEMA_ATOMIC_PLAIN, tally);
		CheckDowncast<CSchemaType_Atomic_T>(context, type, "CSchemaType_Atomic_T",
			SCHEMA_TYPE_ATOMIC, SCHEMA_ATOMIC_T, tally);
		CheckDowncast<CSchemaType_Atomic_CollectionOfT>(context, type,
			"CSchemaType_Atomic_CollectionOfT",
			SCHEMA_TYPE_ATOMIC, SCHEMA_ATOMIC_COLLECTION_OF_T, tally);
		CheckDowncast<CSchemaType_Atomic_TT>(context, type, "CSchemaType_Atomic_TT",
			SCHEMA_TYPE_ATOMIC, SCHEMA_ATOMIC_TT, tally);
		CheckDowncast<CSchemaType_Atomic_I>(context, type, "CSchemaType_Atomic_I",
			SCHEMA_TYPE_ATOMIC, SCHEMA_ATOMIC_I, tally);
		CheckDowncast<CSchemaType_DeclaredClass>(context, type, "CSchemaType_DeclaredClass",
			SCHEMA_TYPE_DECLARED_CLASS, SCHEMA_ATOMIC_INVALID, tally);
		CheckDowncast<CSchemaType_DeclaredEnum>(context, type, "CSchemaType_DeclaredEnum",
			SCHEMA_TYPE_DECLARED_ENUM, SCHEMA_ATOMIC_INVALID, tally);
		CheckDowncast<CSchemaType_FixedArray>(context, type, "CSchemaType_FixedArray",
			SCHEMA_TYPE_FIXED_ARRAY, SCHEMA_ATOMIC_INVALID, tally);
		CheckDowncast<CSchemaType_Bitfield>(context, type, "CSchemaType_Bitfield",
			SCHEMA_TYPE_BITFIELD, SCHEMA_ATOMIC_INVALID, tally);

		context.Detail("%s/%s -> matched %s (%d of 11), %d disagreement(s)",
			TypeCategoryName(type->m_eTypeCategory),
			AtomicCategoryName(type->m_eAtomicCategory),
			tally.matched_name, tally.matched, tally.disagreements);

		// The SCHEMATYPE_ENTRY table maps each category pair to exactly one
		// concrete type, so a well-formed type matches precisely one row.
		// Zero means the object's category pair is not in the table at all -
		// which is what a newly added category looks like.
		if (tally.matched == 0)
		{
			context.Suspect("no concrete type claims the %s/%s pair; Valve may "
			                "have added a category the SDK does not map",
				TypeCategoryName(type->m_eTypeCategory),
				AtomicCategoryName(type->m_eAtomicCategory));
		}
		else if (tally.matched > 1)
		{
			context.Fail("%d concrete types claim the same object", tally.matched);
		}
	});
}

void SchemaSystemSuite::ExerciseSchemaTypeVirtuals(TestContext& context)
{
	context.Note("[CSchemaType] virtual table, one instance per category");

	// A builtin first: everything about it is known in advance, which makes it
	// the cleanest instance to judge the vtable against.
	ProbeSchemaType(context, "builtin", probe_builtin_type_);
	ProbeSchemaType(context, "class", probe_class_type_);
	ProbeSchemaType(context, "enum", probe_enum_type_);
	ProbeSchemaType(context, "atomic", probe_atomic_type_);
	ProbeSchemaType(context, "pointer", probe_pointer_type_);

	// Whatever the first field's type was, if it is not one of the above.
	if (probe_type_ != nullptr && probe_type_ != probe_builtin_type_
		&& probe_type_ != probe_class_type_ && probe_type_ != probe_enum_type_
		&& probe_type_ != probe_atomic_type_ && probe_type_ != probe_pointer_type_)
	{
		ProbeSchemaType(context, "first field", probe_type_);
	}
}

// -----------------------------------------------------------------------------
//  CSchemaSystemTypeScope - the struct
//
//  Worth the most of any layout check in the project. A concrete class with
//  roughly twenty thousand bytes of members, and the two fields plugins care
//  about most - m_ClassBindings and m_EnumBindings - sit at the very end,
//  downstream of every other field. Reading a sane count out of them means the
//  entire declaration in front of them is right.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectTypeScopeLayout(TestContext& context)
{
	context.Note("[CSchemaSystemTypeScope] struct layout (SDK sizeof: %zu bytes)",
		sizeof(CSchemaSystemTypeScope));

	if (probe_scope_ == nullptr)
	{
		context.SkipCall("CSchemaSystemTypeScope (layout)", "no type scope was resolved");
		return;
	}

	// The same string reachable two ways: through a virtual call, and as a char
	// array at an offset the SDK computed. Neither is derived from the other.
	CS2ST_CALL(context, "CSchemaSystemTypeScope::m_szScopeName vs GetScopeName()", {
		const char* from_call = probe_scope_->GetScopeName();
		const char* from_field = reinterpret_cast<const char*>(
			reinterpret_cast<const unsigned char*>(probe_scope_)
			+ offsetof(CSchemaSystemTypeScope, m_szScopeName));

		context.Detail("call \"%.100s\" vs field@0x%zx \"%.100s\"",
			SafeName(from_call),
			static_cast<std::size_t>(offsetof(CSchemaSystemTypeScope, m_szScopeName)),
			from_field);

		if (from_call == nullptr)
		{
			context.Fail("GetScopeName returned null; nothing to compare against");
		}
		else if (std::strncmp(from_call, from_field,
			sizeof(CSchemaSystemTypeScope::m_szScopeName) - 1) != 0)
		{
			context.Fail("the name at the SDK's offset is not the name the scope reports");
		}
	});

	// ISchemaSystemTypeScope, the base, declares no data members.
	context.Note("  ISchemaSystemTypeScope declares no data members; nothing to "
	             "inspect above CSchemaSystemTypeScope");

	StructInspector inspect(context, "CSchemaSystemTypeScope",
		probe_scope_, sizeof(CSchemaSystemTypeScope));

	// m_szScopeName is a char[256], not a pointer, so String() does not apply -
	// it has its own oracle above. Only its first bytes are echoed here so the
	// log reads down the struct in declaration order.
	inspect.Raw    ("m_szScopeName[0..15]", offsetof(CSchemaSystemTypeScope, m_szScopeName), 16);
	inspect.Pointer("m_pGlobalTypeScope",   offsetof(CSchemaSystemTypeScope, m_pGlobalTypeScope));
	inspect.Bool   ("m_bBuiltinTypesInitialized",
	                                        offsetof(CSchemaSystemTypeScope, m_bBuiltinTypesInitialized));

	// One entry out of the builtin array rather than all fourteen: it is an
	// array of a polymorphic type, so its first eight bytes are a vtable
	// pointer, and a single Pointer() check gives the same evidence the whole
	// array would at a fourteenth of the log. The elements themselves are
	// verified far more strongly in ExerciseBuiltinTypes.
	inspect.Pointer("m_BuiltinTypes[0].vptr",
	                                        offsetof(CSchemaSystemTypeScope, m_BuiltinTypes));

	// The eleven interned-type maps. Each is a CSchemaPtrMap, so only its
	// leading bytes are dumped here; their counts - which is what actually
	// validates the middle of the struct - are read in InspectInternedTypeMaps.
	inspect.Raw    ("m_Pointers",           offsetof(CSchemaSystemTypeScope, m_Pointers), 16);
	inspect.Raw    ("m_Atomics",            offsetof(CSchemaSystemTypeScope, m_Atomics), 16);
	inspect.Raw    ("m_AtomicsT",           offsetof(CSchemaSystemTypeScope, m_AtomicsT), 16);
	inspect.Raw    ("m_AtomicsCollectionOfT",
	                                        offsetof(CSchemaSystemTypeScope, m_AtomicsCollectionOfT), 16);
	inspect.Raw    ("m_AtomicsTT",          offsetof(CSchemaSystemTypeScope, m_AtomicsTT), 16);
	inspect.Raw    ("m_AtomicsI",           offsetof(CSchemaSystemTypeScope, m_AtomicsI), 16);
	inspect.Raw    ("m_DeclaredClasses",    offsetof(CSchemaSystemTypeScope, m_DeclaredClasses), 16);
	inspect.Raw    ("m_DeclaredEnums",      offsetof(CSchemaSystemTypeScope, m_DeclaredEnums), 16);
	inspect.Raw    ("m_AtomicInfos",        offsetof(CSchemaSystemTypeScope, m_AtomicInfos), 16);
	inspect.Raw    ("m_FixedArrays",        offsetof(CSchemaSystemTypeScope, m_FixedArrays), 16);
	inspect.Raw    ("m_Bitfields",          offsetof(CSchemaSystemTypeScope, m_Bitfields), 16);

	inspect.Raw    ("m_ClassBindings",      offsetof(CSchemaSystemTypeScope, m_ClassBindings), 16);
	inspect.Raw    ("m_EnumBindings",       offsetof(CSchemaSystemTypeScope, m_EnumBindings), 16);
	inspect.Summarize();

	CS2ST_CALL(context, "CSchemaSystemTypeScope::m_ClassBindings.Count", {
		// CUtlTSHash::Count() is inline and reads a field near the end of a
		// ~20 KB struct. A number in the thousands here is the single best
		// piece of evidence that this declaration is correct end to end;
		// 2^31 means it is not.
		const int count = probe_scope_->m_ClassBindings.Count();
		context.Detail("-> %d class binding(s)", count);
		if (count <= 0)
		{
			context.Fail("a live type scope holds class bindings");
		}
		else if (count > kMaxPlausibleCount)
		{
			context.Fail("%d is not a count; the struct tail is misplaced", count);
		}
	});

	CS2ST_CALL(context, "CSchemaSystemTypeScope::m_EnumBindings.Count", {
		const int count = probe_scope_->m_EnumBindings.Count();
		context.Detail("-> %d enum binding(s)", count);
		if (count < 0 || count > kMaxPlausibleCount)
		{
			context.Fail("%d is not a count; the struct tail is misplaced", count);
		}
	});

	// A count on its own could be a coincidence. Asking the hash to hand back
	// actual handles and resolving each to a named binding is what turns it
	// into a fact - and it is the enumeration a plugin author would most want
	// after an update, because the contents move when the game moves.
	CS2ST_CALL(context, "CSchemaSystemTypeScope::m_ClassBindings.GetElements", {
		const int total = probe_scope_->m_ClassBindings.Count();
		if (total <= 0 || total > kMaxPlausibleCount)
		{
			context.Detail("-> skipped, the count above is not usable");
			return;
		}

		const int wanted = (total < kMaxEntriesPrinted) ? total : kMaxEntriesPrinted;
		UtlTSHashHandle_t handles[kMaxEntriesPrinted] = {};
		const int got = probe_scope_->m_ClassBindings.GetElements(0, wanted, handles);
		context.Detail("-> asked for %d of %d, got %d", wanted, total, got);

		if (got <= 0)
		{
			context.Fail("the hash reports %d entries but hands back none", total);
			return;
		}
		if (got > wanted)
		{
			context.Fail("returned %d handles for a %d-element request", got, wanted);
			return;
		}

		for (int i = 0; i < got; ++i)
		{
			CSchemaClassInfo* info = probe_scope_->m_ClassBindings.Element(handles[i]);
			context.Note("    [%d] %p %.70s", i, static_cast<void*>(info),
				info != nullptr ? SafeName(info->m_pszName) : "(null)");
			if (info == nullptr)
			{
				context.Fail("entry %d resolves to a null binding", i);
			}
		}
	});
}

// -----------------------------------------------------------------------------
//  The interned-type maps
//
//  The previous version of this suite checked the head and the tail of
//  CSchemaSystemTypeScope and left the eleven kilobytes in between unvalidated
//  - which is exactly the region where an inserted field hides. Each map's
//  Count() is a field read at a distinct offset through that middle, so a run
//  of plausible counts pins the whole span rather than just its ends.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectInternedTypeMaps(TestContext& context)
{
	context.Note("[CSchemaSystemTypeScope] interned type maps (validates the "
	             "middle of the struct, not just its ends)");

	if (probe_scope_ == nullptr)
	{
		context.SkipCall("CSchemaPtrMap::Count", "no type scope was resolved");
		return;
	}

	// Written as one guarded call per map so a fault names the map it happened
	// in - which, given they are laid out in order, names the offset too.
#define CS2ST_MAP_COUNT(member)                                                       \
	CS2ST_CALL(context, "CSchemaSystemTypeScope::" #member ".m_Map.Count", {           \
		const int count = probe_scope_->member.m_Map.Count();                          \
		context.Detail("@0x%zx -> %d",                                                 \
			static_cast<std::size_t>(offsetof(CSchemaSystemTypeScope, member)), count); \
		if (count < 0 || count > kMaxPlausibleCount)                                   \
		{                                                                              \
			context.Fail("%d is not a count; this offset is not that map", count);      \
		}                                                                              \
	})

	CS2ST_MAP_COUNT(m_Pointers);
	CS2ST_MAP_COUNT(m_Atomics);
	CS2ST_MAP_COUNT(m_AtomicsT);
	CS2ST_MAP_COUNT(m_AtomicsCollectionOfT);
	CS2ST_MAP_COUNT(m_AtomicsTT);
	CS2ST_MAP_COUNT(m_AtomicsI);
	CS2ST_MAP_COUNT(m_DeclaredClasses);
	CS2ST_MAP_COUNT(m_DeclaredEnums);
	CS2ST_MAP_COUNT(m_AtomicInfos);
	CS2ST_MAP_COUNT(m_FixedArrays);
	CS2ST_MAP_COUNT(m_Bitfields);

#undef CS2ST_MAP_COUNT

	// m_DeclaredClasses is keyed by class ID and holds the interned
	// declared-class types. Walking it does two things at once: it proves the
	// map is a real ordered map rather than a plausible-looking count, and it
	// is the only route this plugin has to a class ID - which is the key the
	// FindRawClassBinding(uint) overload needs and could not otherwise be
	// tested at all.
	CS2ST_CALL(context, "CSchemaSystemTypeScope::m_DeclaredClasses (walk)", {
		const int count = probe_scope_->m_DeclaredClasses.m_Map.Count();
		if (count <= 0 || count > kMaxPlausibleCount)
		{
			context.Detail("-> skipped, the count above is not usable");
			return;
		}

		const int shown = (count < kMaxEntriesPrinted) ? count : kMaxEntriesPrinted;
		int walked = 0;
		for (unsigned short i = 0; walked < shown; ++i)
		{
			if (!probe_scope_->m_DeclaredClasses.m_Map.IsValidIndex(i))
			{
				break;
			}
			const unsigned class_id = probe_scope_->m_DeclaredClasses.m_Map.Key(i);
			CSchemaType_DeclaredClass* type =
				probe_scope_->m_DeclaredClasses.m_Map.Element(i);
			CSchemaClassInfo* info = (type != nullptr) ? type->m_pClassInfo : nullptr;

			context.Note("    id=%u type=%p %.60s", class_id, static_cast<void*>(type),
				info != nullptr ? SafeName(info->m_pszName) : "(no class info)");

			// The key is a class ID, so the by-ID binding lookup must resolve
			// it to the same class the interned type points at. That closes the
			// last untested lookup on the scope interface.
			if (info != nullptr)
			{
				CSchemaClassInfo* by_id = probe_scope_->FindRawClassBinding(class_id);
				if (by_id != info)
				{
					context.Fail("FindRawClassBinding(%u) gave %p, the map says %p",
						class_id, static_cast<void*>(by_id), static_cast<void*>(info));
				}
			}
			++walked;
		}

		context.Detail("-> %d of %d entr%s walked", walked, count,
			walked == 1 ? "y" : "ies");
		if (walked == 0)
		{
			context.Fail("the map reports %d entries but has no valid index", count);
		}
	});

	CS2ST_CALL(context, "CSchemaSystemTypeScope::m_DeclaredEnums (walk)", {
		const int count = probe_scope_->m_DeclaredEnums.m_Map.Count();
		if (count <= 0 || count > kMaxPlausibleCount)
		{
			context.Detail("-> skipped, the count above is not usable");
			return;
		}

		const int shown = (count < kMaxEntriesPrinted) ? count : kMaxEntriesPrinted;
		int walked = 0;
		for (unsigned short i = 0; walked < shown; ++i)
		{
			if (!probe_scope_->m_DeclaredEnums.m_Map.IsValidIndex(i))
			{
				break;
			}
			const unsigned enum_id = probe_scope_->m_DeclaredEnums.m_Map.Key(i);
			CSchemaType_DeclaredEnum* type = probe_scope_->m_DeclaredEnums.m_Map.Element(i);
			CSchemaEnumInfo* info = (type != nullptr) ? type->m_pEnumInfo : nullptr;

			context.Note("    id=%u type=%p %.60s", enum_id, static_cast<void*>(type),
				info != nullptr ? SafeName(info->m_pszName) : "(no enum info)");

			if (info != nullptr)
			{
				CSchemaEnumInfo* by_id = probe_scope_->FindRawEnumBinding(enum_id);
				if (by_id != info)
				{
					context.Fail("FindRawEnumBinding(%u) gave %p, the map says %p",
						enum_id, static_cast<void*>(by_id), static_cast<void*>(info));
				}
			}
			++walked;
		}

		context.Detail("-> %d of %d entr%s walked", walked, count,
			walked == 1 ? "y" : "ies");
	});
}

// -----------------------------------------------------------------------------
//  SchemaClassInfoData_t
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectClassInfoLayout(TestContext& context)
{
	context.Note("[SchemaClassInfoData_t] struct layout");

	if (probe_class_ == nullptr)
	{
		context.SkipCall("SchemaClassInfoData_t (layout)", "no probe class resolved");
		return;
	}

	context.Note("  instance: %s at %p", probe_class_name_, static_cast<void*>(probe_class_));

	// Oracle checks first: fields whose correct value is known from outside the
	// struct, because we know what was asked for and where.
	CS2ST_CALL(context, "SchemaClassInfoData_t::m_pszName vs the name looked up", {
		const char* name = probe_class_->m_pszName;
		context.Detail("-> \"%.100s\" (asked for \"%s\")", SafeName(name), probe_class_name_);
		if (name == nullptr)
		{
			context.Fail("a registered class binding always carries its name");
		}
		else if (std::strcmp(name, probe_class_name_) != 0)
		{
			context.Fail("this binding is not the class that was asked for");
		}
	});

	CS2ST_CALL(context, "SchemaClassInfoData_t::m_pTypeScope vs the scope searched", {
		context.Detail("-> %p (searched %p, the %s scope)",
			static_cast<void*>(probe_class_->m_pTypeScope),
			static_cast<void*>(probe_scope_), probe_scope_name_);
		if (probe_class_->m_pTypeScope != probe_scope_)
		{
			// Not necessarily wrong: a class promoted to the global scope is
			// findable through a module scope while belonging to the global
			// one. Reported as SUSPECT so a real mismatch is still visible
			// without crying wolf about promotion.
			context.Suspect("the binding points at a different scope than the one "
			                "it was found in; global promotion does this legitimately");
		}
	});

	CS2ST_CALL(context, "SchemaClassInfoData_t::m_pSchemaBinding vs itself", {
		context.Detail("-> %p (this is %p)",
			static_cast<void*>(probe_class_->m_pSchemaBinding),
			static_cast<void*>(probe_class_));
		if (probe_class_->m_pSchemaBinding != nullptr
			&& probe_class_->m_pSchemaBinding != probe_class_)
		{
			context.Suspect("m_pSchemaBinding points somewhere other than this binding");
		}
	});

	CS2ST_CALL(context, "SchemaClassInfoData_t (size / alignment / counts)", {
		const int size = probe_class_->m_nSize;
		const unsigned alignment = probe_class_->m_nAlignment;
		context.Detail("size=%d alignment=%u fields=%u bases=%u meta=%u depth=%u/%u",
			size, alignment,
			static_cast<unsigned>(probe_class_->m_nFieldCount),
			static_cast<unsigned>(probe_class_->m_nBaseClassCount),
			static_cast<unsigned>(probe_class_->m_nStaticMetadataCount),
			static_cast<unsigned>(probe_class_->m_nSingleInheritanceDepth),
			static_cast<unsigned>(probe_class_->m_nMultipleInheritanceDepth));

		if (size <= 0)
		{
			context.Fail("a class occupies at least one byte");
		}
		if (!IsPlausibleAlignment(alignment))
		{
			// An alignment is a power of two by definition, which makes this a
			// type-level check rather than a game-knowledge one.
			context.Fail("%u is not a power-of-two alignment", alignment);
		}
		// A class cannot be smaller than its own alignment demands.
		if (size > 0 && IsPlausibleAlignment(alignment)
			&& (static_cast<unsigned>(size) % alignment) != 0)
		{
			context.Fail("a %d-byte class cannot have %u-byte alignment", size, alignment);
		}
		// Single inheritance is a chain, multiple inheritance a DAG, so the
		// multiple depth can never be the shallower of the two.
		if (probe_class_->m_nMultipleInheritanceDepth
			< probe_class_->m_nSingleInheritanceDepth)
		{
			context.Fail("multiple-inheritance depth %u is below single-inheritance depth %u",
				static_cast<unsigned>(probe_class_->m_nMultipleInheritanceDepth),
				static_cast<unsigned>(probe_class_->m_nSingleInheritanceDepth));
		}
		// A count and the array it counts have to agree about emptiness. This
		// is the check that catches a field inserted between them.
		if ((probe_class_->m_nFieldCount > 0) != (probe_class_->m_pFields != nullptr))
		{
			context.Fail("m_nFieldCount is %u but m_pFields is %p",
				static_cast<unsigned>(probe_class_->m_nFieldCount),
				static_cast<void*>(probe_class_->m_pFields));
		}
		if ((probe_class_->m_nBaseClassCount > 0) != (probe_class_->m_pBaseClasses != nullptr))
		{
			context.Fail("m_nBaseClassCount is %u but m_pBaseClasses is %p",
				static_cast<unsigned>(probe_class_->m_nBaseClassCount),
				static_cast<void*>(probe_class_->m_pBaseClasses));
		}
		if ((probe_class_->m_nStaticMetadataCount > 0)
			!= (probe_class_->m_pStaticMetadata != nullptr))
		{
			context.Fail("m_nStaticMetadataCount is %u but m_pStaticMetadata is %p",
				static_cast<unsigned>(probe_class_->m_nStaticMetadataCount),
				static_cast<void*>(probe_class_->m_pStaticMetadata));
		}
	});

	CS2ST_CALL(context, "SchemaClassInfoData_t::m_nFlags1 (declared bits only)", {
		// std::uint32_t rather than the SDK's uint32: this is our own local, so
		// it should not depend on which SDK header happened to be included.
		// SDK integer typedefs are used below only where an API signature
		// demands the exact type (GetSizeAndAlignment's uint8& out parameter).
		const std::uint32_t flags = probe_class_->m_nFlags1;
		// Every bit SchemaClassFlags1_t declares, so anything left over is a
		// bit Valve added - which is a finding, not a failure.
		const std::uint32_t known =
			SCHEMA_CF1_HAS_VIRTUAL_MEMBERS | SCHEMA_CF1_IS_ABSTRACT
			| SCHEMA_CF1_HAS_TRIVIAL_CONSTRUCTOR | SCHEMA_CF1_HAS_TRIVIAL_DESTRUCTOR
			| SCHEMA_CF1_LIMITED_METADATA | SCHEMA_CF1_INHERITANCE_DEPTH_CALCULATED
			| SCHEMA_CF1_MODULE_LOCAL_TYPE_SCOPE | SCHEMA_CF1_GLOBAL_TYPE_SCOPE
			| SCHEMA_CF1_CONSTRUCT_ALLOWED | SCHEMA_CF1_CONSTRUCT_DISALLOWED
			| SCHEMA_CF1_INFO_TAG_MNetworkAssumeNotNetworkable
			| SCHEMA_CF1_INFO_TAG_MNetworkNoBase
			| SCHEMA_CF1_INFO_TAG_MIgnoreTypeScopeMetaChecks
			| SCHEMA_CF1_INFO_TAG_MDisableDataDescValidation
			| SCHEMA_CF1_INFO_TAG_MClassHasEntityLimitedDataDesc
			| SCHEMA_CF1_INFO_TAG_MClassHasCustomAlignedNewDelete
			| SCHEMA_CF1_INFO_TAG_MNonConstructibleClassBase
			| SCHEMA_CF1_INFO_TAG_MConstructibleClassBase
			| SCHEMA_CF1_INFO_TAG_MHasKV3TransferPolymorphicClassname;

		context.Detail("-> 0x%08x (unknown bits 0x%08x)",
			static_cast<unsigned>(flags), static_cast<unsigned>(flags & ~known));

		// The scope bits are mutually exclusive and one of them says where the
		// class lives, which is checkable against the scope we searched.
		const bool says_global = (flags & SCHEMA_CF1_GLOBAL_TYPE_SCOPE) != 0;
		const bool says_local = (flags & SCHEMA_CF1_MODULE_LOCAL_TYPE_SCOPE) != 0;
		if (says_global && says_local)
		{
			context.Fail("the class claims both global and module-local type scope");
		}
		if ((flags & ~known) != 0)
		{
			context.Suspect("bits outside SchemaClassFlags1_t are set; "
			                "Valve added flags the SDK does not name");
		}
	});

	StructInspector inspect(context, "SchemaClassInfoData_t",
		probe_class_, sizeof(SchemaClassInfoData_t));

	inspect.Pointer("m_pSchemaBinding",   offsetof(SchemaClassInfoData_t, m_pSchemaBinding));
	inspect.String ("m_pszName",          offsetof(SchemaClassInfoData_t, m_pszName));
	inspect.String ("m_pszProjectName",   offsetof(SchemaClassInfoData_t, m_pszProjectName));
	inspect.String ("m_pszCPPName",       offsetof(SchemaClassInfoData_t, m_pszCPPName));
	inspect.Int32  ("m_nSize",            offsetof(SchemaClassInfoData_t, m_nSize), 1, INT32_MAX);
	inspect.Pointer("m_pFields",          offsetof(SchemaClassInfoData_t, m_pFields));
	inspect.Pointer("m_pBaseClasses",     offsetof(SchemaClassInfoData_t, m_pBaseClasses));
	inspect.Pointer("m_pDataDescMap",     offsetof(SchemaClassInfoData_t, m_pDataDescMap));
	inspect.Pointer("m_pStaticMetadata",  offsetof(SchemaClassInfoData_t, m_pStaticMetadata));
	inspect.Pointer("m_pTypeScope",       offsetof(SchemaClassInfoData_t, m_pTypeScope));
	inspect.Pointer("m_pDeclaredClass",   offsetof(SchemaClassInfoData_t, m_pDeclaredClass));
	inspect.UInt32 ("m_nFlags1",          offsetof(SchemaClassInfoData_t, m_nFlags1));
	inspect.UInt32 ("m_nFlags2",          offsetof(SchemaClassInfoData_t, m_nFlags2));
	inspect.Pointer("m_pfnManipulator",   offsetof(SchemaClassInfoData_t, m_pfnManipulator));
	inspect.Summarize();
	inspect.HexDump();

	context.SkipCall("SchemaClassInfoData_t::m_pfnManipulator (call)",
		"every action the manipulator takes - register, allocate, deallocate, "
		"construct or destruct in place - mutates something; the pointer is "
		"checked, never invoked");
	context.SkipCall("SchemaClassInfoData_t::m_pDataDescMap (walk)",
		"datamap_t is now fully defined on this branch, but walking it safely "
		"means trusting m_pDataDescForCompare and its dataDesc count from a "
		"third-party binary; the pointer itself is checked above");
	context.SkipCall("SchemaStaticFieldData_t",
		"SchemaClassInfoData_t exposes no static-field array to reach one "
		"through; the struct is declared but unreachable from here");
}

// -----------------------------------------------------------------------------
//  SchemaClassFieldData_t
//
//  The structure a plugin actually reads: a field's name and the byte offset
//  its value lives at. Getting m_nSingleInheritanceOffset wrong does not crash
//  anything - it silently reads the neighbouring property instead, which is the
//  worst failure mode in this whole document.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectClassFields(TestContext& context)
{
	context.Note("[SchemaClassFieldData_t] fields of %s",
		probe_class_name_ != nullptr ? probe_class_name_ : "(none)");

	if (probe_class_ == nullptr)
	{
		context.SkipCall("SchemaClassFieldData_t (layout)", "no probe class resolved");
		return;
	}

	// Everything below indexes m_pFields with m_nFieldCount, both read out of a
	// struct whose layout is what is under test. They are captured and vetted
	// once here rather than re-read inside each guarded call.
	CS2ST_CALL(context, "SchemaClassInfoData_t::m_pFields (bounds)", {
		probe_fields_ = probe_class_->m_pFields;
		probe_field_count_ = static_cast<int>(probe_class_->m_nFieldCount);
		context.Detail("-> %p, %d field(s)",
			static_cast<void*>(probe_fields_), probe_field_count_);
		if (probe_fields_ == nullptr || probe_field_count_ <= 0)
		{
			probe_fields_ = nullptr;
			return;
		}
		if (probe_field_count_ > kMaxEntriesScanned)
		{
			context.Detail("-> %p, %d field(s), scanning the first %d",
				static_cast<void*>(probe_fields_), probe_field_count_, kMaxEntriesScanned);
			probe_field_count_ = kMaxEntriesScanned;
		}
	});

	if (probe_fields_ == nullptr)
	{
		context.SkipCall("SchemaClassFieldData_t (layout)",
			"%s exposes no fields", probe_class_name_);
		return;
	}

	// Each field individually, so a fault names the index it happened at rather
	// than losing the whole walk.
	const int shown = (probe_field_count_ < kMaxEntriesPrinted)
		? probe_field_count_ : kMaxEntriesPrinted;
	for (int i = 0; i < shown; ++i)
	{
		char target[96];
		std::snprintf(target, sizeof(target), "SchemaClassFieldData_t[%d]", i);

		CS2ST_CALL(context, target, {
			const SchemaClassFieldData_t& field = probe_fields_[i];
			CSchemaType* type = field.m_pType;

			context.Detail("%.60s @ +%d, type %p (%s, \"%.40s\") meta=%d",
				SafeName(field.m_pszName),
				field.m_nSingleInheritanceOffset,
				static_cast<void*>(type),
				type != nullptr ? TypeCategoryName(type->m_eTypeCategory) : "no type",
				type != nullptr ? SafeName(type->m_sTypeName.Get()) : "",
				field.m_nStaticMetadataCount);

			if (field.m_pszName == nullptr)
			{
				context.Fail("a schema field always has a name");
			}
			if (type == nullptr)
			{
				context.Fail("a schema field always has a type");
			}
			if (field.m_nStaticMetadataCount < 0)
			{
				context.Fail("a metadata count cannot be negative");
			}
			if ((field.m_nStaticMetadataCount > 0) != (field.m_pStaticMetadata != nullptr))
			{
				context.Fail("metadata count %d but array %p",
					field.m_nStaticMetadataCount,
					static_cast<void*>(field.m_pStaticMetadata));
			}

			// The offset has to land inside the class it belongs to. This is
			// the check that matters most in the suite: an offset outside the
			// class is one a plugin would read straight through into whatever
			// follows the object.
			if (field.m_nSingleInheritanceOffset < 0
				|| field.m_nSingleInheritanceOffset >= probe_class_->m_nSize)
			{
				context.Fail("offset %d is outside a %d-byte class",
					field.m_nSingleInheritanceOffset, probe_class_->m_nSize);
			}

			// And the field plus its own width has to fit. This is a strictly
			// stronger statement than "the offset is inside the class", and it
			// is only askable because GetSizeAndAlignment gives the width -
			// which is the same oracle the CSchemaType sweep uses, applied here
			// to catch a size that disagrees with a layout.
			if (type != nullptr)
			{
				// uint8 for the same reason as in ProbeSchemaType: the out
				// parameter is a uint8& and a reference needs the exact type.
				int type_size = 0;
				uint8 type_alignment = 0;
				if (type->GetSizeAndAlignment(type_size, type_alignment)
					&& type_size > 0
					&& field.m_nSingleInheritanceOffset >= 0
					&& field.m_nSingleInheritanceOffset + type_size > probe_class_->m_nSize)
				{
					context.Fail("a %d-byte field at +%d does not fit in a %d-byte class",
						type_size, field.m_nSingleInheritanceOffset, probe_class_->m_nSize);
				}
			}
		});
	}

	if (probe_field_count_ > shown)
	{
		context.Note("  ... %d more field(s) not printed", probe_field_count_ - shown);
	}

	// The rest are not printed but their offsets are still checked - the value
	// of this scan is in the outlier, and one bad offset out of two hundred is
	// exactly what an inserted field looks like.
	CS2ST_CALL(context, "SchemaClassFieldData_t (all offsets in range)", {
		const int class_size = probe_class_->m_nSize;
		int out_of_range = 0;
		int first_bad = -1;
		int nameless = 0;
		int typeless = 0;

		for (int i = 0; i < probe_field_count_; ++i)
		{
			const int offset = probe_fields_[i].m_nSingleInheritanceOffset;
			if (offset < 0 || offset >= class_size)
			{
				if (out_of_range == 0)
				{
					first_bad = i;
				}
				++out_of_range;
			}
			if (probe_fields_[i].m_pszName == nullptr)
			{
				++nameless;
			}
			if (probe_fields_[i].m_pType == nullptr)
			{
				++typeless;
			}
		}

		context.Detail("%d field(s) checked against a %d-byte class: "
		               "%d offset(s) outside, %d nameless, %d typeless",
			probe_field_count_, class_size, out_of_range, nameless, typeless);
		if (out_of_range > 0)
		{
			context.Fail("first bad offset at index %d, offset %d", first_bad,
				probe_fields_[first_bad].m_nSingleInheritanceOffset);
		}
		if (nameless > 0 || typeless > 0)
		{
			context.Fail("%d field(s) with no name and %d with no type",
				nameless, typeless);
		}
	});

	// Harvest one live CSchemaType per category for the virtual sweep. Done as
	// its own pass rather than inside the printing loop so the sweep gets the
	// widest possible selection rather than only what the first few fields
	// happened to use.
	CS2ST_CALL(context, "SchemaClassFieldData_t (harvest one type per category)", {
		for (int i = 0; i < probe_field_count_; ++i)
		{
			CSchemaType* type = probe_fields_[i].m_pType;
			if (type == nullptr)
			{
				continue;
			}
			if (probe_type_ == nullptr)
			{
				probe_type_ = type;
			}
			switch (type->m_eTypeCategory)
			{
				case SCHEMA_TYPE_BUILTIN:
					// Only if ExerciseBuiltinTypes did not already supply one;
					// its instance is preferable because everything about it is
					// known in advance.
					if (probe_builtin_type_ == nullptr) { probe_builtin_type_ = type; }
					break;
				case SCHEMA_TYPE_DECLARED_CLASS:
					if (probe_class_type_ == nullptr) { probe_class_type_ = type; }
					break;
				case SCHEMA_TYPE_DECLARED_ENUM:
					if (probe_enum_type_ == nullptr) { probe_enum_type_ = type; }
					break;
				case SCHEMA_TYPE_ATOMIC:
					if (probe_atomic_type_ == nullptr) { probe_atomic_type_ = type; }
					break;
				case SCHEMA_TYPE_POINTER:
					if (probe_pointer_type_ == nullptr) { probe_pointer_type_ = type; }
					break;
				default:
					break;
			}
		}

		context.Detail("builtin=%p class=%p enum=%p atomic=%p pointer=%p",
			static_cast<void*>(probe_builtin_type_),
			static_cast<void*>(probe_class_type_),
			static_cast<void*>(probe_enum_type_),
			static_cast<void*>(probe_atomic_type_),
			static_cast<void*>(probe_pointer_type_));
	});

	StructInspector inspect(context, "SchemaClassFieldData_t[0]",
		&probe_fields_[0], sizeof(SchemaClassFieldData_t));

	inspect.String ("m_pszName",              offsetof(SchemaClassFieldData_t, m_pszName));
	inspect.Pointer("m_pType",                offsetof(SchemaClassFieldData_t, m_pType));
	inspect.Int32  ("m_nSingleInheritanceOffset",
	                                          offsetof(SchemaClassFieldData_t, m_nSingleInheritanceOffset),
	                                          0, INT32_MAX);
	inspect.Int32  ("m_nStaticMetadataCount", offsetof(SchemaClassFieldData_t, m_nStaticMetadataCount),
	                                          0, INT32_MAX);
	inspect.Pointer("m_pStaticMetadata",      offsetof(SchemaClassFieldData_t, m_pStaticMetadata));
	inspect.Summarize();
	inspect.HexDump();
}

// -----------------------------------------------------------------------------
//  SchemaBaseClassInfoData_t
//
//  Two fields, one of which has a known-correct value: under single inheritance
//  the first base class sits at offset 0. Cheap, and it is the only check in
//  this suite that touches the inheritance side of a binding at all.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectBaseClasses(TestContext& context)
{
	context.Note("[SchemaBaseClassInfoData_t] base classes of %s",
		probe_class_name_ != nullptr ? probe_class_name_ : "(none)");

	if (probe_class_ == nullptr)
	{
		context.SkipCall("SchemaBaseClassInfoData_t", "no probe class resolved");
		return;
	}

	SchemaBaseClassInfoData_t* bases = nullptr;
	int base_count = 0;

	CS2ST_CALL(context, "SchemaClassInfoData_t::m_pBaseClasses (bounds)", {
		bases = probe_class_->m_pBaseClasses;
		base_count = static_cast<int>(probe_class_->m_nBaseClassCount);
		context.Detail("-> %p, %d base class(es)", static_cast<void*>(bases), base_count);
		if (bases == nullptr || base_count <= 0)
		{
			bases = nullptr;
			return;
		}
		if (base_count > kMaxEntriesScanned)
		{
			context.Fail("%d base classes is not plausible", base_count);
			bases = nullptr;
		}
	});

	if (bases == nullptr)
	{
		context.SkipCall("SchemaBaseClassInfoData_t (layout)",
			"%s has no base classes registered (a root class, or the count is zero)",
			probe_class_name_);
		return;
	}

	for (int i = 0; i < base_count && i < kMaxEntriesPrinted; ++i)
	{
		char target[96];
		std::snprintf(target, sizeof(target), "SchemaBaseClassInfoData_t[%d]", i);

		CS2ST_CALL(context, target, {
			const SchemaBaseClassInfoData_t& base = bases[i];
			CSchemaClassInfo* info = base.m_pClass;
			context.Detail("+%u -> %p %.70s (%d bytes)", base.m_nOffset,
				static_cast<void*>(info),
				info != nullptr ? SafeName(info->m_pszName) : "(null)",
				info != nullptr ? info->m_nSize : -1);

			if (info == nullptr)
			{
				context.Fail("a base class entry with no class behind it");
				return;
			}

			// The first base under single inheritance is at offset 0. This is
			// the one entry with a value known in advance.
			if (i == 0 && base.m_nOffset != 0)
			{
				context.Suspect("the first base class is at +%u rather than 0; "
				                "expected under single inheritance", base.m_nOffset);
			}
			// A base has to fit inside the derived class, and cannot be larger
			// than it.
			// m_nSize is signed and m_nOffset unsigned; compared as int64 so
			// the promotion cannot turn a negative size into a huge unsigned.
			if (static_cast<std::int64_t>(base.m_nOffset)
				>= static_cast<std::int64_t>(probe_class_->m_nSize))
			{
				context.Fail("base at +%u is outside a %d-byte class",
					base.m_nOffset, probe_class_->m_nSize);
			}
			if (info->m_nSize > probe_class_->m_nSize)
			{
				context.Fail("a %d-byte base does not fit in a %d-byte derived class",
					info->m_nSize, probe_class_->m_nSize);
			}
		});
	}

	if (base_count > kMaxEntriesPrinted)
	{
		context.Note("  ... %d more base class(es) not printed",
			base_count - kMaxEntriesPrinted);
	}

	StructInspector inspect(context, "SchemaBaseClassInfoData_t[0]",
		&bases[0], sizeof(SchemaBaseClassInfoData_t));
	inspect.UInt32 ("m_nOffset", offsetof(SchemaBaseClassInfoData_t, m_nOffset));
	inspect.Pointer("m_pClass",  offsetof(SchemaBaseClassInfoData_t, m_pClass));
	inspect.Summarize();
}

// -----------------------------------------------------------------------------
//  SchemaMetadataEntryData_t
//
//  The MNetwork* annotations plugins read to decide what is networked. Two
//  fields, and the names are the interesting part: they are game content, so
//  the list changes when the game changes while nothing about the interface
//  moves.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectClassMetadata(TestContext& context)
{
	context.Note("[SchemaMetadataEntryData_t] static metadata on %s",
		probe_class_name_ != nullptr ? probe_class_name_ : "(none)");

	if (probe_class_ == nullptr)
	{
		context.SkipCall("SchemaMetadataEntryData_t", "no probe class resolved");
		return;
	}

	SchemaMetadataEntryData_t* metadata = nullptr;
	int metadata_count = 0;

	CS2ST_CALL(context, "SchemaClassInfoData_t::m_pStaticMetadata (bounds)", {
		metadata = probe_class_->m_pStaticMetadata;
		metadata_count = static_cast<int>(probe_class_->m_nStaticMetadataCount);
		context.Detail("-> %p, %d entr%s", static_cast<void*>(metadata),
			metadata_count, metadata_count == 1 ? "y" : "ies");
		if (metadata == nullptr || metadata_count <= 0)
		{
			metadata = nullptr;
			return;
		}
		if (metadata_count > kMaxEntriesScanned)
		{
			context.Fail("%d metadata entries is not plausible", metadata_count);
			metadata = nullptr;
		}
	});

	if (metadata == nullptr)
	{
		context.SkipCall("SchemaMetadataEntryData_t (layout)",
			"%s carries no static metadata", probe_class_name_);
		return;
	}

	for (int i = 0; i < metadata_count && i < kMaxEntriesPrinted; ++i)
	{
		char target[96];
		std::snprintf(target, sizeof(target), "SchemaMetadataEntryData_t[%d]", i);

		CS2ST_CALL(context, target, {
			const SchemaMetadataEntryData_t& entry = metadata[i];
			context.Detail("\"%.70s\" -> %p", SafeName(entry.m_pszName), entry.m_pData);
			if (entry.m_pszName == nullptr)
			{
				context.Fail("a metadata entry always has a name");
			}
			// m_pData is deliberately not dereferenced: what it points at
			// depends on the tag, and there is no discriminator to read it by.
		});
	}

	if (metadata_count > kMaxEntriesPrinted)
	{
		context.Note("  ... %d more entr%s not printed", metadata_count - kMaxEntriesPrinted,
			metadata_count - kMaxEntriesPrinted == 1 ? "y" : "ies");
	}

	// The same structure hangs off individual fields, and that is where the
	// MNetwork tags a plugin actually cares about live. Walked separately
	// because its being non-empty is what proves the field-level pointer is
	// real rather than just plausible.
	if (probe_fields_ != nullptr)
	{
		CS2ST_CALL(context, "SchemaMetadataEntryData_t (field-level, first found)", {
			for (int i = 0; i < probe_field_count_; ++i)
			{
				const SchemaClassFieldData_t& field = probe_fields_[i];
				if (field.m_pStaticMetadata == nullptr || field.m_nStaticMetadataCount <= 0)
				{
					continue;
				}

				const int count = (field.m_nStaticMetadataCount < kMaxEntriesPrinted)
					? field.m_nStaticMetadataCount : kMaxEntriesPrinted;
				context.Detail("%.60s carries %d entr%s", SafeName(field.m_pszName),
					field.m_nStaticMetadataCount,
					field.m_nStaticMetadataCount == 1 ? "y" : "ies");
				for (int j = 0; j < count; ++j)
				{
					context.Note("    [%d] \"%.70s\" -> %p", j,
						SafeName(field.m_pStaticMetadata[j].m_pszName),
						field.m_pStaticMetadata[j].m_pData);
					if (field.m_pStaticMetadata[j].m_pszName == nullptr)
					{
						context.Fail("a metadata entry always has a name");
					}
				}
				return;
			}
			context.Detail("-> no field on %s carries static metadata", probe_class_name_);
		});
	}

	StructInspector inspect(context, "SchemaMetadataEntryData_t[0]",
		&metadata[0], sizeof(SchemaMetadataEntryData_t));
	inspect.String ("m_pszName", offsetof(SchemaMetadataEntryData_t, m_pszName));
	inspect.Pointer("m_pData",   offsetof(SchemaMetadataEntryData_t, m_pData));
	inspect.Summarize();
}

// -----------------------------------------------------------------------------
//  SchemaEnumInfoData_t
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectEnumInfoLayout(TestContext& context)
{
	context.Note("[SchemaEnumInfoData_t] struct layout");

	if (probe_enum_ == nullptr)
	{
		context.SkipCall("SchemaEnumInfoData_t (layout)",
			"no enum binding resolved, by name or from a class field");
		return;
	}

	CS2ST_CALL(context, "SchemaEnumInfoData_t (name / size / range)", {
		const unsigned size = probe_enum_->m_nSize;
		const unsigned alignment = probe_enum_->m_nAlignment;
		context.Detail("\"%.60s\" size=%u alignment=%u enumerators=%u meta=%u "
		               "range=[%lld, %lld] flags=0x%02x",
			SafeName(probe_enum_->m_pszName), size, alignment,
			static_cast<unsigned>(probe_enum_->m_nEnumeratorCount),
			static_cast<unsigned>(probe_enum_->m_nStaticMetadataCount),
			static_cast<long long>(probe_enum_->m_nMinEnumeratorValue),
			static_cast<long long>(probe_enum_->m_nMaxEnumeratorValue),
			static_cast<unsigned>(probe_enum_->m_nFlags));

		if (probe_enum_->m_pszName == nullptr)
		{
			context.Fail("a registered enum always carries its name");
		}
		// An enum's underlying type is one of the four integer widths; nothing
		// else is representable. That is the C++ type talking, not the game.
		if (size != 1 && size != 2 && size != 4 && size != 8)
		{
			context.Fail("%u is not an integer width", size);
		}
		if (!IsPlausibleAlignment(alignment))
		{
			context.Fail("%u is not a power-of-two alignment", alignment);
		}
		// An enum is naturally aligned to its own width.
		if (alignment != size && size >= 1 && size <= 8)
		{
			context.Suspect("a %u-byte enum with %u-byte alignment", size, alignment);
		}
		if (probe_enum_->m_nMinEnumeratorValue > probe_enum_->m_nMaxEnumeratorValue)
		{
			context.Fail("the minimum enumerator exceeds the maximum");
		}
		if ((probe_enum_->m_nEnumeratorCount > 0) != (probe_enum_->m_pEnumerators != nullptr))
		{
			context.Fail("m_nEnumeratorCount is %u but m_pEnumerators is %p",
				static_cast<unsigned>(probe_enum_->m_nEnumeratorCount),
				static_cast<void*>(probe_enum_->m_pEnumerators));
		}
		if ((probe_enum_->m_nStaticMetadataCount > 0)
			!= (probe_enum_->m_pStaticMetadata != nullptr))
		{
			context.Fail("m_nStaticMetadataCount is %u but m_pStaticMetadata is %p",
				static_cast<unsigned>(probe_enum_->m_nStaticMetadataCount),
				static_cast<void*>(probe_enum_->m_pStaticMetadata));
		}

		// SCHEMA_EF_IS_REGISTERED must be set on a binding we just looked up
		// successfully, and the two scope bits are mutually exclusive.
		const std::uint8_t flags = probe_enum_->m_nFlags;
		if ((flags & SCHEMA_EF_IS_REGISTERED) == 0)
		{
			context.Fail("the lookup succeeded but the enum is not marked registered");
		}
		if ((flags & SCHEMA_EF_GLOBAL_TYPE_SCOPE) != 0
			&& (flags & SCHEMA_EF_MODULE_LOCAL_TYPE_SCOPE) != 0)
		{
			context.Fail("the enum claims both global and module-local type scope");
		}
		// Cast is explicit because | promotes the enumerators to int; the
		// value is 0x3f, but relying on an implicit narrowing here would be one
		// more thing for a future -Wconversion build to trip over.
		const std::uint8_t known_flags = static_cast<std::uint8_t>(
			SCHEMA_EF_IS_REGISTERED
			| SCHEMA_EF_MODULE_LOCAL_TYPE_SCOPE
			| SCHEMA_EF_GLOBAL_TYPE_SCOPE
			| SCHEMA_EF_UNK003
			| SCHEMA_EF_UNK004
			| SCHEMA_EF_UNK005);
		// Masked back down to 8 bits: ~ promotes to int, so without the cast
		// the complement would carry 24 set bits that are not part of the field.
		const std::uint8_t unknown_flags =
			static_cast<std::uint8_t>(flags & static_cast<std::uint8_t>(~known_flags));
		if (unknown_flags != 0)
		{
			context.Suspect("bits outside SchemaEnumFlags_t are set (0x%02x)",
				static_cast<unsigned>(unknown_flags));
		}
	});

	CS2ST_CALL(context, "SchemaEnumInfoData_t::m_pSchemaBinding vs itself", {
		context.Detail("-> %p (this is %p)",
			static_cast<void*>(probe_enum_->m_pSchemaBinding),
			static_cast<void*>(probe_enum_));
		if (probe_enum_->m_pSchemaBinding != nullptr
			&& probe_enum_->m_pSchemaBinding != probe_enum_)
		{
			context.Suspect("m_pSchemaBinding points somewhere other than this binding");
		}
	});

	StructInspector inspect(context, "SchemaEnumInfoData_t",
		probe_enum_, sizeof(SchemaEnumInfoData_t));

	inspect.Pointer("m_pSchemaBinding",    offsetof(SchemaEnumInfoData_t, m_pSchemaBinding));
	inspect.String ("m_pszName",           offsetof(SchemaEnumInfoData_t, m_pszName));
	inspect.String ("m_pszProjectName",    offsetof(SchemaEnumInfoData_t, m_pszProjectName));
	inspect.Pointer("m_pEnumerators",      offsetof(SchemaEnumInfoData_t, m_pEnumerators));
	inspect.Pointer("m_pStaticMetadata",   offsetof(SchemaEnumInfoData_t, m_pStaticMetadata));
	inspect.Pointer("m_pTypeScope",        offsetof(SchemaEnumInfoData_t, m_pTypeScope));
	inspect.Int64  ("m_nMinEnumeratorValue",
	                                       offsetof(SchemaEnumInfoData_t, m_nMinEnumeratorValue));
	inspect.Int64  ("m_nMaxEnumeratorValue",
	                                       offsetof(SchemaEnumInfoData_t, m_nMaxEnumeratorValue));
	inspect.Summarize();
	inspect.HexDump();
}

// -----------------------------------------------------------------------------
//  SchemaEnumeratorInfoData_t
//
//  The enumerator list, and with it the best-shaped oracle in the suite:
//  m_nMinEnumeratorValue and m_nMaxEnumeratorValue are cached summaries of the
//  array that follows them, so every entry must fall inside that range and the
//  extremes must actually be attained. Two independent descriptions of the same
//  data, one of which is derivable from the other - which is what makes a
//  disagreement proof rather than suspicion.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::InspectEnumerators(TestContext& context)
{
	context.Note("[SchemaEnumeratorInfoData_t] enumerators of %s",
		probe_enum_name_ != nullptr ? probe_enum_name_ : "(none)");

	if (probe_enum_ == nullptr)
	{
		context.SkipCall("SchemaEnumeratorInfoData_t", "no enum binding resolved");
		return;
	}

	SchemaEnumeratorInfoData_t* enumerators = nullptr;
	int enumerator_count = 0;

	CS2ST_CALL(context, "SchemaEnumInfoData_t::m_pEnumerators (bounds)", {
		enumerators = probe_enum_->m_pEnumerators;
		enumerator_count = static_cast<int>(probe_enum_->m_nEnumeratorCount);
		context.Detail("-> %p, %d enumerator(s)",
			static_cast<void*>(enumerators), enumerator_count);
		if (enumerators == nullptr || enumerator_count <= 0)
		{
			enumerators = nullptr;
			return;
		}
		if (enumerator_count > kMaxEntriesScanned)
		{
			context.Detail("-> %p, %d enumerator(s), scanning the first %d",
				static_cast<void*>(enumerators), enumerator_count, kMaxEntriesScanned);
			enumerator_count = kMaxEntriesScanned;
		}
	});

	if (enumerators == nullptr)
	{
		context.SkipCall("SchemaEnumeratorInfoData_t (layout)",
			"%.60s exposes no enumerators", SafeName(probe_enum_name_));
		return;
	}

	for (int i = 0; i < enumerator_count && i < kMaxEntriesPrinted; ++i)
	{
		char target[96];
		std::snprintf(target, sizeof(target), "SchemaEnumeratorInfoData_t[%d]", i);

		CS2ST_CALL(context, target, {
			const SchemaEnumeratorInfoData_t& entry = enumerators[i];
			context.Detail("%.60s = %lld, meta=%d", SafeName(entry.m_pszName),
				static_cast<long long>(entry.m_nValue), entry.m_nStaticMetadataCount);

			if (entry.m_pszName == nullptr)
			{
				context.Fail("an enumerator always has a name");
			}
			if (entry.m_nStaticMetadataCount < 0)
			{
				context.Fail("a metadata count cannot be negative");
			}
			if ((entry.m_nStaticMetadataCount > 0) != (entry.m_pStaticMetadata != nullptr))
			{
				context.Fail("metadata count %d but array %p",
					entry.m_nStaticMetadataCount,
					static_cast<void*>(entry.m_pStaticMetadata));
			}
		});
	}

	if (enumerator_count > kMaxEntriesPrinted)
	{
		context.Note("  ... %d more enumerator(s) not printed",
			enumerator_count - kMaxEntriesPrinted);
	}

	CS2ST_CALL(context, "SchemaEnumeratorInfoData_t vs m_nMin/MaxEnumeratorValue", {
		const std::int64_t declared_min = probe_enum_->m_nMinEnumeratorValue;
		const std::int64_t declared_max = probe_enum_->m_nMaxEnumeratorValue;

		std::int64_t observed_min = enumerators[0].m_nValue;
		std::int64_t observed_max = enumerators[0].m_nValue;
		int out_of_range = 0;
		int first_bad = -1;

		for (int i = 0; i < enumerator_count; ++i)
		{
			const std::int64_t value = enumerators[i].m_nValue;
			if (value < observed_min) { observed_min = value; }
			if (value > observed_max) { observed_max = value; }
			if (value < declared_min || value > declared_max)
			{
				if (out_of_range == 0) { first_bad = i; }
				++out_of_range;
			}
		}

		context.Detail("declared [%lld, %lld] vs observed [%lld, %lld] over %d entr%s",
			static_cast<long long>(declared_min), static_cast<long long>(declared_max),
			static_cast<long long>(observed_min), static_cast<long long>(observed_max),
			enumerator_count, enumerator_count == 1 ? "y" : "ies");

		if (out_of_range > 0)
		{
			context.Fail("%d enumerator(s) outside the declared range, first at "
			             "index %d with value %lld",
				out_of_range, first_bad,
				static_cast<long long>(enumerators[first_bad].m_nValue));
		}
		// The declared extremes are summaries, so they must be attained rather
		// than merely bracket the values - unless the walk was truncated, in
		// which case the unscanned tail could hold them.
		else if (enumerator_count == static_cast<int>(probe_enum_->m_nEnumeratorCount))
		{
			if (observed_min != declared_min || observed_max != declared_max)
			{
				context.Fail("the declared range is wider than any enumerator reaches");
			}
		}

		// Every enumerator has to be representable in the enum's own width.
		// A one-byte enum holding 70000 is not a wide enum, it is a bad read.
		const unsigned width = probe_enum_->m_nSize;
		if (width >= 1 && width <= 8)
		{
			const std::int64_t limit = (width == 8)
				? INT64_MAX
				: (static_cast<std::int64_t>(1) << (width * 8 - 1));
			if (width < 8 && (observed_max >= limit || observed_min < -limit))
			{
				context.Fail("values in [%lld, %lld] do not fit a %u-byte enum",
					static_cast<long long>(observed_min),
					static_cast<long long>(observed_max), width);
			}
		}
	});

	StructInspector inspect(context, "SchemaEnumeratorInfoData_t[0]",
		&enumerators[0], sizeof(SchemaEnumeratorInfoData_t));
	inspect.String ("m_pszName",              offsetof(SchemaEnumeratorInfoData_t, m_pszName));
	inspect.Int64  ("m_nValue",               offsetof(SchemaEnumeratorInfoData_t, m_nValue));
	inspect.Int32  ("m_nStaticMetadataCount", offsetof(SchemaEnumeratorInfoData_t, m_nStaticMetadataCount),
	                                          0, INT32_MAX);
	inspect.Pointer("m_pStaticMetadata",      offsetof(SchemaEnumeratorInfoData_t, m_pStaticMetadata));
	inspect.Summarize();
	inspect.HexDump();
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
//
//  This interface is unusually dangerous to probe blind: much of it registers,
//  installs or mutates schema state that the entire game reads from. A single
//  stray Type_Atomic() call adds a type to a live scope.
//
//  Listed one method per line rather than as families. A reader has to be able
//  to tell "checked, fine" from "nobody looked", and a line that says "the
//  Type_ family, 14 methods" tells them neither which fourteen nor whether the
//  reason applies equally to all of them.
// -----------------------------------------------------------------------------

void SchemaSystemSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ISchemaSystem] not called - mutates schema state");

	context.SkipCall("ISchemaSystem::LoadSchemaDataForModules",
		"loads schema data for a module list; a write, and a slow one");
	context.SkipCall("ISchemaSystem::CompleteModuleRegistration",
		"runs the registration callbacks queued for a module");
	context.SkipCall("ISchemaSystem::RegisterAtomicType", "adds a type to the system");
	context.SkipCall("ISchemaSystem::RegisterAtomics", "adds types to a module scope");
	context.SkipCall("ISchemaSystem::RegisterEnums", "adds enums to a module scope");
	context.SkipCall("ISchemaSystem::RegisterClasses", "adds classes to a module scope");
	context.SkipCall("ISchemaSystem::ValidateClasses",
		"walks a class array and may write back to the bindings in it");
	context.SkipCall("ISchemaSystem::VerifySchemaBindingConsistency",
		"a consistency sweep over every binding on the server; the engine's own "
		"result is already visible in m_DetectedSchemaMismatches, which this "
		"suite reads instead");

	context.Note("[ISchemaSystem] not called - console output or undocumented arguments");

	context.SkipCall("ISchemaSystem::PrintSchemaStats",
		"spews to the console with nothing returned to read back");
	context.SkipCall("ISchemaSystem::PrintSchemaMetaStats", "same");
	context.SkipCall("ISchemaSystem::ConvertOldIntrospectedResourceDataToKV3",
		"three undocumented void* arguments and a CKV3Arena; no way to "
		"construct a meaningful call, and a wrong one writes into the arena");
	context.SkipCall("ISchemaSystem::~ISchemaSystem",
		"declared last in the SDK's interface; see the header on why that "
		"matters and why nothing here calls it");

	context.Note("[CSchemaSystemTypeScope] not called - installs a binding");

	context.SkipCall("CSchemaSystemTypeScope::InstallSchemaClassBinding",
		"adds a class binding to a live scope");
	context.SkipCall("CSchemaSystemTypeScope::InstallSchemaEnumBinding",
		"adds an enum binding to a live scope");
	context.SkipCall("CSchemaSystemTypeScope::MarkClassAsRequiringGlobalPromotion",
		"queues a class for promotion into the global scope");
	context.SkipCall("CSchemaSystemTypeScope::MarkEnumAsRequiringGlobalPromotion",
		"queues an enum for promotion into the global scope");

	context.Note("[CSchemaSystemTypeScope] not called - Type_* interns on a miss");

	// Type_Builtin is the exception and is exercised for all fourteen builtins:
	// it indexes the scope's pre-initialised array instead of interning. Every
	// other member of the family creates and inserts, so each is listed with
	// what it would leave behind.
	context.SkipCall("CSchemaSystemTypeScope::Type_Ptr",
		"interns a pointer-to-T type; Type_Builtin is the only member of this "
		"family that is a pure lookup, and it is called in full above");
	context.SkipCall("CSchemaSystemTypeScope::Type_Atomic", "interns an atomic type");
	context.SkipCall("CSchemaSystemTypeScope::Type_Atomic_T", "interns an atomic-of-T type");
	context.SkipCall("CSchemaSystemTypeScope::Type_Atomic_CollectionOfT",
		"interns a collection-of-T type");
	context.SkipCall("CSchemaSystemTypeScope::Type_Atomic_TT", "interns a two-parameter atomic");
	context.SkipCall("CSchemaSystemTypeScope::Type_Atomic_I", "interns an integer-keyed atomic");
	context.SkipCall("CSchemaSystemTypeScope::Type_DeclaredClass",
		"interns a declared-class type; FindType_DeclaredClass is the "
		"read-only half and is called above");
	context.SkipCall("CSchemaSystemTypeScope::Type_DeclaredEnum",
		"interns a declared-enum type; FindType_DeclaredEnum is the "
		"read-only half and is called above");
	context.SkipCall("CSchemaSystemTypeScope::Type_FixedArray", "interns an array type");
	context.SkipCall("CSchemaSystemTypeScope::Type_FixedArray_Multidimensional",
		"interns an array type, and is variadic with no documented terminator");
	context.SkipCall("CSchemaSystemTypeScope::Type_Bitfield", "interns a bitfield type");

	context.Note("[CSchemaSystemTypeScope] not called - lookup key cannot be supplied");

	context.SkipCall("CSchemaSystemTypeScope::FindType_Atomic_T",
		"keyed by atomic ID plus a template CSchemaType*; the pair can only be "
		"had from an existing _T type, and no field on the probe class has one");
	context.SkipCall("CSchemaSystemTypeScope::FindType_Atomic_CollectionOfT",
		"keyed by atomic ID, template type and a manipulator function pointer");
	context.SkipCall("CSchemaSystemTypeScope::FindType_Atomic_TT",
		"keyed by atomic ID and two template types");
	context.SkipCall("CSchemaSystemTypeScope::FindType_Atomic_I",
		"keyed by atomic ID and an integer whose meaning is type-specific");

	context.Note("[CSchemaSystemTypeScope] not called - writes through an out pointer "
	             "that outlives the call");

	// These take the address of a caller-owned pointer and, on a miss, appear
	// to keep it so a later registration can fill it in. Handing the engine a
	// stack address it might write to after this function returns is a hazard
	// worth more than the coverage.
	context.SkipCall("CSchemaSystemTypeScope::ResolveAtomicInfoThreadsafe",
		"stores the address of a caller-owned pointer for deferred fill-in; "
		"a stack address handed to the engine would dangle on a miss");
	context.SkipCall("CSchemaSystemTypeScope::ResolveEnumInfoThreadsafe", "same");
	context.SkipCall("CSchemaSystemTypeScope::ResolveClassInfoThreadsafe", "same");

	context.Note("[CSchemaType] not called");

	context.SkipCall("CSchemaType::SpewDescription",
		"writes a full type description to a logging channel; the fields it "
		"would print are read directly above");
	context.SkipCall("CSchemaType::~CSchemaType",
		"declared last in the SDK's class; IsValid() at slot 0 is the probe "
		"for whether that assumption holds");
}

} // namespace cs2st
