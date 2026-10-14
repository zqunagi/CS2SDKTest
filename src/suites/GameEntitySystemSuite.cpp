#include "suites/GameEntitySystemSuite.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <appframework/iappsystem.h>
#include <engine2/igameresourceservice.h>
#include <engine2/ivengineserver2.h>
#include <entity2/entityclass.h>
#include <entity2/entityinstance.h>
#include <gameentitysystem.h>
#include <interfaces/interfaces.h>

#include "core/Log.h"
#include "data/GameModules.h"
#include "hook/HookArg.h"
#include "hook/HookRecorder.h"
#include "testkit/StructInspector.h"

/**
 * s2sdk declares `extern CGameEntitySystem* GameEntitySystem();` and defines it
 * in entity2/entitysystem.cpp - which is why that file compiles into this
 * plugin - by walking g_pGameResourceServiceServer's declared members, not a
 * hard-coded byte offset. ResolveEntitySystem cross-checks that accessor
 * against this suite's own independent slot scan.
 */

namespace cs2st
{
namespace
{

// ---- hook ids ---------------------------------------------------------------

constexpr const char* kOnEntityCreated       = "IEntityListener::OnEntityCreated";
constexpr const char* kOnEntitySpawned       = "IEntityListener::OnEntitySpawned";
constexpr const char* kOnEntityDeleted       = "IEntityListener::OnEntityDeleted";
constexpr const char* kOnEntityParentChanged = "IEntityListener::OnEntityParentChanged";

constexpr int kDeclaredHookCount = 4;

// ---- the offset the whole ecosystem depends on ------------------------------

/**
 * Where CGameEntitySystem* sits inside the GameResourceService object.
 *
 * Not from the SDK - the SDK has nothing to say about IGameResourceService
 * beyond its name. These are the values every published CS2 plugin uses, and
 * they are treated here as a hypothesis to be checked rather than a fact.
 */
#if CS2ST_WINDOWS
constexpr std::size_t kAssumedEntitySystemOffset = 0x58;
#else
constexpr std::size_t kAssumedEntitySystemOffset = 0x50;
#endif

/**
 * How far past the start of the service object the fallback sweep looks.
 *
 * A deliberate out-of-bounds read if the object turns out to be smaller than
 * this, which is why each candidate is examined inside its own guarded call:
 * the cost of running off the end is one CRASHED row, not the sweep.
 */
constexpr std::size_t kEntitySystemScanLimit = 0x100;

// ---- walk bounds ------------------------------------------------------------

/**
 * Every list length below is a field at an offset that is itself under test.
 * If the offset is wrong the number is arbitrary, so nothing is walked without
 * a cap - otherwise a misplaced field turns a probe into a hang.
 */
constexpr int kMaxEntitiesScanned = 512;

/** Entities printed individually before the walk goes quiet and just counts. */
constexpr int kMaxEntitiesDetailed = 6;

/**
 * Accessor slots CEntitySystem declares. Computed from the array rather than
 * written down, because MAX_ACCESSORS is private and a literal 32 would stop
 * tracking the declaration the moment it changed.
 */
constexpr int kAccessorSlots =
	static_cast<int>(sizeof(CEntitySystem::m_Accessors) / sizeof(IEntityDataInstantiator*));

/** Every bit EntityFlags_t names. Anything else is unknown, not wrong. */
constexpr std::uint32_t kKnownEntityFlags =
	EF_IS_INVALID_EHANDLE | EF_SPAWN_IN_PROGRESS | EF_IN_STAGING_LIST
	| EF_IN_POST_DATA_UPDATE | EF_DELETE_IN_PROGRESS | EF_IN_STASIS
	| EF_IS_ISOLATED_ALLOCATION_NETWORKABLE | EF_IS_DORMANT | EF_IS_PRE_SPAWN
	| EF_MARKED_FOR_DELETE | EF_IS_CONSTRUCTION_IN_PROGRESS
	| EF_IS_ISOLATED_ALLOCATION | EF_HAS_BEEN_UNSERIALIZED | EF_IS_SUSPENDED
	| EF_IS_ANONYMOUS_ALLOCATION | EF_SUSPEND_OUTSIDE_PVS;

/**
 * C++ class names to try against FindClassByName.
 *
 * Five, and only one has to hit. Which classes a build contains is a fact
 * about the game's content, not about the SDK, so demanding all five would
 * turn a map change into a red row for the wrong reason. A scope where not one
 * of them exists is a different matter.
 */
const char* const kProbeCppClassNames[] = {
	"CBaseEntity",
	"CWorld",
	"CCSPlayerController",
	"CBaseModelEntity",
	"CLogicRelay",
};

/** Designer (map) names for the same test against FindClassByDesignName. */
const char* const kProbeDesignerNames[] = {
	"worldent",
	"info_player_terrorist",
	"cs_player_controller",
	"logic_relay",
	"player",
};

/** A name no map contains, so the only correct answer is "not found". */
constexpr const char* kAbsentName = "cs2sdktest_no_such_entity";

// ---- helpers ----------------------------------------------------------------

/** Which mapped module an address falls inside, or nullptr. */
const char* ModuleOwning(TestContext& context, const void* address)
{
	if (address == nullptr)
	{
		return nullptr;
	}

	std::size_t count = 0;
	const GameModuleDesc* modules = GetGameModules(count);
	const unsigned char* target = static_cast<const unsigned char*>(address);

	for (std::size_t i = 0; i < count; ++i)
	{
		const Module& module = context.GetModule(modules[i].id);
		if (!module.IsValid() || module.base() == nullptr || module.size() == 0)
		{
			continue;
		}

		const unsigned char* base = static_cast<const unsigned char*>(module.base());
		if (target >= base && target < base + module.size())
		{
			return modules[i].id;
		}
	}

	return nullptr;
}

/** Reads the pointer-sized slot `offset` bytes into `object`. */
void* SlotAt(const void* object, std::size_t offset)
{
	const unsigned char* bytes = static_cast<const unsigned char*>(object);
	void* value = nullptr;
	// memcpy rather than a reinterpret_cast dereference: the offset is a
	// hypothesis, and nothing guarantees the SDK's idea of it is aligned.
	std::memcpy(&value, bytes + offset, sizeof(value));
	return value;
}

/**
 * A find filter that accepts everything and counts what it was asked about.
 *
 * The only way to exercise IEntityFindFilter::ShouldFindEntity is to hand one
 * to an iterator and let the iterator call back. Accepting everything keeps
 * the filtered walk comparable with the unfiltered one, which turns the filter
 * into a second check: the two counts have to match.
 */
class AcceptAllFindFilter : public IEntityFindFilter
{
public:
	bool ShouldFindEntity(CEntityInstance* entity) override
	{
		++asked;
		if (!LooksLikeObjectPointer(entity))
		{
			++implausible;
		}
		return true;
	}

