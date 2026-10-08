/**
 * Exercises the entity system - the object every CS2 plugin reaches an entity
 * through, and the only P0 interface the SDK cannot hand you a pointer to.
 *
 * Three things make this suite different from the others.
 *
 * 1. There is no interface to query for.
 *
 *    "GameResourceServiceServerV001" @ engine2 resolves to IGameResourceService,
 *    and s2sdk declares exactly two things about that type: a forward
 *    declaration in interfaces.h and a version string. No class body, no
 *    methods, no way to ask it for the entity system. Every plugin in the
 *    ecosystem instead reads a raw pointer out of the service object at a
 *    hard-coded byte offset - 0x58 on Windows, 0x50 on Linux - and hopes.
 *
 *    That offset is the single most fragile assumption in the CS2 plugin
 *    ecosystem, so this suite does not hard-code it and move on: it reads the
 *    slot the ecosystem assumes, validates the candidate, and if that fails
 *    sweeps the neighbouring slots and reports which one actually holds the
 *    entity system. A log line saying "found at +0x60, not the assumed +0x50"
 *    is worth more than the rest of the suite put together, because every
 *    other plugin will be reading the wrong eight bytes that day.
 *
 *    Validation is deliberately not "the pointer is non-null". A candidate has
 *    to have a vtable pointer landing inside a module the game has mapped, an
 *    entity count inside the fixed 64 x 512 identity table, and a used-list
 *    whose count and head pointer do not contradict each other.
 *
 * 2. Most of what is worth testing is not virtual.
 *
 *    CEntitySystem's vtable is short and almost entirely side-effecting. The
 *    valuable surface is the non-virtual helpers - GetEntityIdentity,
 *    FindClassByName, the three EntityInstance*Iter_t classes - which s2sdk
 *    implements itself, in entity2/entitysystem.cpp, out of nothing but struct
 *    offsets. That code is compiled into this plugin. Calling it *is* testing
 *    the layout: if Valve moves m_EntityList or grows CEntityIdentity, those
 *    functions do not crash, they quietly return the wrong entity.
 *
 *    Which is why the strongest checks here are round trips. Walking
 *    m_pFirstActiveEntity down its m_pNext chain and indexing
 *    m_pIdentityChunks[i / 512][i % 512] are two descriptions of the same
 *    entity that share no code; agreement confirms the chunk array, the entity
 *    index packing in CEntityHandle, and sizeof(CEntityIdentity) at once.
 *
 *    Every oracle in the suite:
 *
 *      linked-list walk             vs  GetEntityIdentity(CEntityIndex)
 *      GetEntityIdentity(index)     vs  GetEntityIdentity(CEntityHandle)
 *      identity->m_pInstance        vs  GetEntityInstance(identity)
 *      instance->m_pEntity          vs  the identity it came from
 *      identity->GetClassname()     vs  identity->m_pClass' designer name
 *      identity->m_pClass           vs  FindClassByDesignName(that name)
 *      identity->m_pClass           vs  FindClassByName(its C++ name)
 *      m_pClass->GetSchemaBinding() vs  instance->Schema_DynamicBinding()
 *      by-class chain               vs  the global chain filtered by name
 *      FindFirstEntityHandleByName  vs  the named entity it was taken from
 *      FindPooledString(name)       vs  the identity's own m_name symbol
 *      GetEntityKeyValuesAllocator  vs  offsetof(m_EntityKeyValuesAllocator)
 *      CGameEntitySystem's fields   vs  sizeof(CEntitySystem) being right
 *
 * 3. The vtable's first callable slot is a long way in.
 *
 *    CEntitySystem derives from IEntityResourceManifestBuilder, whose ten pure
 *    virtuals occupy slots 0-9, and all ten build or mutate resource manifests
 *    - none can be called on a live server. Then the SDK declares
 *    `virtual ~CEntitySystem() = 0` *first* among CEntitySystem's own members,
 *    which under the Itanium ABI takes slots 10 and 11. So GetCurTime, the
 *    first thing this suite can safely call, is slot 12.
 *
 *    If Valve's real class declares its destructor anywhere else, every slot
 *    from 10 on is shifted and GetCurTime is not GetCurTime. There is no
 *    cheaper canary available: it is the first callable slot there is.
 *
 * Hooks: IEntityListener, registered with CGameEntitySystem::AddListenerEntity.
 * This is the one path by which the engine calls into a plugin through this
 * interface, and unlike ISmmAPI::AddListener it has a proper inverse.
 *
 * Two safety rules apply to it, both unusual for this project:
 *
 *   - Installing *writes* into the game's own CUtlVector, at an offset the SDK
 *     only believes in. Every other probe in this plugin is read-only. So the
 *     listener is only installed after a call sweep has run and validated the
 *     layout it depends on; InstallHooks refuses otherwise and says so.
 *   - The entity system is recreated when the level changes, and a listener
 *     registered on the old one goes with it. After a map change the hooks go
 *     quiet, which looks exactly like "the engine stopped calling". Re-run
 *     `meta_test hooks install entity` after changing maps.
 */

