/**
 * Exercises the gamedata every CS2 plugin resolves out of band.
 *
 * CounterStrikeSharp and its ecosystem do not find functions through exports
 * or interfaces alone: a config file beside the plugin supplies byte
 * signatures for unexported functions, vtable slots for methods the SDK does
 * not declare, and byte offsets into structs nobody publishes. That file is
 * the most fragile dependency a plugin has - an engine update moves a byte
 * pattern and the plugin fails to load, or worse, a slot shifts and the
 * plugin calls the wrong function with the right-looking arguments.
 *
 * This suite loads configs/gamedata.toml (the 46 keys the ecosystem's
 * gamedata carries, in a format a human can read and edit), resolves every
 * entry against the live server, and then does the part no gamedata loader
 * does: checks that what a key resolved to behaves like the thing it names.
 *
 * Three kinds of entry, three kinds of check:
 *
 *   signatures   scan server for the byte pattern. A hit inside the module is
 *                necessary but cheap; the strong check is behavioural, on the
 *                subset where a safe call exists (below). Ambiguous patterns
 *                (two hits) are reported, never guessed at.
 *   vtables      every key whose slot an SDK declaration can also compute -
 *                KHook::GetVtableIndex(&Class::Method) - is checked against
 *                the SDK's answer. Two independent sources, one number.
 *   offsets      likewise compared against offsetof() where the SDK declares
 *                the struct: offsetof(CCheckTransmitInfo, m_nPlayerSlot) for
 *                CheckTransmitPlayerSlot, and the service-object slot the
 *                SDK's own GameEntitySystem() accessor walks.
 *
 * The behavioural checks, on signatures a call sweep can reach:
 *
 *   UTIL_CreateEntityByName + UTIL_Remove + CBaseEntity_DispatchSpawn
 *                a full round trip - create an info_target by name (returning
 *                an entity the suite did not have before), spawn it, then
 *                remove it. Each step validates the step before: the spawn
 *                proves the pointer is an entity, the removal proves the
 *                whole chain against the entity list afterwards.
 *   GetCSWeaponDataFromKey
 *                the weapon table lookup every GiveNamedItem goes through:
 *                "weapon_ak47" must resolve, "cs2sdktest_no_such_weapon"
 *                must not.
 *   CCSNavArea_IsValidNavMesh
 *                a boolean fact with a knowable answer on a loaded map.
 *   the scan-only signatures (ExerciseSignatureBehaviours)
 *                the rest of the file's keys, called for real. The ones
 *                that need no game state run directly: a zero-connection
 *                CEntityIOOutput must fire nothing and come back
 *                unchanged, the game-event listener table is a pure
 *                function with a computable answer, the game-system list
 *                walks its own links. The player-shaped ones run against
 *                a bot this suite adds through ServerCommand("bot_add")
 *                and kicks afterwards - one run ahead of the exercise,
 *                because a queued command lands on the next frame and the
 *                sweep is synchronous. Every raw offset those calls read
 *                (the pawn's team byte, the controller's pawn handle, the
 *                item-services pointer) is resolved through the schema
 *                system first, so the binary and the schema must agree
 *                before anything is called - two independent sources, one
 *                number, the same discipline as the vtable rows.
 *
 * What still only resolves-and-reports: CGameEventManager_Init and
 * NetworkStateChanged (verified by decoding their bytes, in
 * ExerciseCodeDecodes), ClientPrint on Windows (its Linux pattern anchors
 * a lea the decode reads; the Windows pattern anchors the prologue), and
 * CBaseTrigger_StartTouch, whose pattern matches nothing on the builds
 * checked. Finding the bytes is the part that breaks on update day, and a
 * row that says "pattern no longer matches anything" is the headline that
 * row exists for.
 *
 * Hooks: Host_Say and CEntityInstance_AcceptInput, installed only after the
 * sweep has validated the addresses. They are the two functions a plugin most
 * often detours out of this file, and both observe only.
 *
 * GameEventManager (a slot on ISource2Server): the ecosystem's route to
 * IGameEventManager2 is to call that slot and subtract 8 from the result.
 * GameEventSuite documents why this project will not blind-call server vtable
 * slots, so the slot's code is decoded first and only an accessor-shaped body
 * (lea/mov rax,[rip+d]; ret) is called - an empty 'ret' stub, which is what
 * slot 93 is on Linux build 25815307, fails the row instead.
 */

#ifndef CS2SDKTEST_SUITES_GAMEDATASUITE_H_
#define CS2SDKTEST_SUITES_GAMEDATASUITE_H_

#include <cstddef>
#include <string>

#include <khook.hpp>

#include "core/TomlData.h"
#include "testkit/ITestSuite.h"

class CCommand;
class CEntityInstance;
class CSchemaSystemTypeScope;
class ISchemaSystem;
class IVEngineServer2;

namespace cs2st
{

class GamedataSuite : public ITestSuite
{
public:
	/** Binds the hook callbacks; the addresses are attached in InstallHooks. */
	GamedataSuite();

