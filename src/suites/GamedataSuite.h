#ifndef CS2SDKTEST_SUITES_GAMEDATASUITE_H_
#define CS2SDKTEST_SUITES_GAMEDATASUITE_H_

#include <cstddef>
#include <map>
#include <string>

#include <khook.hpp>

#include "core/Platform.h"
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
	/** Where one signature key's pattern landed, or why it did not. */
	struct ScanHit
	{
		const void* address = nullptr;
		std::size_t rva = 0;
		std::size_t pattern_bytes = 0;
		std::string error; // empty on a single, clean match
	};

	/** A behaviour check, run inside the key's own row. */
	using Verifier = void (GamedataSuite::*)(TestContext&);

	/** A vtable slot's behaviour check; the row's table and slot number. */
	using VerifierSlot = void (GamedataSuite::*)(TestContext&, const std::string&, long long);

	/**
	 * What one signature key is checked with: the prerequisites it needs
	 * (kNeeds* bits, see the .cpp) and either a verifier or, for the keys
	 * no safe call exists for, the reason the row is scan-only.
	 */
	struct SignatureCheck
	{
		const char* name;
		unsigned needs;
		Verifier verify;
		const char* scan_only_reason;
	};

	/** The check table, looked up by key name. Null for an unknown key. */
	static const SignatureCheck* FindCheck(const std::string& name);

	/** Scans one key's pattern; the result is kept, no row is written. */
	ScanHit ScanSignature(TestContext& context, const std::string& table);

	/**
	 * One row per signature key: scan result plus behaviour verdict. A
	 * failed scan fails the row, a missing prerequisite skips the behaviour
	 * with the reason, and otherwise the verifier decides.
	 */
	void ExerciseSignatureRow(TestContext& context, const std::string& table);

	/** The first prerequisite in `needs` this run lacks, or null. */
	const char* MissingPrerequisite(unsigned needs) const;

	/** Address a key resolved to this run, or null. */
	void* Address(const char* name) const;

	/** Detail for the current row, prefixed with where the scan landed. */
	void RowDetail(TestContext& context, const char* format, ...) CS2ST_PRINTF_FMT(3, 4);

	/** A scratch entity to call against, spawned when DispatchSpawn resolved. */
	void* CreateScratchEntity(bool spawn, const char* classname = "info_target");
	void RemoveScratchEntity(void* entity);

	/** The AK-47's weapon vdata, through the key format the game uses. */
	void* LookupAk47VData();

	/**
	 * Finds the bot the player-shaped rows need. A bot's pawn is made by the
	 * controller's own think, a frame after bot_add, so a sweep cannot add a
	 * bot and use it in the same call. This neither waits nor restarts: with
	 * no bot in the server it queues one 'bot_add t' that is then left in
	 * place (never kicked), and the bot rows skip with that reason. Starting
	 * the server with `+bot_quota 1` has the bot there from the first run.
	 */
	void PrepareBot(TestContext& context);

	// ---- verifiers, one per signature key that has a safe call ------------

	void VerifyClientPrintAll(TestContext& context);
	void VerifyClientPrint(TestContext& context);
	void VerifySwitchTeam(TestContext& context);
	void VerifySetPawn(TestContext& context);
	void VerifyPostThink(TestContext& context);
	void VerifyEventManagerInit(TestContext& context);
	void VerifyGiveNamedItem(TestContext& context);
	void VerifyUtilRemove(TestContext& context);
	void VerifySetModel(TestContext& context);
	void VerifyCanUse(TestContext& context);
	void VerifyCanAcquire(TestContext& context);
	void VerifyWeaponData(TestContext& context);
	void VerifyTerminateRound(TestContext& context);
	void VerifyCreateEntityByName(TestContext& context);
	void VerifyDispatchSpawn(TestContext& context);
	void VerifyEmitSoundFilter(TestContext& context);
	void VerifyAddEntityIOEvent(TestContext& context);
	void VerifyLegacyListener(TestContext& context);
	void VerifyRemovePlayerItem(TestContext& context);
	void VerifyTakeDamageOld(TestContext& context);
	void VerifyTakeDamageInfoConstructor(TestContext& context);
	void VerifyEndTouch(TestContext& context);
	void VerifyStartTouch(TestContext& context);
	void VerifyAcceptInput(TestContext& context);
	void VerifyHostSay(TestContext& context);
	void VerifyFireOutputInternal(TestContext& context);
	void VerifyGameSystemList(TestContext& context);
	void VerifyNetworkStateChanged(TestContext& context);
	void VerifyNavMesh(TestContext& context);

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
	 * Finds a bot by walking the entity list for CCSPlayerController
	 * entities with a zero m_steamID (fake clients). `name` receives the
	 * bot's m_iszPlayerName. The pawn is the
	 * controller's m_hPawn resolved through the entity system, and stays
	 * null while the bot has not spawned yet. False when no bot is in the
	 * server.
	 */
	bool FindBotPair(CEntityInstance*& controller, CEntityInstance*& pawn, std::string& name);

	/**
	 * Walks the active entity list matching the C++ class name off each
	 * identity's class (EntityInstanceByClassIter_t matches designer names,
	 * which is a different axis). Null when nothing in the list is one.
	 */
	CEntityInstance* FindEntityByCppClassname(const char* cpp_class_name);

	/** vtable slots, one row per key, in file order. */
	void ExerciseVtableSlots(TestContext& context);

	/**
	 * The function pointer at `slot` in `object`'s vtable, when that pointer
	 * lands inside the server module; null otherwise. The bounds check is the
	 * row's first assertion: a stale slot number past the table end reads
	 * outside it, and a row that called that pointer would jump through
	 * garbage. Every behavioural vtable row goes through here first.
	 */
	void* VtableSlotInServer(TestContext& context, const void* object, long long slot);

	/** The behaviour checks the vtable rows run, one per called key. */
	VerifierSlot FindSlotCheck(const std::string& name);

	/** Why the bot rows skip, when PrepareBot found no usable bot. */
	const char* BotMissingReason() const;

	/** The behaviour checks the vtable rows run, one per called key. */
	void VerifyChangeTeamSlot(TestContext& context, const std::string& table, long long slot);
	void VerifyRespawnSlot(TestContext& context, const std::string& table, long long slot);
	void VerifyGiveNamedItemSlot(TestContext& context, const std::string& table, long long slot);
	void VerifyDropActiveWeaponSlot(TestContext& context, const std::string& table,
	                                long long slot);
	void VerifyRemoveWeaponsSlot(TestContext& context, const std::string& table, long long slot);
	void VerifyGetSkeletonInstanceSlot(TestContext& context, const std::string& table,
	                                   long long slot);
	void VerifyFindPickerEntitySlot(TestContext& context, const std::string& table,
	                                long long slot);
	void VerifyIsPlayerPawnSlot(TestContext& context, const std::string& table, long long slot);
	void VerifyCommitSuicideSlot(TestContext& context, const std::string& table, long long slot);
	void VerifyTeleportSlot(TestContext& context, const std::string& table, long long slot);
	void VerifySetStateChangedSlot(TestContext& context, const std::string& table,
	                               long long slot);

	/**
	 * vtables.CServerSideClient is not a slot but the vtable discriminator
	 * CSS's RTTI lookup takes; the row runs that lookup against engine2's own
	 * RTTI and requires exactly one CServerSideClient vtable with the offset.
	 */
	void VerifyServerSideClientRtti(TestContext& context, const TomlData::Entry& entry);

	/** byte offsets cross-checked against offsetof() where the SDK declares. */
	void ExerciseStructOffsets(TestContext& context);
	void VerifyTakeDamageHitGroup(TestContext& context, long long offset);

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

	/** ServerCommand - the bot_add route. Null is not fatal. */
	IVEngineServer2* engine_ = nullptr;

	/** The schema system and the server module's type scope, for offsets. */
	ISchemaSystem* schema_ = nullptr;
	CSchemaSystemTypeScope* server_scope_ = nullptr;
	std::string server_scope_key_; // the spelling that found it

	/**
	 * Every signature key's scan, by key name (no "signatures." prefix),
	 * filled before any row is written so a row can call another key's
	 * function. Reset on every run.
	 */
	std::map<std::string, ScanHit> scans_;

	/** "server+0x..." for the row being written; RowDetail's prefix. */
	std::string row_location_;

	/** The bot the player-shaped rows call against, when one is in the server. */
	CEntityInstance* bot_controller_ = nullptr;
	CEntityInstance* bot_pawn_ = nullptr;
	std::string bot_name_;

	/** Why the bot rows skip, when PrepareBot found no usable bot. */
	std::string bot_skip_reason_;

	/** The bot pawn's m_pItemServices, resolved through the schema. */
	void* item_services_ = nullptr;

	/** The AK the GiveNamedItem row handed the bot; later rows use it. */
	void* weapon_ = nullptr;

	/** Set when CommitSuicide's slot row killed the bot pawn. */
	bool bot_pawn_dead_ = false;

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
