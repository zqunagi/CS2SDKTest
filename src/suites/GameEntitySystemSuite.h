#ifndef CS2SDKTEST_SUITES_GAMEENTITYSYSTEMSUITE_H_
#define CS2SDKTEST_SUITES_GAMEENTITYSYSTEMSUITE_H_

#include <cstddef>

#include <gameentitysystem.h>

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
	 * "GameResourceServiceServerV001". Held as void* because the suite treats
	 * it as raw bytes for its slot scan; the typed view the SDK declares is
	 * CGameResourceService in engine2/igameresourceservice.h.
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