	const char* Id() const override { return "gamedata"; }
	const char* DisplayName() const override { return "gamedata / signatures / vtable slots"; }
	const char* Description() const override
	{
		return "The out-of-band data plugins resolve: signature scans, slot indexes, offsets";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

	int InstallHooks(TestContext& context) override;
	void RemoveHooks(TestContext& context) override;
	bool HooksInstalled() const override { return hooks_installed_; }

private:
	/** One signature key resolved against the server module. */
	bool ExerciseSignature(TestContext& context, const std::string& table);

	/** Behavioural checks on the signatures that can be called safely. */
	void ExerciseRoundTrip(TestContext& context);
	void ExerciseWeaponData(TestContext& context);
	void ExerciseNavMesh(TestContext& context);

	/** Signatures verified by decoding their bytes where calling is unsafe. */
	void ExerciseCodeDecodes(TestContext& context);

	/**
	 * The scan-only signatures, called: standalone ones first (a local
	 * object or a global is the whole setup), then the ones that need a
	 * player, against a bot this suite adds and kicks.
	 */
	void ExerciseSignatureBehaviours(TestContext& context);
	void ExerciseStandaloneBehaviours(TestContext& context);
	void ExerciseBotBehaviours(TestContext& context);

	/**
	 * (class name, field name) -> byte offset, off the server module's
	 * schema type scope. False when either name does not resolve; the
	 * behavioural checks then skip rather than guess an offset.
	 */
	bool SchemaOffset(const char* class_name, const char* field_name,
	                  std::size_t& offset);
	/** SchemaOffset over a fallback list; the field's declaring class wins. */
	bool SchemaOffsetAny(const char* const* class_names, const char* field_name,
	                     std::size_t& offset);

	/**
	 * Finds the bot's controller/pawn pair by walking the entity list for
	 * CCSPlayerController entities named cs_bot, then the pawn handle off
	 * the controller. False when no bot is in the server.
	 */
	bool FindBotPair(CEntityInstance*& controller, CEntityInstance*& pawn);

	/**
	 * Walks the active entity list matching the C++ class name off each
	 * identity's class (EntityInstanceByClassIter_t matches designer names,
	 * which is a different axis). Null when nothing in the list is one.
	 */
	CEntityInstance* FindEntityByCppClassname(const char* cpp_class_name);

	/** vtable slots cross-checked against SDK-computed indexes. */
	void ExerciseVtableSlots(TestContext& context);

	/** byte offsets cross-checked against offsetof() where the SDK declares. */
	void ExerciseStructOffsets(TestContext& context);

	// ---- hook callbacks (observation only) ------------------------------

	KHook::Return<void> Pre_HostSay(class CEntityInstance* controller, class CCommand& args,
	                                bool teamonly, int unk1, const char* unk2);
	KHook::Return<void> Post_HostSay(class CEntityInstance* controller, class CCommand& args,
	                                 bool teamonly, int unk1, const char* unk2);

	KHook::Return<void> Pre_AcceptInput(CEntityInstance* self, const char* input,
	                                    CEntityInstance* activator, CEntityInstance* caller,
	                                    void* value, int output_id, void* unk);
	KHook::Return<void> Post_AcceptInput(CEntityInstance* self, const char* input,
	                                     CEntityInstance* activator, CEntityInstance* caller,
	                                     void* value, int output_id, void* unk);

	// ---- state -----------------------------------------------------------

	TomlData data_;
	bool data_loaded_ = false;

	/** The service object offsets.GameEntitySystem is read from, when present. */
	void* resource_service_ = nullptr;

	/** ISource2Server, whose vtables.GameEventManager slot is decoded. */
	void* server_ = nullptr;

	/** ServerCommand - the bot_add / bot_kick route. Null is not fatal. */
	IVEngineServer2* engine_ = nullptr;

	/** The schema system and the server module's type scope, for offsets. */
	ISchemaSystem* schema_ = nullptr;
	CSchemaSystemTypeScope* server_scope_ = nullptr;

	/**
	 * Resolved addresses the hooks and behavioural calls need, filled by the
	 * sweep. Null until a run has matched the corresponding pattern.
	 */
	void* host_say_ = nullptr;
	void* accept_input_ = nullptr;
	void* create_entity_ = nullptr;
	void* dispatch_spawn_ = nullptr;
	void* util_remove_ = nullptr;
	void* weapon_data_ = nullptr;
	void* navmesh_ = nullptr;
	void* event_manager_init_ = nullptr;
	void* network_state_changed_ = nullptr;

	/** The scan-only signatures the behavioural layer calls. */
	void* client_print_all_ = nullptr;
	void* client_print_ = nullptr;
	void* switch_team_ = nullptr;
	void* set_pawn_ = nullptr;
	void* post_think_ = nullptr;
	void* give_named_item_ = nullptr;
	void* set_model_ = nullptr;
	void* can_use_ = nullptr;
	void* can_acquire_ = nullptr;
	void* terminate_round_ = nullptr;
	void* emit_sound_filter_ = nullptr;
	void* add_entity_io_event_ = nullptr;
	void* legacy_listener_ = nullptr;
	void* remove_player_item_ = nullptr;
	void* take_damage_old_ = nullptr;
	void* end_touch_ = nullptr;
	void* fire_output_internal_ = nullptr;
	void* gamesystem_pfirst_ = nullptr;

	/** The navmesh global the IsValidNavMesh stub compares, decoded by the sweep. */
	void* navmesh_slot_ = nullptr;

	/** Addresses verified by the sweep; InstallHooks refuses otherwise. */
	bool signatures_verified_ = false;

	bool hooks_installed_ = false;
	KHook::Function<void, class CEntityInstance*, class CCommand&, bool, int, const char*>
		hook_host_say_;
	KHook::Function<void, CEntityInstance*, const char*, CEntityInstance*,
	                CEntityInstance*, void*, int, void*>
		hook_accept_input_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_GAMEDATASUITE_H_ */