#ifndef CS2SDKTEST_SUITES_GAMEENTITYSYSTEMSUITE_H_
#define CS2SDKTEST_SUITES_GAMEENTITYSYSTEMSUITE_H_

#include <cstddef>

// The SDK header has to be here rather than forward-declared: this suite *is*
// an IEntityListener, so the base class must be complete where the class is
// declared. It also drags in CGameEntitySystem, which is the type of a member.
#include <entity2/entitysystem.h>

#include "testkit/ITestSuite.h"

class CEntityClass;
class CEntityIdentity;
class CEntityInstance;

namespace cs2st
{

class GameEntitySystemSuite : public ITestSuite, public IEntityListener
{
public:
	const char* Id() const override { return "entity"; }
	const char* DisplayName() const override { return "CGameEntitySystem / CEntitySystem"; }
	const char* Description() const override
	{
		return "The entity system, reached through IGameResourceService at an assumed offset";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

	int InstallHooks(TestContext& context) override;
	void RemoveHooks(TestContext& context) override;
	bool HooksInstalled() const override { return listener_registered_; }

	// ---- IEntityListener ---------------------------------------------------
	//
	// The engine walks m_entityListeners for real work, so these must behave
	// exactly like the empty defaults they override: record and return.

	void OnEntityCreated(CEntityInstance* entity) override;
	void OnEntitySpawned(CEntityInstance* entity) override;
	void OnEntityDeleted(CEntityInstance* entity) override;
	void OnEntityParentChanged(CEntityInstance* entity, CEntityInstance* new_parent) override;

private:
	// ---- locating the entity system ----------------------------------------

	void ResolveEntitySystem(TestContext& context);

	/**
	 * Does `candidate` behave like a CGameEntitySystem?
	 *
	 * Reads through the pointer, so only call it from inside a guarded call.
	 * `why` receives the first thing that ruled the candidate out, for the log.
	 */
	bool LooksLikeEntitySystem(const void* candidate, const char** why) const;

	// ---- calls -------------------------------------------------------------

	void ExerciseResourceServiceAppSystem(TestContext& context);
	void ExerciseEntitySystemVirtuals(TestContext& context);
	void CollectProbeEntities(TestContext& context);
	void ExerciseIdentityLookups(TestContext& context);
	void ExerciseClassLookups(TestContext& context);
	void ExerciseNameLookups(TestContext& context);
	void ExerciseIterators(TestContext& context);
	void ExerciseEntityInstanceVirtuals(TestContext& context);

	// ---- layout ------------------------------------------------------------

	void InspectEntitySystemLayout(TestContext& context);
	void InspectGameEntitySystemLayout(TestContext& context);
	void InspectEntityListLayout(TestContext& context);
	void InspectEventQueueLayout(TestContext& context);
	void InspectEntityIdentityLayout(TestContext& context);
	void InspectEntityInstanceLayout(TestContext& context);
	void InspectEntityClassLayout(TestContext& context);
	void InspectEntityClassInfoLayout(TestContext& context);

	void NoteSideEffectingCalls(TestContext& context);

	/** Shared body of the four listener callbacks. */
	void RecordEntityEvent(const char* hook_id, CEntityInstance* entity,
	                       CEntityInstance* other);

	// ---- state -------------------------------------------------------------

	/**
	 * "GameResourceServiceServerV001". Held as void* because that is all the
	 * SDK gives us - IGameResourceService is a forward declaration with no
	 * body anywhere in s2sdk.
	 */
	void* resource_service_ = nullptr;

	CGameEntitySystem* entity_system_ = nullptr;

	/** Byte offset the entity system was actually found at, or SIZE_MAX. */
	std::size_t entity_system_offset_ = static_cast<std::size_t>(-1);

	/**
	 * The identity round trips agreed, so reading one hop past a pointer the
	 * engine hands a hook is justified rather than reckless. Set by
	 * ExerciseIdentityLookups; consulted by the listener callbacks and by
	 * InstallHooks.
	 */
	bool identity_layout_verified_ = false;

	/** A live entity harvested from the active list, and what came with it. */
	CEntityIdentity* probe_identity_ = nullptr;
	CEntityInstance* probe_instance_ = nullptr;
	CEntityClass* probe_class_ = nullptr;
	const char* probe_designer_name_ = nullptr;
	const char* probe_cpp_name_ = nullptr;
	int probe_entity_index_ = -1;

	/** An entity that carries a targetname, for the by-name lookups. */
	CEntityIdentity* named_identity_ = nullptr;
	const char* named_entity_name_ = nullptr;

	/** How many identities the linked-list walk reached (capped). */
	int active_entity_count_ = 0;

	/** AddListenerEntity has been called and not yet undone. */
	bool listener_registered_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_GAMEENTITYSYSTEMSUITE_H_ */