	int asked = 0;
	int implausible = 0;
};

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool GameEntitySystemSuite::Acquire(TestContext& context)
{
	// Nothing but the factory query belongs here. This also runs on the hooks
	// install path, which is not wrapped in a FaultGuardScope, so a fault in
	// Acquire takes the server down with nothing in the log. The raw memory
	// read that finds the entity system happens in ExerciseCalls instead.
	resource_service_ = context.QueryInterface(
		"engine2", GAMERESOURCESERVICESERVER_INTERFACE_VERSION);

	if (resource_service_ == nullptr)
	{
		const Module& engine_module = context.GetModule("engine2");
		if (!engine_module.IsValid())
		{
			context.UnavailableCall("IGameResourceService", "engine2 is not loaded");
		}
		else
		{
			context.UnavailableCall("IGameResourceService",
				"engine2 is loaded but does not answer '%s'",
				GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("service at %p (%s)", resource_service_,
		GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	return true;
}

void GameEntitySystemSuite::ExerciseCalls(TestContext& context)
{
	entity_system_ = nullptr;
	entity_system_offset_ = static_cast<std::size_t>(-1);
	identity_layout_verified_ = false;
	probe_identity_ = nullptr;
	probe_instance_ = nullptr;
	probe_class_ = nullptr;
	probe_designer_name_ = nullptr;
	probe_cpp_name_ = nullptr;
	probe_entity_index_ = -1;
	named_identity_ = nullptr;
	named_entity_name_ = nullptr;
	active_entity_count_ = 0;

	ExerciseResourceServiceAppSystem(context);
	ResolveEntitySystem(context);

	if (entity_system_ == nullptr)
	{
		context.SkipCall("CGameEntitySystem",
			"the entity system pointer could not be located, so nothing below it can be read");
		NoteSideEffectingCalls(context);
		return;
	}

	// The vtable comes first for the same reason IAppSystem does elsewhere: if
	// slot 12 is not GetCurTime, the object is not the class the SDK names and
	// every field read below is meaningless.
	ExerciseEntitySystemVirtuals(context);

	InspectEntitySystemLayout(context);
	InspectGameEntitySystemLayout(context);
	InspectEntityListLayout(context);
	InspectEventQueueLayout(context);

	CollectProbeEntities(context);
	ExerciseIdentityLookups(context);
	InspectEntityIdentityLayout(context);

	ExerciseClassLookups(context);
	InspectEntityClassLayout(context);
	InspectEntityClassInfoLayout(context);

	ExerciseNameLookups(context);
	ExerciseIterators(context);

	ExerciseEntityInstanceVirtuals(context);
	InspectEntityInstanceLayout(context);

	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IGameResourceService - IAppSystem base slots, through an assumed cast
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::ExerciseResourceServiceAppSystem(TestContext& context)
{
	context.Note("[IGameResourceService] IAppSystem base slots");
	context.Note("the SDK declares IGameResourceService in engine2/igameresourceservice.h; "
	             "the cast to IAppSystem below is an assumption this section tests");

	// static_cast would now walk the SDK's own inheritance (CGameResourceService
	// derives from IEngineService derives from IAppSystem), but this section
	// exists to test the premise rather than repeat it: the service is
	// registered through a module's CreateInterface like every other app
	// system, which puts the IAppSystem subobject at offset 0 - and if that is
	// wrong, GetTier() below calls something else entirely and the answers stop
	// making sense immediately.
	IAppSystem* app_system = reinterpret_cast<IAppSystem*>(resource_service_);

	CS2ST_CALL(context, "IGameResourceService::GetTier", {
		const int tier = static_cast<int>(app_system->GetTier());
		context.Detail("-> %d", tier);
		if (tier < 0 || tier > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside AppSystemTier_t; this is not GetTier", tier);
		}
	});

	CS2ST_CALL(context, "IGameResourceService::IsSingleton", {
		const bool singleton = app_system->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the game resource service is a singleton; false suggests a slot shift");
		}
	});

	CS2ST_CALL(context, "IGameResourceService::GetBuildType", {
		const BuildType_t build = app_system->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)",
				static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IGameResourceService::GetDependencies", {
		const AppSystemInfo_t* deps = app_system->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}
		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
	});

	// The one query with a knowable right answer, and the cheapest test of the
	// cast above: an app system asked for its own version has to hand back
	// itself. Anything else and we are not talking to an app system.
	CS2ST_CALL(context, "IGameResourceService::QueryInterface", {
		void* self = app_system->QueryInterface(GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
		context.Detail("-> %p%s", self, (self == resource_service_) ? " (itself)" : "");
		if (self != nullptr && self != resource_service_)
		{
			context.Fail("asked for its own version and got a different object");
		}
	});

	context.SkipCall("IGameResourceService::Connect", "would re-run engine startup");
	context.SkipCall("IGameResourceService::Init", "would re-run engine startup");
	context.SkipCall("IGameResourceService::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IGameResourceService::PreShutdown", "would begin engine shutdown");
	context.SkipCall("IGameResourceService::Shutdown", "would shut the engine down");
	context.SkipCall("IGameResourceService::Disconnect", "would tear down engine interfaces");
}

// -----------------------------------------------------------------------------
//  Finding CGameEntitySystem - the fragile point, turned into a measurement
// -----------------------------------------------------------------------------

bool GameEntitySystemSuite::LooksLikeEntitySystem(const void* candidate,
                                                  const char** why) const
{
	*why = "";

	if (!LooksLikeObjectPointer(candidate))
	{
		*why = "not a plausible object pointer";
		return false;
	}

	// A CGameEntitySystem is polymorphic, so byte 0 is a vtable pointer.
	const void* vtable = SlotAt(candidate, 0);
	if (!LooksLikeObjectPointer(vtable))
	{
		*why = "no plausible vtable pointer at +0";
		return false;
	}

	const CGameEntitySystem* system = static_cast<const CGameEntitySystem*>(candidate);
	const CConcreteEntityList& list = system->m_EntityList;

	// The identity table is a fixed 64 x 512 grid, so every count and limit in
	// it is bounded by the declaration itself rather than by anything about how
	// the server is configured. That is what makes these usable as a filter.
	const int used = list.m_usedList.m_nCount;
	const int dormant = list.m_dormantList.m_nCount;
	if (used < 0 || used > MAX_TOTAL_ENTITIES)
	{
		*why = "used-list count is outside the fixed identity table";
		return false;
	}
	if (dormant < 0 || dormant > MAX_TOTAL_ENTITIES)
	{
		*why = "dormant-list count is outside the fixed identity table";
		return false;
	}
	if (list.m_nNetworkableEntityLimit > static_cast<std::uint32_t>(MAX_TOTAL_ENTITIES)
		|| list.m_nNonNetworkableEntityLimit > static_cast<std::uint32_t>(MAX_TOTAL_ENTITIES))
	{
		*why = "an entity limit exceeds the fixed identity table";
		return false;
	}

	// A count and the pointer it counts cannot disagree.
	if (used > 0 && !LooksLikeObjectPointer(list.m_usedList.m_pHead))
	{
		*why = "used-list has a count but no plausible head";
		return false;
	}
	if (list.m_pFirstActiveEntity != nullptr
		&& !LooksLikeObjectPointer(list.m_pFirstActiveEntity))
	{
		*why = "m_pFirstActiveEntity is neither null nor a plausible pointer";
		return false;
	}

	return true;
}

void GameEntitySystemSuite::ResolveEntitySystem(TestContext& context)
{
	context.Note("[CGameEntitySystem] locating the instance");
	context.Note("the SDK's GameEntitySystem() reads the service's declared members; "
	             "this scan reads raw slots at the offset the plugin ecosystem "
	             "agrees on and nobody verifies");

	// Printed whether or not the assumed offset works, because the raw slot
	// values are what someone reads when it stops working.
	CS2ST_CALL(context, "IGameResourceService raw pointer slots", {
		int plausible = 0;
		for (std::size_t offset = 0; offset < kEntitySystemScanLimit;
		     offset += sizeof(void*))
		{
			if (LooksLikeObjectPointer(SlotAt(resource_service_, offset)))
			{
				++plausible;
			}
		}
		context.Detail("-> %d of %zu slots in the first 0x%zx bytes hold a plausible pointer",
			plausible, kEntitySystemScanLimit / sizeof(void*), kEntitySystemScanLimit);
	});

	CS2ST_CALL(context, "CGameEntitySystem @ the assumed offset", {
		void* candidate = SlotAt(resource_service_, kAssumedEntitySystemOffset);
		const char* why = "";

		if (LooksLikeEntitySystem(candidate, &why))
		{
			context.Detail("+0x%zx -> %p accepted", kAssumedEntitySystemOffset, candidate);
			entity_system_ = static_cast<CGameEntitySystem*>(candidate);
			entity_system_offset_ = kAssumedEntitySystemOffset;
		}
		else
		{
			context.Detail("+0x%zx -> %p rejected: %s",
				kAssumedEntitySystemOffset, candidate, why);
		}
	});

	// Only when the assumption failed. In the normal case this whole block is
	// absent from the log, and its presence is itself the headline.
	if (entity_system_ == nullptr)
	{
		context.Note("the assumed offset did not hold - sweeping the neighbouring slots");

		for (std::size_t offset = 0;
		     offset < kEntitySystemScanLimit && entity_system_ == nullptr;
		     offset += sizeof(void*))
		{
			if (offset == kAssumedEntitySystemOffset)
			{
				continue;
			}

			char target[64];
			std::snprintf(target, sizeof(target),
				"CGameEntitySystem candidate @ +0x%zx", offset);

			// One guarded call per candidate: reading through a pointer that
			// only looks plausible can fault, and that must cost this row
			// rather than the sweep.
			CS2ST_CALL(context, target, {
				void* candidate = SlotAt(resource_service_, offset);
				const char* why = "";
				if (LooksLikeEntitySystem(candidate, &why))
				{
					context.Detail("-> %p ACCEPTED", candidate);
					entity_system_ = static_cast<CGameEntitySystem*>(candidate);
					entity_system_offset_ = offset;
				}
				else
				{
					context.Detail("-> %p rejected: %s", candidate, why);
				}
			});
		}
	}

	CS2ST_CALL(context, "CGameEntitySystem offset verdict", {
		if (entity_system_ == nullptr)
		{
			context.Detail("-> not found in the first 0x%zx bytes of the service object",
				kEntitySystemScanLimit);
			context.Fail("no slot in the service object holds anything that behaves like "
			             "an entity system; every CS2 plugin that reads +0x%zx is reading "
			             "the wrong eight bytes",
				kAssumedEntitySystemOffset);
			return;
		}

		const void* vtable = SlotAt(entity_system_, 0);
		const char* owner = ModuleOwning(context, vtable);

		context.Detail("-> %p at +0x%zx, vtable %p in %s",
			static_cast<void*>(entity_system_), entity_system_offset_,
			vtable, owner != nullptr ? owner : "<no mapped module>");

		if (entity_system_offset_ != kAssumedEntitySystemOffset)
		{
			context.Fail("found at +0x%zx, not the assumed +0x%zx - every published CS2 "
			             "plugin reads the assumed one",
				entity_system_offset_, kAssumedEntitySystemOffset);
		}
		else if (owner == nullptr)
		{
			// Advisory rather than fatal: the module table is a list of names,
			// and a library it does not name would produce this too.
			context.Suspect("its vtable is not inside any module this plugin knows by name");
		}
		else if (std::strcmp(owner, "server") != 0)
		{
			context.Suspect("its vtable lives in %s; the game DLL constructs the entity "
			                "system, so 'server' is what to expect", owner);
		}
	});

	// The SDK's own accessor walks g_pGameResourceServiceServer's declared
	// members - a description that shares no code with this suite's byte-offset
	// scan. Agreement is an oracle for both; disagreement means the declared
	// layout (or the ecosystem's offset) has drifted.
	CS2ST_CALL(context, "CGameEntitySystem vs the SDK's own GameEntitySystem()", {
		CGameEntitySystem* from_sdk = GameEntitySystem();
		context.Detail("-> SDK accessor %p, slot scan %p",
			static_cast<void*>(from_sdk), static_cast<void*>(entity_system_));
		if (from_sdk != entity_system_)
		{
			context.Fail("the SDK's GameEntitySystem() and this suite's scan disagree; "
			             "one of them is reading the wrong member");
		}
	});
}

// -----------------------------------------------------------------------------
//  CEntitySystem - the callable part of the vtable
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::ExerciseEntitySystemVirtuals(TestContext& context)
{
	context.Note("[CEntitySystem] virtuals - slots 0-9 belong to "
	             "IEntityResourceManifestBuilder and all ten mutate, so the first "
	             "callable slot is 12 (after the destructor pair at 10-11)");

	CS2ST_CALL(context, "CEntitySystem::GetCurTime", {
		const GameTime_t now = entity_system_->GetCurTime(WorldGroupId_t());
		const float seconds = now.GetTime();
		context.Detail("-> %.4f", static_cast<double>(seconds));
		// Same criterion StructInspector applies to a float field, and the same
		// reason: NaN and infinity are a large, easily-hit part of the encoding,
		// so a finite answer is real evidence and a non-finite one is proof.
		if (!std::isfinite(seconds))
		{
			context.Fail("a game time cannot be NaN or infinite; slot 12 is not GetCurTime");
		}
	});

	// An independent reading of the same clock. Not asserted equal - world
	// groups can legitimately run their own time - but a value that tracks
	// curtime is what says slot 12 really is the time getter.
	CS2ST_CALL(context, "CEntitySystem::GetCurTime vs CGlobalVars::curtime", {
		IVEngineServer2* engine = static_cast<IVEngineServer2*>(
			context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
		if (engine == nullptr)
		{
			context.Detail("-> engine2 does not answer %s", INTERFACEVERSION_VENGINESERVER);
			return;
		}

		CGlobalVars* globals = engine->GetServerGlobals();
		if (globals == nullptr)
		{
			context.Detail("-> no server globals to compare against");
			return;
		}

		const float from_system = entity_system_->GetCurTime(WorldGroupId_t()).GetTime();
		const float from_globals = globals->curtime;
		context.Detail("entity system %.4f vs globals %.4f (delta %.4f)",
			static_cast<double>(from_system), static_cast<double>(from_globals),
			static_cast<double>(from_system - from_globals));
	});

	CS2ST_CALL(context, "CEntitySystem::FindEntityProcedural", {
		// A procedural name the game cannot possibly resolve. Read-only: the
		// resolver is a comparison chain over "!self", "!activator" and
		// friends, and nothing is created when none of them match.
		CEntityInstance* found = entity_system_->FindEntityProcedural("!cs2sdktest_no_such_procedural");
		context.Detail("-> %p for an unknown procedural name", static_cast<void*>(found));
		if (found != nullptr && !LooksLikeObjectPointer(found))
		{
			context.Fail("returned something that is not a pointer");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::GetSpawnGroupWorldId", {
		// WorldGroupId_t lost its GetHashCode()/IsValid() helpers in the
		// 2026-10 SDK - the AMNOTE on the class says the high bit marks
		// client-side groups and -1 (all bits set) is invalid, so the bits
		// carry all the meaning this row ever had.
		const WorldGroupId_t world_id = entity_system_->GetSpawnGroupWorldId(0);
		context.Detail("-> id 0x%08x for spawn group 0", world_id.m_nId);
	});

	// The ten IEntityResourceManifestBuilder slots, one line each. Every one of
	// them builds or retains a resource manifest, so none can run on a live
	// server - but they are also slots 0-9 of this vtable, which is why they
	// are listed here rather than filed away with the other side effects.
	context.SkipCall("IEntityResourceManifestBuilder::BuildResourceManifest(spawn group, EntitySpawnInfo_t[])",
		"builds a resource manifest and precaches through it");
	context.SkipCall("IEntityResourceManifestBuilder::BuildResourceManifest(spawn group, CEntityKeyValues[])",
		"builds a resource manifest and precaches through it");
	context.SkipCall("IEntityResourceManifestBuilder::BuildResourceManifest(spawn group, keyvalues vector, filter)",
		"builds a resource manifest and precaches through it");
	context.SkipCall("IEntityResourceManifestBuilder::BuildResourceManifest(manifest or group name)",
		"builds a resource manifest and precaches through it");
	context.SkipCall("IEntityResourceManifestBuilder::BuildResourceManifest(callback)",
		"calls back into caller-supplied code with a manifest under construction");
	context.SkipCall("IEntityResourceManifestBuilder::BuildResourceManifestForEntity",
		"allocates entity keyvalues into a caller-supplied CKV3Arena");
	context.SkipCall("IEntityResourceManifestBuilder::InvokePrecacheCallback",
		"invokes a member function pointer on an entity to precache resources");
	context.SkipCall("IEntityResourceManifestBuilder::AddRefKeyValues",
		"takes a reference the engine expects somebody to release");
	context.SkipCall("IEntityResourceManifestBuilder::ReleaseKeyValues",
		"drops a reference this plugin never took");
	context.SkipCall("IEntityResourceManifestBuilder::LockResourceManifest",
		"locks the manifest builder and writes through the context pointer");
}

// -----------------------------------------------------------------------------
//  Walking the active entity list
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::CollectProbeEntities(TestContext& context)
{
	context.Note("[CConcreteEntityList] walking the active entity chain");

	CS2ST_CALL(context, "CEntitySystem::m_EntityList.m_pFirstActiveEntity (walk)", {
		CEntityIdentity* current = entity_system_->m_EntityList.m_pFirstActiveEntity;
		if (current == nullptr)
		{
			context.Detail("-> the active chain is empty");
			return;
		}

		int walked = 0;
		int detailed = 0;
		int broken_links = 0;

		while (current != nullptr && walked < kMaxEntitiesScanned)
		{
			++walked;

			const char* designer = current->GetClassname();
			const int index = current->GetEntityIndex().Get();

			if (detailed < kMaxEntitiesDetailed)
			{
				// Printed one at a time so a fault names the entity it happened
				// on rather than just "somewhere in the walk".
				char scratch[64];
				context.Note("  #%d index %d %p designer \"%.63s\"",
					walked, index, static_cast<void*>(current),
					SafeStringArg(designer, scratch));
				++detailed;
			}

			// The first entity that answers everything the rest of the suite
			// needs becomes the probe. Taking it from the live list rather than
			// naming a class keeps the suite working after a content update.
			if (probe_identity_ == nullptr
				&& LooksLikeObjectPointer(current->m_pInstance)
				&& LooksLikeObjectPointer(current->m_pClass)
				&& index >= 0 && index < MAX_TOTAL_ENTITIES)
			{
				probe_identity_ = current;
				probe_instance_ = current->m_pInstance;
				probe_class_ = current->m_pClass;
				probe_designer_name_ = designer;
				probe_entity_index_ = index;
			}

			// A targetname is what the by-name lookups need, and most entities
			// do not have one.
			if (named_identity_ == nullptr && current->m_name.IsValid()
				&& current->m_name.String()[0] != '\0')
			{
				named_identity_ = current;
				named_entity_name_ = current->m_name.String();
			}

			CEntityIdentity* next = current->m_pNext;
			if (next != nullptr && !LooksLikeObjectPointer(next))
			{
				++broken_links;
				break;
			}
			current = next;
		}

		active_entity_count_ = walked;
		context.Detail("-> %d entit%s%s, probe index %d (\"%s\"), named probe \"%s\"",
			walked, walked == 1 ? "y" : "ies",
			walked == kMaxEntitiesScanned ? " (capped)" : "",
			probe_entity_index_,
			probe_designer_name_ != nullptr ? probe_designer_name_ : "<none>",
			named_entity_name_ != nullptr ? named_entity_name_ : "<none>");

		if (broken_links > 0)
		{
			context.Fail("the chain reached a next pointer that is not a pointer");
		}
	});

	if (probe_identity_ == nullptr)
	{
		context.Note("no usable entity was found; the identity, class and instance "
		             "sections below have nothing to run against");
	}
	else
	{
		// Cached now because everything downstream reads it, and reading it
		// once inside a guarded call is cheaper than guarding every use.
		CS2ST_CALL(context, "CEntityClass::m_pClassInfo->m_pszCPPClassname (probe)", {
			CEntityClassInfo* info = probe_class_->m_pClassInfo;
			if (!LooksLikeObjectPointer(info))
			{
				context.Detail("-> the probe class has no plausible class info");
				return;
			}
			probe_cpp_name_ = info->m_pszCPPClassname;
			char scratch[64];
			context.Detail("-> \"%.63s\"", SafeStringArg(probe_cpp_name_, scratch));
		});
	}
}

// -----------------------------------------------------------------------------
//  Identity lookups - the strongest oracles in the suite
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::ExerciseIdentityLookups(TestContext& context)
{
	context.Note("[CEntitySystem] identity lookups - chunk arithmetic against the chain");

	if (probe_identity_ == nullptr)
	{
		context.SkipCall("CEntitySystem::GetEntityIdentity(CEntityIndex)",
			"no live entity was found to look up");
		context.SkipCall("CEntitySystem::GetEntityIdentity(CEntityHandle)",
			"no live entity was found to look up");
		context.SkipCall("CEntitySystem::GetEntityInstance(CEntityIdentity*)",
			"no live entity was found to look up");
		context.SkipCall("CEntitySystem::GetEntityInstance(CEntityIndex)",
			"no live entity was found to look up");
		context.SkipCall("CEntitySystem::GetEntityInstance(CEntityHandle)",
			"no live entity was found to look up");
		return;
	}

	bool index_ok = false;
	bool handle_ok = false;
	bool instance_ok = false;

	// The best check available here. Reaching an entity by following m_pNext
	// and reaching it by indexing m_pIdentityChunks[i / 512][i % 512] are two
	// descriptions of one object that share no code: agreement confirms the
	// chunk array's offset, the entity-index bitfield inside CEntityHandle, and
	// sizeof(CEntityIdentity) all at once, because the second route computes an
	// address the first one only ever followed.
	CS2ST_CALL(context, "CEntitySystem::GetEntityIdentity(CEntityIndex)", {
		CEntityIdentity* by_index =
			entity_system_->GetEntityIdentity(CEntityIndex(probe_entity_index_));
		context.Detail("index %d -> %p, chain gave %p",
			probe_entity_index_, static_cast<void*>(by_index),
			static_cast<void*>(probe_identity_));

		if (by_index != probe_identity_)
		{
			context.Fail("the chunk table and the active chain disagree about entity %d; "
			             "either m_pIdentityChunks moved or CEntityIdentity changed size",
				probe_entity_index_);
			return;
		}
		index_ok = true;
	});

	CS2ST_CALL(context, "CEntitySystem::GetEntityIdentity(CEntityHandle)", {
		const CEntityHandle handle = probe_identity_->GetRefEHandle();
		CEntityIdentity* by_handle = entity_system_->GetEntityIdentity(handle);
		context.Detail("handle %d:%d -> %p",
			handle.GetEntryIndex(), handle.GetSerialNumber(),
			static_cast<void*>(by_handle));

		if (by_handle != probe_identity_)
		{
			// The handle overload additionally compares serial numbers, so this
			// failing while the index overload passed points at m_EHandle or
			// m_flags rather than at the chunk table.
			context.Fail("the handle overload found a different object than the index one");
			return;
		}
		handle_ok = true;
	});

	CS2ST_CALL(context, "CEntitySystem::GetEntityInstance(CEntityIdentity*)", {
		CEntityInstance* instance = entity_system_->GetEntityInstance(probe_identity_);
		context.Detail("-> %p, m_pInstance %p", static_cast<void*>(instance),
			static_cast<void*>(probe_instance_));
		if (instance != probe_instance_)
		{
			context.Fail("the accessor and the field it reads disagree");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::GetEntityInstance(CEntityIndex)", {
		CEntityInstance* instance =
			entity_system_->GetEntityInstance(CEntityIndex(probe_entity_index_));
		context.Detail("index %d -> %p", probe_entity_index_,
			static_cast<void*>(instance));
		if (instance != probe_instance_)
		{
			context.Fail("looking the same entity up by index produced a different instance");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::GetEntityInstance(CEntityHandle)", {
		CEntityInstance* instance =
			entity_system_->GetEntityInstance(probe_identity_->GetRefEHandle());
		context.Detail("-> %p", static_cast<void*>(instance));
		if (instance != probe_instance_)
		{
			context.Fail("looking the same entity up by handle produced a different instance");
		}
	});

	// The back pointer closes the loop: identity -> instance -> identity has to
	// return where it started, which is the cheapest way to tell that
	// CEntityInstance::m_pEntity is where the SDK thinks it is.
	CS2ST_CALL(context, "CEntityInstance::m_pEntity (round trip)", {
		CEntityIdentity* back = probe_instance_->m_pEntity;
		context.Detail("instance %p -> identity %p (expected %p)",
			static_cast<void*>(probe_instance_), static_cast<void*>(back),
			static_cast<void*>(probe_identity_));
		if (back != probe_identity_)
		{
			context.Fail("the instance does not point back at the identity that owns it");
			return;
		}
		instance_ok = true;
	});

	CS2ST_CALL(context, "CEntityInstance::GetRefEHandle vs CEntityIdentity::GetRefEHandle", {
		// CEntityInstance::GetRefEHandle is a one-line forward through
		// m_pEntity, so this pins the forwarding rather than the handle itself.
		const CEntityHandle from_instance = probe_instance_->GetRefEHandle();
		const CEntityHandle from_identity = probe_identity_->GetRefEHandle();
		context.Detail("instance %d:%d vs identity %d:%d",
			from_instance.GetEntryIndex(), from_instance.GetSerialNumber(),
			from_identity.GetEntryIndex(), from_identity.GetSerialNumber());
		if (from_instance != from_identity)
		{
			context.Fail("the two routes to one entity handle disagree");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::GetEntityIdentity (out of range)", {
		// -1 and the last index are the two the SDK's own bounds check rejects,
		// so null is the only correct answer and a non-null one means the
		// bounds test is reading something other than the index.
		CEntityIdentity* negative = entity_system_->GetEntityIdentity(CEntityIndex(-1));
		CEntityIdentity* past_end =
			entity_system_->GetEntityIdentity(CEntityIndex(MAX_TOTAL_ENTITIES));
		context.Detail("index -1 -> %p, index %d -> %p",
			static_cast<void*>(negative), MAX_TOTAL_ENTITIES,
			static_cast<void*>(past_end));
		if (negative != nullptr || past_end != nullptr)
		{
			context.Fail("an out-of-range entity index resolved to an identity");
		}
	});

	identity_layout_verified_ = index_ok && handle_ok && instance_ok;
	context.Note("identity layout %s - the hook callbacks read one hop past a handed "
	             "pointer only when it is",
		identity_layout_verified_ ? "verified" : "NOT verified");
}

// -----------------------------------------------------------------------------
//  Class lookups
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::ExerciseClassLookups(TestContext& context)
{
	context.Note("[CEntitySystem] entity class maps");

	// Reading the two ordered maps' counts is also the cheapest probe of the
	// middle of CEntitySystem: each Count() is a load at a different offset
	// nearly 3 KB into the object, so a plausible answer from each one pins
	// that span of the declaration.
	CS2ST_CALL(context, "CEntitySystem::m_entClassesByCPPClassname.Count", {
		const int count = static_cast<int>(entity_system_->m_entClassesByCPPClassname.Count());
		context.Detail("@0x%zx -> %d",
			static_cast<std::size_t>(offsetof(CEntitySystem, m_entClassesByCPPClassname)),
			count);
		if (count > 0 && !entity_system_->m_entClassesByCPPClassname.IsValidIndex(0))
		{
			context.Fail("the map reports %d entries and then rejects index 0", count);
		}
	});

	CS2ST_CALL(context, "CEntitySystem::m_entClassesByClassname.Count", {
		const int count = static_cast<int>(entity_system_->m_entClassesByClassname.Count());
		context.Detail("@0x%zx -> %d",
			static_cast<std::size_t>(offsetof(CEntitySystem, m_entClassesByClassname)),
			count);
		if (count > 0 && !entity_system_->m_entClassesByClassname.IsValidIndex(0))
		{
			context.Fail("the map reports %d entries and then rejects index 0", count);
		}
	});

	CS2ST_CALL(context, "CEntitySystem::m_entityComponentHelpers.Count", {
		context.Detail("@0x%zx -> %d",
			static_cast<std::size_t>(offsetof(CEntitySystem, m_entityComponentHelpers)),
			static_cast<int>(entity_system_->m_entityComponentHelpers.Count()));
	});

	CS2ST_CALL(context, "CEntitySystem::m_entityNames.Count", {
		context.Detail("@0x%zx -> %d",
			static_cast<std::size_t>(offsetof(CEntitySystem, m_entityNames)),
			static_cast<int>(entity_system_->m_entityNames.Count()));
	});

	// Probe names first: they exercise the maps without depending on the walk
	// having found anything.
	CS2ST_CALL(context, "CEntitySystem::FindClassByName (probe names)", {
		int hits = 0;
		for (const char* name : kProbeCppClassNames)
		{
			CEntityClass* found = entity_system_->FindClassByName(name);
			context.Note("  FindClassByName(\"%s\") -> %p", name,
				static_cast<void*>(found));
			if (found != nullptr)
			{
				++hits;
			}
		}
		context.Detail("-> %d of %zu probe names resolved", hits,
			sizeof(kProbeCppClassNames) / sizeof(kProbeCppClassNames[0]));
		// Which classes exist is content, not SDK, so no individual name is
		// required. A map in which not one of them exists is not a map.
		if (hits == 0)
		{
			context.Fail("not one probe C++ class name resolved; "
			             "m_entClassesByCPPClassname is not being read as a map");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::FindClassByDesignName (probe names)", {
		int hits = 0;
		for (const char* name : kProbeDesignerNames)
		{
			CEntityClass* found = entity_system_->FindClassByDesignName(name);
			context.Note("  FindClassByDesignName(\"%s\") -> %p", name,
				static_cast<void*>(found));
			if (found != nullptr)
			{
				++hits;
			}
		}
		context.Detail("-> %d of %zu probe names resolved", hits,
			sizeof(kProbeDesignerNames) / sizeof(kProbeDesignerNames[0]));
		if (hits == 0)
		{
			context.Fail("not one probe designer name resolved; "
			             "m_entClassesByClassname is not being read as a map");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::FindClassByName (absent name)", {
		// A matcher that only ever says yes is as broken as one that only ever
		// says no, and the second half is free to test.
		CEntityClass* found = entity_system_->FindClassByName(kAbsentName);
		context.Detail("-> %p for \"%s\"", static_cast<void*>(found), kAbsentName);
		if (found != nullptr)
		{
			context.Fail("a class name no build contains resolved to a class");
		}
	});

	if (probe_class_ == nullptr)
	{
		context.SkipCall("CEntitySystem::FindClassByDesignName (live entity)",
			"no live entity was found to take a class name from");
		context.SkipCall("CEntitySystem::FindClassByName (live entity)",
			"no live entity was found to take a C++ class name from");
		return;
	}

	// The oracle version of the two calls above: the name comes off a live
	// entity, and the map has to hand back the very pointer that entity holds.
	CS2ST_CALL(context, "CEntitySystem::FindClassByDesignName (live entity)", {
		CEntityClass* found = entity_system_->FindClassByDesignName(probe_designer_name_);
		char scratch[64];
		context.Detail("\"%.63s\" -> %p, the entity holds %p",
			SafeStringArg(probe_designer_name_, scratch),
			static_cast<void*>(found), static_cast<void*>(probe_class_));
		if (found != probe_class_)
		{
			context.Fail("the designer-name map and CEntityIdentity::m_pClass disagree");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::FindClassByName (live entity)", {
		if (probe_cpp_name_ == nullptr)
		{
			context.Detail("-> the probe class exposed no C++ name");
			return;
		}
		CEntityClass* found = entity_system_->FindClassByName(probe_cpp_name_);
		char scratch[64];
		context.Detail("\"%.63s\" -> %p, the entity holds %p",
			SafeStringArg(probe_cpp_name_, scratch),
			static_cast<void*>(found), static_cast<void*>(probe_class_));
		if (found != probe_class_)
		{
			context.Fail("the C++-name map and CEntityIdentity::m_pClass disagree");
		}
	});

	CS2ST_CALL(context, "CEntityClass::m_designerName vs CEntityIdentity::GetClassname", {
		// Two places store the same designer name, and only one of them is the
		// one lookups go through.
		const char* from_class = probe_class_->m_designerName.String();
		const char* from_identity = probe_identity_->GetClassname();
		char a[64];
		char b[64];
		context.Detail("class \"%.63s\" vs identity \"%.63s\"",
			SafeStringArg(from_class, a), SafeStringArg(from_identity, b));
		if (from_class != from_identity)
		{
			// Pooled symbols, so the pointers themselves should match; compare
			// them rather than the text, which is the stricter statement.
			context.Fail("the class and the identity name the same entity differently");
		}
	});

	CS2ST_CALL(context, "CEntityClass::GetSchemaBinding", {
		CSchemaClassInfo* binding = probe_class_->GetSchemaBinding();
		context.Detail("-> %p", static_cast<void*>(binding));
		if (binding != nullptr && !LooksLikeObjectPointer(binding))
		{
			context.Fail("the schema binding is neither null nor a pointer");
		}
	});

	CS2ST_CALL(context, "CEntityClass::GetDataDescMap", {
		context.Detail("-> %p", static_cast<void*>(probe_class_->GetDataDescMap()));
	});
}

// -----------------------------------------------------------------------------
//  Name lookups and the string pool
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::ExerciseNameLookups(TestContext& context)
{
	context.Note("[CEntitySystem] pooled strings and by-name lookup");

	CS2ST_CALL(context, "CEntitySystem::FindPooledString (absent)", {
		const CUtlSymbolLarge symbol = entity_system_->FindPooledString(kAbsentName);
		context.Detail("-> valid=%s for \"%s\"",
			symbol.IsValid() ? "true" : "false", kAbsentName);
		if (symbol.IsValid())
		{
			context.Fail("a string never pooled came back as a live symbol");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::FindPooledString (empty)", {
		// The SDK's own early-out. Cheap, and it is the one input whose answer
		// does not depend on the game's state at all.
		const CUtlSymbolLarge symbol = entity_system_->FindPooledString("");
		context.Detail("-> valid=%s for the empty string",
			symbol.IsValid() ? "true" : "false");
		if (symbol.IsValid())
		{
			context.Fail("the empty string produced a symbol");
		}
	});

	if (named_identity_ == nullptr)
	{
		context.SkipCall("CEntitySystem::FindPooledString (live name)",
			"no entity on this map carries a targetname");
		context.SkipCall("CEntitySystem::FindFirstEntityHandleByName",
			"no entity on this map carries a targetname");
		return;
	}

	CS2ST_CALL(context, "CEntitySystem::FindPooledString (live name)", {
		// A name read off a live entity is by construction in the pool, and the
		// symbol is a pointer into it - so it should come back identical to the
		// one the entity holds, not merely equal as text.
		const CUtlSymbolLarge symbol = entity_system_->FindPooledString(named_entity_name_);
		char scratch[64];
		context.Detail("\"%.63s\" -> %p, entity holds %p",
			SafeStringArg(named_entity_name_, scratch),
			static_cast<const void*>(symbol.String()),
			static_cast<const void*>(named_identity_->m_name.String()));
		if (!symbol.IsValid())
		{
			context.Fail("a name taken off a live entity is not in the string pool");
		}
		else if (symbol != named_identity_->m_name)
		{
			context.Fail("the pool returned a different symbol than the entity carries");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::FindFirstEntityHandleByName", {
		// Runs EntityInstanceByNameIter_t end to end: pooled-string lookup, the
		// m_entityNames map, the handle vector it stores, and back through
		// GetEntityIdentity. Several thousand bytes of assumed layout in one
		// call, with a known right answer.
		const CEntityHandle handle =
			entity_system_->FindFirstEntityHandleByName(named_entity_name_);
		char scratch[64];
		context.Detail("\"%.63s\" -> handle %d:%d",
			SafeStringArg(named_entity_name_, scratch),
			handle.GetEntryIndex(), handle.GetSerialNumber());

		if (!handle.IsValid())
		{
			context.Fail("the name of a live entity resolved to no handle");
			return;
		}

		CEntityIdentity* found = entity_system_->GetEntityIdentity(handle);
		if (found == nullptr)
		{
			context.Fail("the handle it returned resolves to no identity");
			return;
		}
		if (found->m_name != named_identity_->m_name)
		{
			context.Fail("the entity it found carries a different name");
		}
	});

	CS2ST_CALL(context, "CEntitySystem::FindFirstEntityHandleByName (absent)", {
		const CEntityHandle handle = entity_system_->FindFirstEntityHandleByName(kAbsentName);
		context.Detail("-> valid=%s for \"%s\"",
			handle.IsValid() ? "true" : "false", kAbsentName);
		if (handle.IsValid())
		{
			context.Fail("a name no map contains resolved to an entity");
		}
	});
}

// -----------------------------------------------------------------------------
//  The three iterators
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::ExerciseIterators(TestContext& context)
{
	context.Note("[EntityInstance*Iter_t] the SDK's own traversal code");

	// Negative until the plain walk has actually completed, so a fault in it
	// cannot turn into a bogus failure in the filtered walk below.
	int iterated = -1;

	CS2ST_CALL(context, "EntityInstanceIter_t::First / Next", {
		// Same chain the walk above followed, reached through the SDK's
		// iterator instead of by hand. A different count means the iterator's
		// filtering - the EF_MARKED_FOR_DELETE test, the world group compare -
		// is reading fields that have moved.
		EntityInstanceIter_t iter;
		int count = 0;
		for (CEntityInstance* entity = iter.First();
		     entity != nullptr && count < kMaxEntitiesScanned;
		     entity = iter.Next())
		{
			++count;
		}
		iterated = count;
		context.Detail("-> %d instance(s), the manual walk reached %d",
			count, active_entity_count_);
		if (count > active_entity_count_ && active_entity_count_ < kMaxEntitiesScanned)
		{
			context.Fail("the iterator produced more entities than the chain contains");
		}
	});

	CS2ST_CALL(context, "EntityInstanceIter_t + IEntityFindFilter::ShouldFindEntity", {
		// The only way to reach ShouldFindEntity is to be called back through
		// it. A filter that accepts everything has to leave the count alone.
		AcceptAllFindFilter filter;
		EntityInstanceIter_t iter(&filter);
		int count = 0;
		for (CEntityInstance* entity = iter.First();
		     entity != nullptr && count < kMaxEntitiesScanned;
		     entity = iter.Next())
		{
			++count;
		}
		context.Detail("-> %d instance(s), filter asked %d time(s), %d implausible",
			count, filter.asked, filter.implausible);
		if (filter.asked == 0 && count > 0)
		{
			context.Fail("the iterator returned entities without consulting the filter");
		}
		if (filter.implausible > 0)
		{
			context.Fail("the iterator handed the filter %d value(s) that are not pointers",
				filter.implausible);
		}
		if (iterated >= 0 && count != iterated)
		{
			context.Fail("an accept-everything filter changed the result from %d to %d",
				iterated, count);
		}
	});

	CS2ST_CALL(context, "EntityInstanceIter_t (dormant)", {
		EntityInstanceIter_t iter(nullptr, ENTITY_ITER_OVER_DORMANT);
		int count = 0;
		for (CEntityInstance* entity = iter.First();
		     entity != nullptr && count < kMaxEntitiesScanned;
		     entity = iter.Next())
		{
			++count;
		}
		// No assertion on the number: how many entities are dormant is a fact
		// about the map, not about the SDK.
		context.Detail("-> %d dormant instance(s), m_dormantList.m_nCount says %d",
			count, entity_system_->m_EntityList.m_dormantList.m_nCount);
	});

	if (probe_designer_name_ == nullptr)
	{
		context.SkipCall("EntityInstanceByClassIter_t",
			"no live entity was found to take a class name from");
	}
	else
	{
		CS2ST_CALL(context, "EntityInstanceByClassIter_t::First / Next", {
			// Two genuinely different chains: this walks m_pFirstEntity down
			// m_pNextByClass, while the count it is compared against comes from
			// walking the global chain and comparing designer names. They index
			// the same set through different pointers in CEntityIdentity.
			EntityInstanceByClassIter_t iter(probe_designer_name_);
			int by_class = 0;
			for (CEntityInstance* entity = iter.First();
			     entity != nullptr && by_class < kMaxEntitiesScanned;
			     entity = iter.Next())
			{
				++by_class;
			}

			int by_scan = 0;
			CEntityIdentity* current = entity_system_->m_EntityList.m_pFirstActiveEntity;
			for (int walked = 0;
			     current != nullptr && walked < kMaxEntitiesScanned;
			     ++walked, current = current->m_pNext)
			{
				if ((current->m_flags & EF_MARKED_FOR_DELETE) != 0)
				{
					continue;
				}
				if (current->m_designerName == CUtlSymbolLarge(probe_designer_name_))
				{
					++by_scan;
				}
			}

			char scratch[64];
			context.Detail("\"%.63s\" -> %d by class chain, %d by scanning the global chain",
				SafeStringArg(probe_designer_name_, scratch), by_class, by_scan);
			if (by_class != by_scan)
			{
				context.Fail("the by-class chain and the global chain disagree about how "
				             "many entities have this class");
			}
		});
	}

	if (named_entity_name_ == nullptr)
	{
		context.SkipCall("EntityInstanceByNameIter_t",
			"no entity on this map carries a targetname");
	}
	else
	{
		CS2ST_CALL(context, "EntityInstanceByNameIter_t::First / Next", {
			EntityInstanceByNameIter_t iter(named_entity_name_);
			CEntityInstance* first = iter.First();
			int count = 0;
			for (CEntityInstance* entity = first;
			     entity != nullptr && count < kMaxEntitiesScanned;
			     entity = iter.Next())
			{
				++count;
			}
			char scratch[64];
			context.Detail("\"%.63s\" -> %d instance(s), first %p",
				SafeStringArg(named_entity_name_, scratch), count,
				static_cast<void*>(first));
			if (count == 0)
			{
				context.Fail("iterating by a name taken off a live entity found nothing");
			}
		});
	}

	CS2ST_CALL(context, "EntityInstanceByNameIter_t (wildcard)", {
		// The '*' branch takes a different route entirely - it walks the global
		// chain and calls CEntityIdentity::NameMatches per entity, which is the
		// only thing that reaches V_CompareNameWithWildcards in tier0.
		EntityInstanceByNameIter_t iter("cs2sdktest_no_such_*");
		int count = 0;
		for (CEntityInstance* entity = iter.First();
		     entity != nullptr && count < kMaxEntitiesScanned;
		     entity = iter.Next())
		{
			++count;
		}
		context.Detail("-> %d match(es) for a wildcard nothing can satisfy", count);
		if (count != 0)
		{
			context.Fail("a wildcard no entity matches produced %d entities", count);
		}
	});

	if (named_identity_ == nullptr)
	{
		context.SkipCall("CEntityIdentity::NameMatches",
			"no entity on this map carries a targetname");
		context.SkipCall("CEntityIdentity::ClassMatches",
			"no entity on this map carries a targetname");
	}
	else
	{
		CS2ST_CALL(context, "CEntityIdentity::NameMatches", {
			const bool exact = named_identity_->NameMatches(named_entity_name_);
			const bool absent = named_identity_->NameMatches(kAbsentName);
			char scratch[64];
			context.Detail("\"%.63s\" -> %s, \"%s\" -> %s",
				SafeStringArg(named_entity_name_, scratch), exact ? "true" : "false",
				kAbsentName, absent ? "true" : "false");
			if (!exact)
			{
				context.Fail("an entity does not match its own name");
			}
			if (absent)
			{
				context.Fail("an entity matches a name it does not have");
			}
		});

		CS2ST_CALL(context, "CEntityIdentity::ClassMatches", {
			const char* designer = named_identity_->GetClassname();
			const bool exact = named_identity_->ClassMatches(designer);
			const bool absent = named_identity_->ClassMatches(kAbsentName);
			char scratch[64];
			context.Detail("\"%.63s\" -> %s, \"%s\" -> %s",
				SafeStringArg(designer, scratch), exact ? "true" : "false",
				kAbsentName, absent ? "true" : "false");
			if (!exact)
			{
				context.Fail("an entity does not match its own class name");
			}
			if (absent)
			{
				context.Fail("an entity matches a class name it does not have");
			}
		});
	}

	context.SkipCall("EntityInstanceByClassIter_t(CEntityInstance* start, ...)",
		"the start-from-an-entity overload resumes a by-class walk mid-chain; "
		"the from-scratch overload above already covers m_pNextByClass");
	context.SkipCall("EntityInstanceIter_t::SetWorldGroupId",
		"filters the walk to one world group; covered implicitly by the "
		"default group the iterators above run with");
}

// -----------------------------------------------------------------------------
//  CEntityInstance - the vtable a plugin actually holds
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::ExerciseEntityInstanceVirtuals(TestContext& context)
{
	context.Note("[CEntityInstance] virtuals on a live entity");

	if (probe_instance_ == nullptr)
	{
		context.SkipCall("CEntityInstance virtuals", "no live entity was found");
	}
	else
	{
		// Slot 0. Everything else in this vtable is downstream of it being
		// where the SDK says, and the SDK declares ~CEntityInstance in the
		// middle of the class rather than at either end - so a destructor in a
		// different place shifts this whole table.
		CS2ST_CALL(context, "CEntityInstance::GetNetworkSerializerInfo", {
			void* info = probe_instance_->GetNetworkSerializerInfo();
			context.Detail("-> %p", info);
			if (info != nullptr && !LooksLikeObjectPointer(info))
			{
				context.Fail("slot 0 returned something that is not a pointer");
			}
		});

		CS2ST_CALL(context, "CEntityInstance::GetScriptDesc", {
			context.Detail("-> %p",
				static_cast<void*>(probe_instance_->GetScriptDesc()));
		});

		CS2ST_CALL(context, "CEntityInstance::ObjectCaps", {
			context.Detail("-> 0x%08x", static_cast<unsigned>(probe_instance_->ObjectCaps()));
		});

		CS2ST_CALL(context, "CEntityInstance::RequiredEdictIndex", {
			const int required = probe_instance_->RequiredEdictIndex().Get();
			context.Detail("-> %d", required);
			// -1 means "no requirement"; anything else has to be an index into
			// the fixed identity table.
			if (required < -1 || required >= MAX_TOTAL_ENTITIES)
			{
				context.Fail("%d is not an entity index", required);
			}
		});

		CS2ST_CALL(context, "CEntityInstance::FullEdictChanged", {
			context.Detail("-> %s", probe_instance_->FullEdictChanged() ? "true" : "false");
		});

		CS2ST_CALL(context, "CEntityInstance::GetDataDescMap", {
			void* map = probe_instance_->GetDataDescMap();
			context.Detail("-> %p, the class says %p", map,
				static_cast<void*>(probe_class_ != nullptr
					? probe_class_->GetDataDescMap() : nullptr));
			if (map != nullptr && !LooksLikeObjectPointer(map))
			{
				context.Fail("returned something that is not a pointer");
			}
		});

		CS2ST_CALL(context, "CEntityInstance::GetChangeAccessorPathInfo", {
			context.Detail("-> %p",
				static_cast<void*>(probe_instance_->GetChangeAccessorPathInfo()));
		});

		CS2ST_CALL(context, "CEntityInstance::GetChangeAccessorPathInfo (const)", {
			// The const/non-const overload pair (GetChangeAccessorPathInfo_1/_2
			// until the 2026-10 SDK named them) shares one address in a call
			// sweep; both rows exist so the pair's two vtable slots each have
			// their own line to diff.
			context.Detail("-> %p",
				static_cast<void*>(probe_instance_->GetChangeAccessorPathInfo()));
		});

		// An oracle across two subsystems: the schema binding reached through
		// the entity's own virtual has to be the one its CEntityClass holds.
		// Disagreement means the entity's vtable and the class table describe
		// different types, which is exactly what a slot shift looks like.
		CS2ST_CALL(context, "CEntityInstance::Schema_DynamicBinding", {
			CSchemaClassInfo* from_instance = probe_instance_->Schema_DynamicBinding().Get();
			CSchemaClassInfo* from_class =
				probe_class_ != nullptr ? probe_class_->GetSchemaBinding() : nullptr;
			context.Detail("-> %p, the class binding is %p",
				static_cast<void*>(from_instance), static_cast<void*>(from_class));
			if (from_instance != nullptr && !LooksLikeObjectPointer(from_instance))
			{
				context.Fail("returned something that is not a pointer");
			}
			else if (from_class != nullptr && from_instance != from_class)
			{
				context.Fail("the entity and its class name different schema bindings");
			}
		});
	}

	// One line per remaining slot. The vtable is 44 entries long and most of it
	// drives the entity through its lifecycle; a reader has to be able to tell
	// which of those were considered and rejected.
	context.SkipCall("CEntityInstance::unk001", "unidentified slot; calling it means "
		"calling something with unknown arguments and unknown effects");
	context.SkipCall("CEntityInstance::unk002", "unidentified slot");
	context.SkipCall("CEntityInstance::~CEntityInstance", "would destroy a live entity");
	context.SkipCall("CEntityInstance::Connect", "part of entity construction");
	context.SkipCall("CEntityInstance::Disconnect", "part of entity teardown");
	context.SkipCall("CEntityInstance::Precache", "loads resources and needs a live "
		"CEntityPrecacheContext the plugin cannot construct");
	context.SkipCall("CEntityInstance::AddedToEntityDatabase", "a database insertion callback");
	context.SkipCall("CEntityInstance::Spawn", "spawns the entity a second time");
	context.SkipCall("CEntityInstance::unk101", "unidentified slot");
	context.SkipCall("CEntityInstance::PostDataUpdate", "drives the network update path");
	context.SkipCall("CEntityInstance::OnDataUnchangedInPVS", "drives the network update path");
	context.SkipCall("CEntityInstance::Activate", "re-activates a live entity");
	context.SkipCall("CEntityInstance::UpdateOnRemove", "begins removing the entity");
	context.SkipCall("CEntityInstance::OnSetDormant", "changes the entity's dormancy");
	context.SkipCall("CEntityInstance::ScriptEntityIO", "returns a script object whose "
		"ownership and lifetime the SDK does not describe");
	context.SkipCall("CEntityInstance::ScriptAcceptInput", "fires an entity input");
	context.SkipCall("CEntityInstance::PreDataUpdate", "drives the network update path");
	context.SkipCall("CEntityInstance::DrawEntityDebugOverlays", "draws to the world");
	context.SkipCall("CEntityInstance::DrawDebugTextOverlays", "draws to the world");
	context.SkipCall("CEntityInstance::Save", "needs an ISave and writes the entity out");
	context.SkipCall("CEntityInstance::Restore", "needs an IRestore and overwrites the entity");
	context.SkipCall("CEntityInstance::OnSave", "a save-cycle callback");
	context.SkipCall("CEntityInstance::OnRestore", "a restore-cycle callback");
	context.SkipCall("CEntityInstance::unk201", "unidentified slot");
	context.SkipCall("CEntityInstance::NetworkStateChanged", "marks fields dirty for transmission");
	context.SkipCall("CEntityInstance::unk301", "unidentified slot");
	context.SkipCall("CEntityInstance::unk302", "unidentified slot");
	context.SkipCall("CEntityInstance::NetworkUpdateState", "would stop the entity networking");
	context.SkipCall("CEntityInstance::NetworkStateChangedLog", "spews to the console");
	context.SkipCall("CEntityInstance::unk401", "unidentified slot");
	context.SkipCall("CEntityInstance::unk402", "unidentified slot");
	context.SkipCall("CEntityInstance::AddChangeAccessorPath", "allocates a field path id");
	context.SkipCall("CEntityInstance::AssignChangeAccessorPathIds", "renumbers field paths");
	context.SkipCall("CEntityInstance::unk501", "unidentified slot");
	context.SkipCall("CEntityInstance::unk502", "unidentified slot");
	context.SkipCall("CEntityInstance::Pulse_OnDynamicAttributeChanged",
		"a Pulse notification callback");
	context.SkipCall("CEntityInstance::ReloadPrivateScripts", "reloads and re-runs vscript");
	context.SkipCall("CEntityInstance::unk601", "unidentified slot");
	context.SkipCall("CEntityInstance::unk602", "unidentified slot");
	context.SkipCall("CEntityInstance::GetEntityIndex", "an inline forward to "
		"CEntityIdentity::GetEntityIndex, already exercised through the identity");
	context.SkipCall("CEntityInstance::GetClassname", "an inline forward to "
		"CEntityIdentity::GetClassname, already exercised through the identity");
}

// -----------------------------------------------------------------------------
//  Struct layouts
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::InspectEntitySystemLayout(TestContext& context)
{
	context.Note("[CEntitySystem] struct layout - %zu bytes as the SDK declares "
	             "it, the base half of the two passes over this object",
		sizeof(CEntitySystem));

	// IEntityResourceManifestBuilder, which CEntitySystem derives from, declares
	// no data members - there is nothing to lay out. Its vtable is covered as
	// calls instead.
	context.Note("  IEntityResourceManifestBuilder declares no data members; "
	             "nothing to inspect above CEntitySystem");

	// Cast rather than passing entity_system_ straight through: same address
	// today, and the cast is what stays correct if the chain ever grows a base
	// that is not primary.
	StructInspector inspect(context, "CEntitySystem",
		static_cast<const CEntitySystem*>(entity_system_), sizeof(CEntitySystem));

	// m_pCurrentManifest is private, so offsetof cannot name it. Its address is
	// still computed rather than written down: it is the pointer immediately
	// before m_EntityList, which is the first thing offsetof can reach.
	inspect.Pointer("m_pCurrentManifest (private)",
		offsetof(CEntitySystem, m_EntityList) - sizeof(void*));

	// m_EntityList gets its own inspector further down - it is 2.6 KB of
	// concrete data and deserves the space.
	inspect.Raw("m_EntityList (head)", offsetof(CEntitySystem, m_EntityList), 16);

	inspect.String("m_sEntSystemName", offsetof(CEntitySystem, m_sEntSystemName));

	// The four ordered maps and the event queue are opaque here; their counts
	// are read as calls in ExerciseClassLookups, which is the check that
	// actually pins these offsets.
	inspect.Raw("m_entClassesByCPPClassname",
		offsetof(CEntitySystem, m_entClassesByCPPClassname), 24);
	inspect.Raw("m_entClassesByClassname",
		offsetof(CEntitySystem, m_entClassesByClassname), 24);
	inspect.Raw("m_entityComponentHelpers",
		offsetof(CEntitySystem, m_entityComponentHelpers), 24);
	inspect.Raw("m_entityNames", offsetof(CEntitySystem, m_entityNames), 24);

	inspect.Raw("m_EventQueue (head)", offsetof(CEntitySystem, m_EventQueue), 16);
	inspect.Raw("m_entityIONotifiers", offsetof(CEntitySystem, m_entityIONotifiers), 24);

	// Counts. A negative one is impossible by type - these are nesting depths
	// and suppression counters, not values with signs - which makes each of
	// them a probe at a distinct offset through the middle of the struct.
	inspect.Int32("m_nSuppressDormancyChangeCount",
		offsetof(CEntitySystem, m_nSuppressDormancyChangeCount), 0, INT32_MAX);
	inspect.Enum("m_eNetworkSerializationMode",
		offsetof(CEntitySystem, m_eNetworkSerializationMode),
		0, NET_SERIALIZATION_MODE_COUNT - 1);
	inspect.Int32("m_nExecuteQueuedCreationDepth",
		offsetof(CEntitySystem, m_nExecuteQueuedCreationDepth), 0, INT32_MAX);
	inspect.Int32("m_nExecuteQueuedDeletionDepth",
		offsetof(CEntitySystem, m_nExecuteQueuedDeletionDepth), 0, INT32_MAX);
	inspect.Int32("m_nSuppressDestroyImmediateCount",
		offsetof(CEntitySystem, m_nSuppressDestroyImmediateCount), 0, INT32_MAX);
	inspect.Int32("m_nSuppressAutoDeletionExecutionCount",
		offsetof(CEntitySystem, m_nSuppressAutoDeletionExecutionCount), 0, INT32_MAX);
	inspect.Int32("m_nEntityKeyValuesAllocatorRefCount",
		offsetof(CEntitySystem, m_nEntityKeyValuesAllocatorRefCount), 0, INT32_MAX);
	inspect.Float("m_flChangeCallbackSpewThreshold",
		offsetof(CEntitySystem, m_flChangeCallbackSpewThreshold));

	// The bool block, m_Unk1-6 / m_bEnableAutoDeletionExecution until the
	// 2026-10 SDK named them. The sharpest run of probes in the struct: random
	// bytes clear a single bool about 127 times in 128, and seven in a row is a
	// different proposition entirely.
	inspect.Bool("m_bPerformDeletionPostCreation",
		offsetof(CEntitySystem, m_bPerformDeletionPostCreation));
	inspect.Bool("m_bPerformDeallocationPostCreation",
		offsetof(CEntitySystem, m_bPerformDeallocationPostCreation));
	inspect.Bool("m_bInvokePostDataUpdates",
		offsetof(CEntitySystem, m_bInvokePostDataUpdates));
	inspect.Bool("m_bAutoExecuteQueuedDeletion",
		offsetof(CEntitySystem, m_bAutoExecuteQueuedDeletion));
	inspect.Bool("m_bProfilingResourceSystem",
		offsetof(CEntitySystem, m_bProfilingResourceSystem));
	inspect.Bool("m_bInRestoreActivation",
		offsetof(CEntitySystem, m_bInRestoreActivation));
	inspect.Bool("m_unk101", offsetof(CEntitySystem, m_unk101));
	// New in the same SDK update, and the eighth bool in the run.
	inspect.Bool("m_bProcessingQueuedActivatesAndPostDataUpdates",
		offsetof(CEntitySystem, m_bProcessingQueuedActivatesAndPostDataUpdates));

	inspect.Raw("m_queuedCreations", offsetof(CEntitySystem, m_queuedCreations), 24);
	inspect.Raw("m_queuedPostDataUpdates",
		offsetof(CEntitySystem, m_queuedPostDataUpdates), 24);
	inspect.Raw("m_queuedDeletions", offsetof(CEntitySystem, m_queuedDeletions), 24);
	inspect.Raw("m_queuedDeferredDeletions",
		offsetof(CEntitySystem, m_queuedDeferredDeletions), 24);
	inspect.Raw("m_queuedDeallocations",
		offsetof(CEntitySystem, m_queuedDeallocations), 24);
	inspect.Raw("m_queuedDormancyChanges",
		offsetof(CEntitySystem, m_queuedDormancyChanges), 24);

	inspect.Raw("m_EntityPostSpawnCallback",
		offsetof(CEntitySystem, m_EntityPostSpawnCallback), 16);

	inspect.Pointer("m_pNetworkFieldChangedEventQueue",
		offsetof(CEntitySystem, m_pNetworkFieldChangedEventQueue));
	inspect.Pointer("m_pNetworkFieldScratchData",
		offsetof(CEntitySystem, m_pNetworkFieldScratchData));
	inspect.Pointer("m_pFieldChangeLimitSpew",
		offsetof(CEntitySystem, m_pFieldChangeLimitSpew));

	inspect.Raw("m_DataDescKeyUnserializers",
		offsetof(CEntitySystem, m_DataDescKeyUnserializers), 32);
	inspect.Raw("m_ComponentUnserializerInfoAllocator",
		offsetof(CEntitySystem, m_ComponentUnserializerInfoAllocator), 32);
	inspect.Raw("m_EntityKeyValuesAllocator",
		offsetof(CEntitySystem, m_EntityKeyValuesAllocator), 32);
	inspect.Raw("m_Symbols", offsetof(CEntitySystem, m_Symbols), 32);

	inspect.UInt32("m_hActiveSpawnGroup", offsetof(CEntitySystem, m_hActiveSpawnGroup));

	// Twelve floats sitting nearly 8 KB into the struct. A transform is either
	// finite or it is not a transform, so this is twelve independent probes at
	// the far end of the declaration - the region a newly inserted field would
	// have pushed out of place.
	for (int element = 0; element < 12; ++element)
	{
		char field[32];
		std::snprintf(field, sizeof(field), "m_vSpawnOriginOffset[%d][%d]",
			element / 4, element % 4);
		inspect.Float(field,
			offsetof(CEntitySystem, m_vSpawnOriginOffset)
			+ offsetof(matrix3x4_t, m_flMatVal)
			+ static_cast<std::size_t>(element) * sizeof(float));
	}

	// Four of the thirty-two accessor slots individually, so the log shows what
	// they hold; the remaining twenty-eight are checked in bulk below, which is
	// the same evidence without twenty-eight rows of it.
	for (int slot = 0; slot < 4; ++slot)
	{
		char field[32];
		std::snprintf(field, sizeof(field), "m_Accessors[%d]", slot);
		inspect.Pointer(field, offsetof(CEntitySystem, m_Accessors)
			+ static_cast<std::size_t>(slot) * sizeof(void*));
	}

	inspect.Raw("m_EntityMaterialAttributes",
		offsetof(CEntitySystem, m_EntityMaterialAttributes), 32);

	// No HexDump here. This object is really a CGameEntitySystem, so the dump
	// belongs to that pass, where it covers the whole thing instead of a prefix.
	inspect.Summarize();

	CS2ST_CALL(context, "CEntitySystem::m_Accessors (all slots)", {
		int populated = 0;
		int implausible = 0;
		for (int slot = 0; slot < kAccessorSlots; ++slot)
		{
			IEntityDataInstantiator* accessor = entity_system_->m_Accessors[slot];
			if (accessor == nullptr)
			{
				continue;
			}
			++populated;
			if (!LooksLikeObjectPointer(accessor))
			{
				++implausible;
			}
		}
		context.Detail("@0x%zx -> %d of %d slots populated, %d implausible",
			static_cast<std::size_t>(offsetof(CEntitySystem, m_Accessors)),
			populated, kAccessorSlots, implausible);
		if (implausible > 0)
		{
			context.Fail("%d accessor slot(s) hold a value that is not a pointer", implausible);
		}
	});

	// Not an oracle - the accessor is inline, so both sides come from the same
	// declaration - but it is what tells a reader where the SDK believes the
	// entity keyvalues arena lives, which is the number to compare after an
	// update.
	CS2ST_CALL(context, "CEntitySystem::GetEntityKeyValuesAllocator", {
		const void* from_call = entity_system_->GetEntityKeyValuesAllocator();
		const void* from_layout = reinterpret_cast<const unsigned char*>(entity_system_)
			+ offsetof(CEntitySystem, m_EntityKeyValuesAllocator);
		context.Detail("-> %p, offsetof says %p (+0x%zx)", from_call, from_layout,
			static_cast<std::size_t>(offsetof(CEntitySystem, m_EntityKeyValuesAllocator)));
		if (from_call != from_layout)
		{
			context.Fail("the accessor and offsetof disagree, which cannot happen unless "
			             "the two were compiled against different declarations");
		}
	});
}

void GameEntitySystemSuite::InspectGameEntitySystemLayout(TestContext& context)
{
	context.Note("[CGameEntitySystem] struct layout - these fields start at 0x%zx, "
	             "so reading them correctly is what proves the %zu-byte base is intact",
		sizeof(CEntitySystem), sizeof(CEntitySystem));

	StructInspector inspect(context, "CGameEntitySystem", entity_system_,
		sizeof(CGameEntitySystem));

	// Entity counts, bounded by the fixed 64 x 512 identity table rather than
	// by anything about the server's configuration.
	inspect.Int32("m_iNetworkedEntCount",
		offsetof(CGameEntitySystem, m_iNetworkedEntCount), 0, MAX_TOTAL_ENTITIES);
	inspect.Int32("m_iNonNetworkedSavedEntCount",
		offsetof(CGameEntitySystem, m_iNonNetworkedSavedEntCount), 0, MAX_TOTAL_ENTITIES);
	inspect.Raw("m_spawnGroupEntityFilters",
		offsetof(CGameEntitySystem, m_spawnGroupEntityFilters), 32);
	inspect.Raw("m_entityListeners",
		offsetof(CGameEntitySystem, m_entityListeners), 24);
	inspect.Pointer("m_pEntity2SaveRestore",
		offsetof(CGameEntitySystem, m_pEntity2SaveRestore));
	inspect.Pointer("m_pEntity2Networkables",
		offsetof(CGameEntitySystem, m_pEntity2Networkables));

	inspect.Summarize();

	// The most-derived pass owns the dump, so it covers the base half too.
	inspect.HexDump(64, 256);

	CS2ST_CALL(context, "CGameEntitySystem::m_entityListeners (bounds)", {
		// The vector InstallHooks writes into. Checked here so the hooks path
		// has something to refuse on, and checked by type: a count and the
		// buffer it counts cannot contradict each other.
		const int count = entity_system_->m_entityListeners.Count();
		const void* base = entity_system_->m_entityListeners.Base();
		context.Detail("@0x%zx -> %d listener(s), base %p",
			static_cast<std::size_t>(offsetof(CGameEntitySystem, m_entityListeners)),
			count, base);

		if (count < 0)
		{
			context.Fail("a listener count cannot be negative");
			return;
		}
		if (count > 0 && !LooksLikeObjectPointer(base))
		{
			context.Fail("%d listeners and no plausible buffer to hold them", count);
			return;
		}
		for (int i = 0; i < count && i < 16; ++i)
		{
			IEntityListener* listener = entity_system_->m_entityListeners[i];
			if (!LooksLikeObjectPointer(listener))
			{
				context.Fail("listener %d is not a pointer", i);
				return;
			}
		}
	});

	CS2ST_CALL(context, "CGameEntitySystem::m_spawnGroupEntityFilters.Count", {
		context.Detail("@0x%zx -> %u",
			static_cast<std::size_t>(offsetof(CGameEntitySystem, m_spawnGroupEntityFilters)),
			entity_system_->m_spawnGroupEntityFilters.Count());
	});
}

void GameEntitySystemSuite::InspectEntityListLayout(TestContext& context)
{
	context.Note("[CConcreteEntityList] struct layout");

	const CConcreteEntityList* list = &entity_system_->m_EntityList;
	StructInspector inspect(context, "CConcreteEntityList", list, sizeof(CConcreteEntityList));

	// The first four chunk pointers individually - a server with any entities
	// at all has chunk 0 - and all sixty-four in bulk afterwards.
	for (int chunk = 0; chunk < 4; ++chunk)
	{
		char field[32];
		std::snprintf(field, sizeof(field), "m_pIdentityChunks[%d]", chunk);
		inspect.Pointer(field, offsetof(CConcreteEntityList, m_pIdentityChunks)
			+ static_cast<std::size_t>(chunk) * sizeof(void*));
	}

	inspect.Pointer("m_pFirstActiveEntity",
		offsetof(CConcreteEntityList, m_pFirstActiveEntity));

	// Each CList is two pointers and a count. The counts are bounded by the
	// table's own fixed size, which is the SDK's declaration talking rather
	// than the map's contents.
	inspect.Pointer("m_usedList.m_pHead", offsetof(CConcreteEntityList, m_usedList));
	inspect.Pointer("m_usedList.m_pTail",
		offsetof(CConcreteEntityList, m_usedList) + sizeof(void*));
	inspect.Int32("m_usedList.m_nCount",
		offsetof(CConcreteEntityList, m_usedList) + 2 * sizeof(void*),
		0, MAX_TOTAL_ENTITIES);

	inspect.Pointer("m_dormantList.m_pHead", offsetof(CConcreteEntityList, m_dormantList));
	inspect.Pointer("m_dormantList.m_pTail",
		offsetof(CConcreteEntityList, m_dormantList) + sizeof(void*));
	inspect.Int32("m_dormantList.m_nCount",
		offsetof(CConcreteEntityList, m_dormantList) + 2 * sizeof(void*),
		0, MAX_TOTAL_ENTITIES);

	inspect.Pointer("m_freeNetworkableList.m_pHead",
		offsetof(CConcreteEntityList, m_freeNetworkableList));
	inspect.Pointer("m_freeNetworkableList.m_pTail",
		offsetof(CConcreteEntityList, m_freeNetworkableList) + sizeof(void*));
	inspect.Int32("m_freeNetworkableList.m_nCount",
		offsetof(CConcreteEntityList, m_freeNetworkableList) + 2 * sizeof(void*),
		0, MAX_TOTAL_ENTITIES);

	inspect.Pointer("m_freeNonNetworkableList.m_pHead",
		offsetof(CConcreteEntityList, m_freeNonNetworkableList));
	inspect.Pointer("m_freeNonNetworkableList.m_pTail",
		offsetof(CConcreteEntityList, m_freeNonNetworkableList) + sizeof(void*));
	inspect.Int32("m_freeNonNetworkableList.m_nCount",
		offsetof(CConcreteEntityList, m_freeNonNetworkableList) + 2 * sizeof(void*),
		0, MAX_TOTAL_ENTITIES);

	inspect.Int32("m_nNetworkableEntityLimit",
		offsetof(CConcreteEntityList, m_nNetworkableEntityLimit), 0, MAX_TOTAL_ENTITIES);
	inspect.Int32("m_nNonNetworkableEntityLimit",
		offsetof(CConcreteEntityList, m_nNonNetworkableEntityLimit), 0, MAX_TOTAL_ENTITIES);
	inspect.Int32("m_nMaxPlayers",
		offsetof(CConcreteEntityList, m_nMaxPlayers), 0, MAX_TOTAL_ENTITIES);
	inspect.Raw("m_PVSBits (head)", offsetof(CConcreteEntityList, m_PVSBits), 32);

	inspect.Summarize();

	CS2ST_CALL(context, "CConcreteEntityList::m_pIdentityChunks (all chunks)", {
		int populated = 0;
		int implausible = 0;
		for (int chunk = 0; chunk < MAX_ENTITY_LISTS; ++chunk)
		{
			CEntityIdentity* head = list->m_pIdentityChunks[chunk];
			if (head == nullptr)
			{
				continue;
			}
			++populated;
			if (!LooksLikeObjectPointer(head))
			{
				++implausible;
			}
		}
		context.Detail("-> %d of %d chunks allocated, %d implausible",
			populated, MAX_ENTITY_LISTS, implausible);
		if (implausible > 0)
		{
			context.Fail("%d chunk pointer(s) are not pointers", implausible);
		}
	});

	// Four independent count/pointer pairs. Each contradiction is a field
	// inserted between the two, which is the failure a plain range check cannot
	// see. Written as a function-scope macro rather than an array of literals:
	// a braced initializer list inside a CS2ST_CALL body is a macro argument,
	// and its commas split the argument list before the compiler ever sees it.
#define CS2ST_LIST_HEAD_PAIR(member)                                            	CS2ST_CALL(context, "CConcreteEntityList::" #member " count vs head", {      		const int count = list->member.m_nCount;                                		const void* head = list->member.m_pHead;                                		const void* tail = list->member.m_pTail;                                		context.Detail("@0x%zx -> %d entr%s, head %p, tail %p",                 			static_cast<std::size_t>(offsetof(CConcreteEntityList, member)),    			count, count == 1 ? "y" : "ies", head, tail);                       		if ((count > 0) != (head != nullptr))                                   		{                                                                       			context.Fail("the count and the head contradict each other");       		}                                                                       		else if ((head != nullptr) != (tail != nullptr))                        		{                                                                       			context.Fail("the list has one end and not the other");             		}                                                                       	})

	CS2ST_LIST_HEAD_PAIR(m_usedList);
	CS2ST_LIST_HEAD_PAIR(m_dormantList);
	CS2ST_LIST_HEAD_PAIR(m_freeNetworkableList);
	CS2ST_LIST_HEAD_PAIR(m_freeNonNetworkableList);
#undef CS2ST_LIST_HEAD_PAIR
}

void GameEntitySystemSuite::InspectEventQueueLayout(TestContext& context)
{
	context.Note("[CEventQueue] struct layout");

	const CEventQueue* queue = &entity_system_->m_EventQueue;
	StructInspector inspect(context, "CEventQueue", queue, sizeof(CEventQueue));

	inspect.Raw("m_Mutex", offsetof(CEventQueue, m_Mutex), sizeof(CAtomicMutex));

	// m_Events is the *sentinel* node of the pending-event list, not an event.
	// The first version of this said so in a comment and then judged its
	// payload anyway, which produced three FAILs that were all the sentinel
	// behaving exactly as a sentinel should. Every payload field here is
	// therefore reported and not judged - what is under test at these offsets
	// is that the fields are where the SDK puts them, and the values below say
	// that far more convincingly than a range check would have. That includes
	// the middle of the struct this revision gave real names to (the Pulse
	// argument pack and param map, 144 + ~80 opaque bytes) - reported as Raw
	// so a field inserted ahead of them moves a number instead of nothing.
	const std::size_t events = offsetof(CEventQueue, m_Events);

	inspect.Raw("m_Events.m_WorldGroupId (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_WorldGroupId),
		sizeof(WorldGroupId_t));
	// -FLT_MAX is not an implausible game value, it is the marker that makes a
	// time-ordered list work: every real event sorts after the head. Observed
	// exactly that, which pins m_flFireTime to this offset on its own.
	inspect.Raw("m_Events.m_flFireTime (sentinel: -FLT_MAX)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_flFireTime), sizeof(GameTime_t));
	// Never written on the head node, so it holds whatever the allocator left.
	inspect.Raw("m_Events.m_targetType (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_targetType),
		sizeof(EntityIOTargetType_t));
	inspect.Raw("m_Events.m_pTarget (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_pTarget), sizeof(CUtlSymbolLarge));
	inspect.Raw("m_Events.m_pTargetInput (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_pTargetInput),
		sizeof(CUtlSymbolLarge));
	inspect.Raw("m_Events.m_hActivator (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_hActivator),
		sizeof(CEntityHandle));
	inspect.Raw("m_Events.m_hCaller (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_hCaller),
		sizeof(CEntityHandle));
	inspect.Raw("m_Events.m_hEntTarget (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_hEntTarget),
		sizeof(CEntityHandle));
	inspect.Raw("m_Events.m_variantValue (sentinel: unset)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_variantValue),
		sizeof(CVariant));
	inspect.Raw("m_Events.m_PulseArguments (sentinel: uninitialized)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_PulseArguments),
		sizeof(CPulseArgumentPack));
	inspect.Raw("m_Events.m_paramMap (sentinel: uninitialized)",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_paramMap),
		sizeof(CPulseInputParamMap));

	// The links are the part that is real, because they are what the engine
	// splices actual events into. m_pNext is judged; m_pPrev is not, because on
	// a head sentinel with an empty queue nothing has ever written it - and the
	// two fields around it (m_flFireTime holding its marker, m_pNext holding a
	// pointer) are what license that, since misalignment does not repair itself.
	inspect.Pointer("m_Events.m_pNext",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_pNext));
	inspect.Pointer("m_Events.m_pPrev",
		events + offsetof(EntityIOQueuePrioritizedEvent_t, m_pPrev), kMayBeUninitialized);

	inspect.Summarize();
}

void GameEntitySystemSuite::InspectEntityIdentityLayout(TestContext& context)
{
	context.Note("[CEntityIdentity] struct layout - the one every CS2 plugin reads");

	if (probe_identity_ == nullptr)
	{
		context.SkipCall("CEntityIdentity", "no live entity was found to inspect");
		return;
	}

	StructInspector inspect(context, "CEntityIdentity", probe_identity_,
		sizeof(CEntityIdentity));

	inspect.Pointer("m_pInstance", offsetof(CEntityIdentity, m_pInstance));
	inspect.Pointer("m_pClass", offsetof(CEntityIdentity, m_pClass));
	inspect.UInt32("m_EHandle", offsetof(CEntityIdentity, m_EHandle));
	inspect.Int32("m_nameStringTableIndex",
		offsetof(CEntityIdentity, m_nameStringTableIndex));

	// Both are CUtlSymbolLarge, which is a bare const char* into the pool.
	inspect.String("m_name", offsetof(CEntityIdentity, m_name));
	inspect.String("m_designerName", offsetof(CEntityIdentity, m_designerName));

	// Private, so the offsets are derived from the public fields that bracket
	// them rather than written out. m_hPublicScope is the eight bytes before
	// m_flags; m_hSpawnGroup the four before m_worldGroupId; m_pAttributes the
	// eight before m_pPrev.
	inspect.Raw("m_hPublicScope (private)",
		offsetof(CEntityIdentity, m_flags) - sizeof(std::uint64_t),
		sizeof(std::uint64_t));
	inspect.UInt32("m_flags", offsetof(CEntityIdentity, m_flags));
	inspect.UInt32("m_hSpawnGroup (private)",
		offsetof(CEntityIdentity, m_worldGroupId) - sizeof(SpawnGroupHandle_t));
	inspect.UInt32("m_worldGroupId", offsetof(CEntityIdentity, m_worldGroupId));
	inspect.UInt32("m_fDataObjectTypes", offsetof(CEntityIdentity, m_fDataObjectTypes));
	inspect.Int32("m_PathIndex", offsetof(CEntityIdentity, m_PathIndex));
	inspect.Pointer("m_pAttributes (private)",
		offsetof(CEntityIdentity, m_pPrev) - sizeof(void*));

	inspect.Pointer("m_pPrev", offsetof(CEntityIdentity, m_pPrev));
	inspect.Pointer("m_pNext", offsetof(CEntityIdentity, m_pNext));
	inspect.Pointer("m_pPrevByClass", offsetof(CEntityIdentity, m_pPrevByClass));
	inspect.Pointer("m_pNextByClass", offsetof(CEntityIdentity, m_pNextByClass));

	inspect.Summarize();
	inspect.HexDump();

	CS2ST_CALL(context, "CEntityIdentity::m_flags (declared bits)", {
		const std::uint32_t flags = static_cast<std::uint32_t>(probe_identity_->m_flags);
		const std::uint32_t unknown = flags & ~kKnownEntityFlags;
		context.Detail("-> 0x%08x, unknown bits 0x%08x", flags, unknown);
		if (unknown != 0)
		{
			// Suspect rather than Fail: EntityFlags_t names sixteen bits and
			// nothing says Valve stopped there, so an extra bit is a new flag
			// at least as often as it is a misread field.
			context.Suspect("bits outside EntityFlags_t are set");
		}
	});

	CS2ST_CALL(context, "CEntityIdentity::GetSpawnGroup vs the private field", {
		// GetSpawnGroup is the only public route to m_hSpawnGroup, and the
		// field's offset here is derived from its neighbours - so this is the
		// one thing that confirms the derivation.
		const SpawnGroupHandle_t from_call = probe_identity_->GetSpawnGroup();
		SpawnGroupHandle_t from_layout = 0;
		std::memcpy(&from_layout,
			reinterpret_cast<const unsigned char*>(probe_identity_)
				+ offsetof(CEntityIdentity, m_worldGroupId) - sizeof(SpawnGroupHandle_t),
			sizeof(from_layout));
		context.Detail("call %u vs field %u", from_call, from_layout);
		if (from_call != from_layout)
		{
			context.Fail("the getter and the offset derived from its neighbours disagree");
		}
	});
}

void GameEntitySystemSuite::InspectEntityInstanceLayout(TestContext& context)
{
	context.Note("[CEntityInstance] struct layout");

	if (probe_instance_ == nullptr)
	{
		context.SkipCall("CEntityInstance", "no live entity was found to inspect");
		return;
	}

	StructInspector inspect(context, "CEntityInstance", probe_instance_,
		sizeof(CEntityInstance));

	inspect.String("m_iszPrivateVScripts", offsetof(CEntityInstance, m_iszPrivateVScripts));
	inspect.Pointer("m_pEntity", offsetof(CEntityInstance, m_pEntity));
	// Private; the eight bytes between m_pEntity and m_pKeyValues.
	inspect.Pointer("m_hPrivateScope (private)",
		offsetof(CEntityInstance, m_pKeyValues) - sizeof(void*));
	inspect.Pointer("m_pKeyValues", offsetof(CEntityInstance, m_pKeyValues));
	inspect.Pointer("m_CScriptComponent", offsetof(CEntityInstance, m_CScriptComponent));

	inspect.Summarize();
	inspect.HexDump();
}

void GameEntitySystemSuite::InspectEntityClassLayout(TestContext& context)
{
	context.Note("[CEntityClass] struct layout");

	if (probe_class_ == nullptr)
	{
		context.SkipCall("CEntityClass", "no live entity was found to take a class from");
		return;
	}

	StructInspector inspect(context, "CEntityClass", probe_class_, sizeof(CEntityClass));

	inspect.Pointer("m_pScriptDesc", offsetof(CEntityClass, m_pScriptDesc));
	inspect.Pointer("m_NetworkSerializerInfo",
		offsetof(CEntityClass, m_NetworkSerializerInfo));
	inspect.Pointer("m_pInputs", offsetof(CEntityClass, m_pInputs));
	inspect.Pointer("m_pOutputs", offsetof(CEntityClass, m_pOutputs));
	// Input and output counts index the arrays above; negative is impossible.
	inspect.Int32("m_nInputCount", offsetof(CEntityClass, m_nInputCount), 0, INT32_MAX);
	inspect.Int32("m_nOutputCount", offsetof(CEntityClass, m_nOutputCount), 0, INT32_MAX);
	inspect.Pointer("m_pSharedPulseSignature",
		offsetof(CEntityClass, m_pSharedPulseSignature));
	inspect.Pointer("m_pfnRegisterPulseBindings",
		offsetof(CEntityClass, m_pfnRegisterPulseBindings));
	inspect.Pointer("m_NameToThinkFunc", offsetof(CEntityClass, m_NameToThinkFunc));
	inspect.Pointer("m_ThinkFuncToName", offsetof(CEntityClass, m_ThinkFuncToName));
	inspect.Pointer("m_pfnEnumerateComponents",
		offsetof(CEntityClass, m_pfnEnumerateComponents));
	inspect.Pointer("m_pComponentOverrides", offsetof(CEntityClass, m_pComponentOverrides));
	inspect.Pointer("m_pClassInfo", offsetof(CEntityClass, m_pClassInfo));
	inspect.Pointer("m_pBaseClassInfo", offsetof(CEntityClass, m_pBaseClassInfo));
	inspect.String("m_designerName", offsetof(CEntityClass, m_designerName));
	inspect.UInt32("m_flags", offsetof(CEntityClass, m_flags));
	inspect.Int32("m_SpawnOrder", offsetof(CEntityClass, m_SpawnOrder));
	inspect.UInt32("m_nAllHelpersFlags", offsetof(CEntityClass, m_nAllHelpersFlags));
	inspect.Raw("m_ComponentOffsets", offsetof(CEntityClass, m_ComponentOffsets), 24);
	inspect.Raw("m_AllHelpers", offsetof(CEntityClass, m_AllHelpers), 24);
	inspect.Raw("m_componentUnserializerClassInfo",
		offsetof(CEntityClass, m_componentUnserializerClassInfo),
		sizeof(ComponentUnserializerClassInfo_t));
	inspect.Raw("m_flattenedSerializer",
		offsetof(CEntityClass, m_flattenedSerializer), 16);
	inspect.Raw("m_classOutputInfos", offsetof(CEntityClass, m_classOutputInfos), 24);
	inspect.UInt32("m_requiredEHandle", offsetof(CEntityClass, m_requiredEHandle));
	inspect.Pointer("m_pNext", offsetof(CEntityClass, m_pNext));
	inspect.Pointer("m_pFirstEntity", offsetof(CEntityClass, m_pFirstEntity));
	inspect.Pointer("m_pServerClass", offsetof(CEntityClass, m_pServerClass));
	inspect.Int32("m_nClassIndex", offsetof(CEntityClass, m_nClassIndex), 0, INT32_MAX);

	inspect.Summarize();
	inspect.HexDump();

	CS2ST_CALL(context, "CEntityClass::m_nInputCount vs m_pInputs", {
		// A count and the array it counts. This catches a field inserted
		// between the two, which neither one alone can see.
		const int inputs = probe_class_->m_nInputCount;
		const int outputs = probe_class_->m_nOutputCount;
		context.Detail("%d input(s) at %p, %d output(s) at %p",
			inputs, static_cast<void*>(probe_class_->m_pInputs),
			outputs, static_cast<void*>(probe_class_->m_pOutputs));
		if ((inputs > 0) != (probe_class_->m_pInputs != nullptr))
		{
			context.Fail("the input count and the input array contradict each other");
		}
		if ((outputs > 0) != (probe_class_->m_pOutputs != nullptr))
		{
			context.Fail("the output count and the output array contradict each other");
		}
	});

	CS2ST_CALL(context, "CEntityClass::m_pFirstEntity", {
		// The head of the by-class chain the class iterator walks. If the class
		// has any entity at all, this one's class pointer has to point back.
		CEntityIdentity* first = probe_class_->m_pFirstEntity;
		context.Detail("-> %p", static_cast<void*>(first));
		if (first == nullptr)
		{
			return;
		}
		if (!LooksLikeObjectPointer(first))
		{
			context.Fail("the by-class chain head is not a pointer");
			return;
		}
		if (first->m_pClass != probe_class_)
		{
			context.Fail("the first entity of this class belongs to a different class");
		}
	});
}

void GameEntitySystemSuite::InspectEntityClassInfoLayout(TestContext& context)
{
	context.Note("[CEntityClassInfo] struct layout");

	CEntityClassInfo* info = (probe_class_ != nullptr) ? probe_class_->m_pClassInfo : nullptr;
	if (!LooksLikeObjectPointer(info))
	{
		context.SkipCall("CEntityClassInfo", "the probe class exposed no class info");
		return;
	}

	// Two strings, a flags word, four pointers: the densest run of
	// type-checkable fields anywhere in this suite. m_nFlags replaced two
	// pointer placeholders upstream (m_pszDescription, m_pPredDescMap) - the
	// struct shrank by two slots, which is exactly what a layout probe is
	// here to notice.
	StructInspector inspect(context, "CEntityClassInfo", info, sizeof(CEntityClassInfo));

	inspect.String("m_pszClassname", offsetof(CEntityClassInfo, m_pszClassname));
	inspect.String("m_pszCPPClassname", offsetof(CEntityClassInfo, m_pszCPPClassname));
	inspect.UInt32("m_nFlags", offsetof(CEntityClassInfo, m_nFlags));
	inspect.Pointer("m_pClass", offsetof(CEntityClassInfo, m_pClass));
	inspect.Pointer("m_pBaseClassInfo", offsetof(CEntityClassInfo, m_pBaseClassInfo));
	inspect.Pointer("m_pSchemaBinding", offsetof(CEntityClassInfo, m_pSchemaBinding));
	inspect.Pointer("m_pDataDescMap", offsetof(CEntityClassInfo, m_pDataDescMap));

	inspect.Summarize();
	inspect.HexDump();

	CS2ST_CALL(context, "CEntityClassInfo::m_pClass (round trip)", {
		// class -> class info -> class has to come back to where it started.
		context.Detail("class %p -> info %p -> class %p",
			static_cast<void*>(probe_class_), static_cast<void*>(info),
			static_cast<void*>(info->m_pClass));
		if (info->m_pClass != probe_class_)
		{
			context.Fail("the class info does not point back at the class that owns it");
		}
	});
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void GameEntitySystemSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[CEntitySystem / CGameEntitySystem] not called - would change the game");

	context.SkipCall("CEntitySystem::~CEntitySystem", "would destroy the entity system");
	context.SkipCall("CEntitySystem::ClearEntityDatabase", "deletes every entity on the server");
	context.SkipCall("CEntitySystem::OnEntityParentChanged", "reparents a live entity");
	context.SkipCall("CEntitySystem::OnAddEntity", "registers an entity that was never created");
	context.SkipCall("CEntitySystem::OnRemoveEntity", "unregisters a live entity");
	context.SkipCall("CEntitySystem::SortEntities", "reorders the spawn list mid-frame");
	context.SkipCall("CEntitySystem::Spawn", "spawns entities from a caller-built array");
	context.SkipCall("CEntitySystem::Activate", "activates entities out of the engine's order");
	context.SkipCall("CEntitySystem::PostDataUpdate", "drives the network update path");
	context.SkipCall("CEntitySystem::OnSetDormant", "changes entity dormancy and PVS membership");
	context.SkipCall("CEntitySystem::UpdateOnRemove", "begins deleting entities");

	context.SkipCall("CEntitySystem::AllocPooledString",
		"adds a string to the entity symbol table permanently; FindPooledString is "
		"the read-only half and is exercised above");
	context.SkipCall("CEntitySystem::AddEntityKeyValuesAllocatorRef",
		"increments a refcount nothing in this plugin will ever release");
	context.SkipCall("CEntitySystem::ReleaseEntityKeyValuesAllocatorRef",
		"drops a reference this plugin never took, and clears the keyvalues arena "
		"when the count reaches zero");

	context.SkipCall("CGameEntitySystem::~CGameEntitySystem", "would destroy the entity system");
	context.SkipCall("CGameEntitySystem::AddListenerEntity",
		"writes into the game's own listener vector; exercised from InstallHooks "
		"instead, and only after a call sweep has validated the layout");
	context.SkipCall("CGameEntitySystem::RemoveListenerEntity",
		"the inverse of the above; exercised from RemoveHooks");
}

// -----------------------------------------------------------------------------
//  Hooks - IEntityListener
// -----------------------------------------------------------------------------

int GameEntitySystemSuite::InstallHooks(TestContext& context)
{
	(void)context;

	if (listener_registered_)
	{
		return 0;
	}

	// The one place this plugin writes into the game. Everything else it does
	// is a read, and AddListenerEntity appends to a CUtlVector at an offset
	// 8400 bytes into an object whose layout is exactly what is under test - so
	// it only runs once a call sweep has said that layout holds.
	if (entity_system_ == nullptr || !identity_layout_verified_)
	{
		CS2ST_LOG_WARN("%s: refusing to install - run 'meta_test run %s' first so the "
		               "entity system is located and its layout checked; "
		               "AddListenerEntity writes into the game's own memory\n",
			Id(), Id());
		return 0;
	}

	const int existing = entity_system_->m_entityListeners.Count();
	if (existing < 0 || existing > 64)
	{
		CS2ST_LOG_WARN("%s: refusing to install - m_entityListeners reports %d entries, "
		               "which is not a listener count; the offset has moved\n",
			Id(), existing);
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before registering, so a callback that never fires still appears
	// in the dump as `never`. After a CS2 update that row is the finding.
	recorder.Declare(kOnEntityCreated, Id());
	recorder.Declare(kOnEntitySpawned, Id());
	recorder.Declare(kOnEntityDeleted, Id());
	recorder.Declare(kOnEntityParentChanged, Id());

	entity_system_->AddListenerEntity(this);

	const int after = entity_system_->m_entityListeners.Count();
	if (after != existing + 1
		|| entity_system_->m_entityListeners[after - 1] != static_cast<IEntityListener*>(this))
	{
		// The write already happened; all that is left is to say so loudly and
		// try to undo it.
		CS2ST_LOG_ERROR("%s: AddListenerEntity did not land where the SDK says it should "
		                "(%d listeners before, %d after) - removing again\n",
			Id(), existing, after);
		entity_system_->RemoveListenerEntity(this);
		return 0;
	}

	listener_registered_ = true;
	CS2ST_LOG_INFO("%s: listening for entity events (%d listener(s) registered in total)\n",
		Id(), after);
	CS2ST_LOG_INFO("%s: the entity system is replaced when the level changes, which takes "
	               "this listener with it - re-install after a map change\n", Id());

	return kDeclaredHookCount;
}

void GameEntitySystemSuite::RemoveHooks(TestContext& context)
{
	(void)context;

	if (!listener_registered_)
	{
		return;
	}

	if (entity_system_ != nullptr)
	{
		entity_system_->RemoveListenerEntity(this);
	}

	listener_registered_ = false;
}

void GameEntitySystemSuite::RecordEntityEvent(const char* hook_id, CEntityInstance* entity,
                                              CEntityInstance* other)
{
	// No fault guard here. This runs in the engine's own frame, long after the
	// call sweep's FaultGuardScope has gone, so a fault takes the server down
	// rather than producing a CRASHED row. Vet everything, walk nothing that
	// has not already been proven, and stay cheap.
	if (!LooksLikeObjectPointer(entity))
	{
		CS2ST_HOOK_ENTER_ARGS(hook_id, "entity=%p NOT-A-POINTER",
			static_cast<void*>(entity));
		CS2ST_HOOK_LEAVE(hook_id);
		return;
	}

	char scratch[64];
	const char* designer = "<layout unverified>";

	// One hop past the handed pointer, and only because ExerciseIdentityLookups
	// proved that hop lands where the SDK says. Without a class name the event
	// says an entity happened, which is most of the value gone - but not enough
	// value to justify guessing.
	if (identity_layout_verified_)
	{
		CEntityIdentity* identity = entity->m_pEntity;
		designer = LooksLikeObjectPointer(identity)
			? SafeStringArg(identity->GetClassname(), scratch)
			: "<no identity>";
	}

	if (other != nullptr)
	{
		CS2ST_HOOK_ENTER_ARGS(hook_id, "entity=%p class=\"%.63s\" other=%p",
			static_cast<void*>(entity), designer, static_cast<void*>(other));
	}
	else
	{
		CS2ST_HOOK_ENTER_ARGS(hook_id, "entity=%p class=\"%.63s\"",
			static_cast<void*>(entity), designer);
	}

	// Entry and return are both recorded even though nothing can supersede us
	// here: it keeps the dump's balance column meaning the same thing in every
	// suite. What it proves here is only that our own handler ran to the end.
	CS2ST_HOOK_LEAVE(hook_id);
}

void GameEntitySystemSuite::OnEntityCreated(CEntityInstance* entity)
{
	RecordEntityEvent(kOnEntityCreated, entity, nullptr);
}

void GameEntitySystemSuite::OnEntitySpawned(CEntityInstance* entity)
{
	RecordEntityEvent(kOnEntitySpawned, entity, nullptr);
}

void GameEntitySystemSuite::OnEntityDeleted(CEntityInstance* entity)
{
	RecordEntityEvent(kOnEntityDeleted, entity, nullptr);
}

void GameEntitySystemSuite::OnEntityParentChanged(CEntityInstance* entity,
                                                  CEntityInstance* new_parent)
{
	RecordEntityEvent(kOnEntityParentChanged, entity, new_parent);
}

} // namespace cs2st
