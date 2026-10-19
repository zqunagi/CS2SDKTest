#include "suites/GamedataSuite.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>

#include <engine2/igameresourceservice.h>
#include <engine2/ivengineserver2.h>
#include <engine2/isource2server.h>
#include <entity2/entityclass.h>
#include <entity2/entityinstance.h>
#include <entity2/entitysystem.h>
#include <gameentitysystem.h>
#include <interfaces/interfaces.h>
#include <recipientfilter.h>
#include <schemasystem/schemasystem.h>
#include <tier0/convar.h>

#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/SignatureScan.h"
#include "hook/HookArg.h"
#include "hook/HookRecorder.h"

/**
 * The one suite whose subject is not an SDK interface but a config file: the
 * byte patterns, slot indexes and offsets the plugin ecosystem resolves out of
 * band because no header declares them. The header comment on GamedataSuite.h
 * describes the shape; this file is the sweep.
 *
 * The function types below are the ecosystem's own, transcribed from how CSS
 * declares and calls each entry - not paraphrased - because the point of a
 * behavioural check is calling the function the way its consumers do.
 */

namespace cs2st
{
namespace
{

// ---- hook ids ---------------------------------------------------------------

constexpr const char* kHostSay = "Host_Say";
constexpr const char* kAcceptInput = "CEntityInstance_AcceptInput";

/** Number of function hooks; InstallHooks reports it. */
constexpr int kHookCount = 2;

// ---- table prefixes in the TOML ----------------------------------------------

constexpr const char* kSignaturesPrefix = "signatures.";
constexpr const char* kVtablesPrefix = "vtables.";

/** Full TOML table paths for the two vtable keys an SDK declaration computes. */
constexpr const char* kCheckTransmitTable = "vtables.ISource2GameEntities::CheckTransmit";
constexpr const char* kAddResourceTable = "vtables.CEntityResourceManifest_AddResource";

/** CCommand's own COMMAND_MAX_ARGC, which is private, so it is restated here. */
constexpr int kMaxPlausibleArgC = 64;

// ---- the ecosystem's function types ------------------------------------------

using HostSay_t = void (*)(CEntityInstance*, CCommand&, bool, int, const char*);
using CreateEntityByName_t = void* (*)(const char*, int);
using DispatchSpawn_t = void (*)(void*, void*);
using UtilRemove_t = void (*)(void*);
using WeaponDataFromKey_t = void* (*)(int, const char*);
using NavMeshBool_t = bool (*)();

// The scan-only signatures' types, transcribed the same way: from the
// ecosystem's declarations where it has one (FireOutputInternal,
// AddEntityIOEvent, LegacyGameEventListener, TakeDamageOld), and from the
// live build's own registers where it does not - every pointer-width count
// below was read off build 25947352's prologue saves, not guessed.

// UTIL_ClientPrintAll(dest, msg_name, param1..param4): the six integer
// registers the prologue spills (rdi..r9), with msg_name landing in strlen.
using ClientPrintAll_t = void (*)(int, const char*, const char*, const char*,
                                  const char*, const char*);
// ClientPrint(out, filter, origin, dest, ...): writes a vtable into out[0]
// and an object chain into out[1]/out[2]; the pointer arguments are all read
// (the filter's callback slot, the origin vectors), so callers pass buffers,
// never null.
using ClientPrint_t = void (*)(void*, const void*, const void*, int, const void*,
                               const void*, char, const void*);
using SwitchTeam_t = void (*)(void*, int);
using SetPawn_t = void (*)(void*, void*, bool, bool, bool, bool);
using PostThink_t = void (*)(void*);
// GiveNamedItem(services, name, subType, pInfo, bool, ptr, double, float):
// the prologue saves arg3 (esi), arg4 (rcx), arg5 (r8d), arg6 (r9) and arg7
// (rdi) before the first test; the two floats ride xmm.
using GiveNamedItem_t = void* (*)(void*, const char*, int, void*, bool, void*,
                                  double, float);
using SetModel_t = void (*)(void*);
using CanUse_t = int (*)(void*, void*);
using CanAcquire_t = int (*)(void*, void*, int, int*);
using TerminateRound_t = void (*)(void*, int, long long, float);
// EmitSoundFilter(out, filter, entindex, params): returns its sound-event guid
// through the hidden first argument (32 bytes at the callers, seeded with
// 0xFFFFFFFF00000000) and hands that pointer back. The filter is a
// CRecipientFilter-derived object whose virtuals the body calls, so it must
// have a real vtable.
using EmitSoundFilter_t = void* (*)(void*, const void*, unsigned, const void*);
using AddEntityIOEvent_t = void (*)(void*, void*, const char*, void*, void*,
                                    void*, float, void*, void*);
using LegacyListener_t = void* (*)(int);
using RemovePlayerItem_t = void (*)(void*, void*);
// TakeDamageOld(pawn, info, result, double, float): the decompiler's two
// trailing float arguments are what CSS's 3-argument typedef misses.
using TakeDamageOld_t = long long (*)(void*, void*, void*, double, float);
using EndTouch_t = void (*)(void*, void*);
// FireOutputInternal(output, activator, caller, value, delay, unk1, unk2):
// returns its first argument untouched when the output has no connections.
using FireOutputInternal_t = void* (*)(void*, void*, void*, const void*, float,
                                       void*, void*);
// AcceptInput(self, input, activator, caller, value, output_id, unk): the
// engine's input pump entry, the same call the AddEntityIOEvent row queues
// for the next frame. Seven arguments, the shape this suite's own detour
// forwards. The ecosystem's detour typedef, called directly.
using AcceptInput_t = long long (*)(void*, const char*, void*, void*, void*, int, void*);
// Host_Say(controller, args, teamonly, unk1, unk2): the say path every chat
// message and chat trigger goes through.
using HostSayDirect_t = void (*)(void*, CCommand&, bool, int, const char*);
// StartTouch(trigger, other): appends `other` to the trigger's touch list,
// the half of the touch pair the EndTouch signature removes.
using StartTouch_t = void (*)(void*, void*);

// ---- helpers -----------------------------------------------------------------

/** Reads a whole file as text. stdio, like everything else in core/. */
bool ReadTextFile(const std::string& path, std::string& out)
{
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		return false;
	}

	char buffer[4096];
	std::size_t got = 0;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
	{
		out.append(buffer, got);
	}
	const bool ok = (std::ferror(file) == 0);
	std::fclose(file);
	return ok;
}

/**
 * Directory gamedata.toml lives in: <mod>/addons/<alias>/configs, derived from
 * this plugin's own binary the same way LogDirectory() derives its logs path.
 */
std::string ConfigsGamedataPath()
{
	const std::string binary = fs::PluginBinaryPath();
	if (binary.empty())
	{
		return fs::Join("configs", "gamedata.toml");
	}

	const std::string plugin_dir = fs::DirectoryName(fs::DirectoryName(binary));
	if (plugin_dir.empty())
	{
		return fs::Join("configs", "gamedata.toml");
	}
	return fs::Join(fs::Join(plugin_dir, "configs"), "gamedata.toml");
}

/** Reads the pointer-sized slot `offset` bytes into `object`. */
void* SlotAt(const void* object, std::size_t offset)
{
	void* value = nullptr;
	// memcpy rather than a dereference: the offset is what this suite tests,
	// and nothing guarantees alignment on the hypothesis being checked.
	std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
	return value;
}

/** True when `address` lies inside `module`'s mapped image. */
bool InModule(const Module& module, const void* address)
{
	const auto* base = static_cast<const unsigned char*>(module.base());
	const auto* at = static_cast<const unsigned char*>(address);
	return base != nullptr && at >= base && at < base + module.size();
}

/**
 * Schema class names the pawn's service pointers live in, most derived first.
 * A field is declared once, in its declaring class; walking down finds that
 * declaration wherever the dump put it.
 */
constexpr const char* kItemServicesClasses[] = {"CCSPlayerPawn", "CCSPlayerPawnBase",
                                                "CBasePlayerPawn", nullptr};

/**
 * Schema class names the pawn's last-hit fields may be declared in, most
 * derived first. The current binary's field table carries m_LastHitBox
 * (CCSPlayerPawnBase, 0x17bc); the ecosystem's dump still names
 * m_LastHitGroup on CBaseCombatCharacter - a build older than the server
 * here. Both spellings are tried through the same chain, whichever side of
 * the rename this build sits on.
 */
constexpr const char* kLastHitGroupClasses[] = {"CCSPlayerPawn", "CCSPlayerPawnBase",
                                                "CBasePlayerPawn", "CBaseCombatCharacter",
                                                "CBaseModelEntity", "CBaseEntity", nullptr};

/**
 * The key format GetCSWeaponDataFromKey takes: the item definition index as
 * decimal text. The game's own callers (build 25947352, sub_1456760) format
 * m_iItemDefinitionIndex with "%d" and pass category 1; 7 is the AK-47.
 */
constexpr const char* kAk47Key = "7";

/** The category filter that disables the check in GetCSWeaponDataFromKey. */
constexpr int kAnyWeaponCategory = -1;

// Prerequisite bits for SignatureCheck::needs. Each names something a row
// borrows from elsewhere in the run; MissingPrerequisite turns the first one
// absent into the row's skip reason.
constexpr unsigned kNeedsNothing = 0;
constexpr unsigned kNeedsCreate = 1u << 0;       // UTIL_CreateEntityByName resolved
constexpr unsigned kNeedsBot = 1u << 1;          // a bot is in the server
constexpr unsigned kNeedsItemServices = 1u << 2; // the bot pawn's m_pItemServices
constexpr unsigned kNeedsWeapon = 1u << 3;       // the GiveNamedItem row's AK
constexpr unsigned kNeedsWeaponData = 1u << 4;   // GetCSWeaponDataFromKey resolved

} // namespace

// -----------------------------------------------------------------------------
//  Acquire - the file, and nothing but the file
// -----------------------------------------------------------------------------

bool GamedataSuite::Acquire(TestContext& context)
{
	// This also runs on the hooks-install path, which has no fault guard, so
	// nothing here goes past file reads and interface queries. The scans and
	// every engine call live in ExerciseCalls, inside CS2ST_CALL.
	//
	// The file is re-read on every run rather than cached: editing a pattern
	// and re-running, without reloading the plugin, is the workflow this file
	// exists for.
	const std::string path = ConfigsGamedataPath();
	std::string text;
	if (!ReadTextFile(path, text))
	{
		data_loaded_ = false;
		context.UnavailableCall("configs/gamedata.toml", "cannot open %s", path.c_str());
		return false;
	}

	data_ = TomlData::Parse(text);
	if (!data_.ok())
	{
		data_loaded_ = false;
		context.UnavailableCall("configs/gamedata.toml",
			"parse error at line %d: %s",
			data_.error_line(), data_.error().c_str());
		return false;
	}

	data_loaded_ = true;
	context.Note("gamedata file: %s (%zu key(s))", path.c_str(), data_.tables().size());

	// The service object the GameEntitySystem offset is read from. Not fatal:
	// every oracle except that one works from the file and the compiler alone.
	resource_service_ = context.QueryInterface(
		"engine2", GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	if (resource_service_ == nullptr)
	{
		context.Note("engine2 does not answer %s; the GameEntitySystem offset will be "
		             "compared as numbers only",
			GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	}
	else
	{
		// The SDK's GameEntitySystem() reads this global, and nothing but
		// GameEntitySystemSuite's Acquire fills it - so `meta_test run
		// gamedata` on its own saw a null entity system in every row that
		// walks the entity list or queues through it.
		g_pGameResourceServiceServer = static_cast<IGameResourceService*>(resource_service_);
	}

	// The object vtables.GameEventManager is read from. Not fatal either: that
	// row fails on its own when the interface is absent.
	server_ = context.QueryInterface("server", INTERFACEVERSION_SERVERGAMEDLL);

	// ServerCommand - the bot management the behavioural layer queues. Not
	// fatal: every bot-dependent check skips with its own line when absent.
	engine_ = static_cast<IVEngineServer2*>(context.QueryInterface(
		"engine2", INTERFACEVERSION_VENGINESERVER));

	// The schema system and the server module's type scope, where the
	// behavioural layer resolves the offsets its calls read. Same deal: not
	// fatal, the checks that need an offset skip.
	schema_ = static_cast<ISchemaSystem*>(context.QueryInterface(
		"schemasystem", SCHEMASYSTEM_INTERFACE_VERSION));
	server_scope_ = nullptr;
	server_scope_key_.clear();
	if (schema_ != nullptr)
	{
		// Which spelling the schema system keys its scopes by is undocumented
		// (SchemaSystemSuite enumerates m_TypeScopes to find out): "server"
		// finds nothing on Linux, where the scope is the module's file name.
		// The loaded module's own name goes first, then the known spellings;
		// FindTypeScopeForModule only looks, it never creates a scope.
		const Module& server_module = context.GetModule("server");
		const std::string module_name = server_module.IsValid() ? server_module.name()
		                                                        : std::string();
		const char* const candidates[] = {module_name.c_str(), "libserver.so",
		                                  "server.dll", "server", nullptr};
		for (const char* const* key = candidates; *key != nullptr && server_scope_ == nullptr;
		     ++key)
		{
			if (**key != '\0')
			{
				server_scope_ = schema_->FindTypeScopeForModule(*key);
				if (server_scope_ != nullptr)
				{
					server_scope_key_ = *key;
				}
			}
		}
		if (server_scope_ == nullptr)
		{
			context.Note("schema: no type scope under any of the known server names; rows "
			             "that read an offset through the schema will fail at setup");
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
//  The sweep
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseCalls(TestContext& context)
{
	// A run can be repeated; each one re-resolves from the file as written.
	scans_.clear();
	bot_controller_ = nullptr;
	bot_pawn_ = nullptr;
	bot_name_.clear();
	bot_skip_reason_.clear();
	item_services_ = nullptr;
	weapon_ = nullptr;
	bot_pawn_dead_ = false;
	signatures_verified_ = false;

	// Every pattern is scanned before any row is written: a row's behaviour
	// check may call another key's function (the entity rows all create
	// through UTIL_CreateEntityByName), wherever that key sits in the file.
	for (const std::string& table : data_.tables())
	{
		if (table.rfind(kSignaturesPrefix, 0) == 0)
		{
			scans_[table.substr(std::strlen(kSignaturesPrefix))] = ScanSignature(context, table);
		}
	}

	// Hooks need exactly two of those addresses, each landed on by one match.
	signatures_verified_ =
		(Address(kHostSay) != nullptr && Address(kAcceptInput) != nullptr);

	PrepareBot(context);

	context.Note("[signatures] one row per key: pattern scan, then a behaviour check "
	             "where a safe call exists");

	int resolved = 0;
	int total = 0;
	for (const std::string& table : data_.tables())
	{
		if (table.rfind(kSignaturesPrefix, 0) != 0)
		{
			continue;
		}
		++total;
		if (scans_[table.substr(std::strlen(kSignaturesPrefix))].error.empty())
		{
			++resolved;
		}
		ExerciseSignatureRow(context, table);
	}

	// Offsets before vtables: the HitGroup row needs the bot pawn alive
	// (TakeDamageOld on it), and the vtable rows are destructive - the
	// CommitSuicide slot kills the pawn, RemoveWeapons strips the arsenal.
	// File order still wins within each section.
	ExerciseStructOffsets(context);
	ExerciseVtableSlots(context);

	context.Note("gamedata sweep: %d of %d signature(s) resolved to one address; "
	             "hooks are %seligible",
		resolved, total, signatures_verified_ ? "" : "NOT ");
}

GamedataSuite::ScanHit GamedataSuite::ScanSignature(TestContext& context,
                                                    const std::string& table)
{
	ScanHit hit;

	// Guarded like any call: the scan reads the whole mapped image, and an
	// unreadable page is a finding rather than a reason to take the server down.
	FaultInfo fault;
	const CallOutcome outcome = InvokeGuarded([&]() {
		const Module& server = context.GetModule("server");
		if (!server.IsValid())
		{
			hit.error = "scan: the server module is not loaded";
			return;
		}

		const TomlData::Entry* library = data_.Find(table + ".library");
		if (library == nullptr || library->text != "server")
		{
			hit.error = "scan: no library = \"server\" key; this suite only scans the server module";
			return;
		}

		const TomlData::Entry* pattern_entry = data_.FindPlatform(table, CS2ST_WINDOWS != 0);
		if (pattern_entry == nullptr || pattern_entry->is_number)
		{
			hit.error = std::string("scan: no ") + (CS2ST_WINDOWS ? "windows" : "linux")
				+ " pattern in the file";
			return;
		}

		BytePattern pattern;
		const char* parse_error = ParseBytePattern(pattern_entry->text, pattern);
		if (parse_error != nullptr)
		{
			hit.error = std::string("scan: pattern does not parse: ") + parse_error;
			return;
		}
		hit.pattern_bytes = pattern.size();

		const PatternScanResult result = ScanModule(server, pattern);
		char text[160];
		if (result.matches == 0)
		{
			// The headline this file exists for: an engine update moved the
			// bytes, and every plugin carrying this key stopped loading.
			std::snprintf(text, sizeof(text), "scan: %zu-byte pattern matches nothing in %s",
				pattern.size(), server.name().c_str());
			hit.error = text;
			return;
		}
		if (result.matches > 1)
		{
			// Two hits is how a too-short pattern announces itself. The first
			// hit is not more likely the function than the second, so neither
			// is kept.
			std::snprintf(text, sizeof(text),
				"scan: pattern matches more than once (%p and %p); it does not name one function",
				result.first, result.second);
			hit.error = text;
			return;
		}

		hit.address = result.first;
		hit.rva = static_cast<std::size_t>(static_cast<const unsigned char*>(result.first)
			- static_cast<const unsigned char*>(server.base()));
	}, &fault);

	if (outcome == CallOutcome::kCrashed)
	{
		hit = ScanHit{};
		hit.error = "scan: faulted - " + fault.description;
	}
	return hit;
}

const GamedataSuite::SignatureCheck* GamedataSuite::FindCheck(const std::string& name)
{
	// Every key the file carries, with the check its row runs. The order is
	// irrelevant here - rows follow the file - but the prerequisites are not:
	// a row needing kNeedsWeapon only runs after the GiveNamedItem row, which
	// the file puts first.
	static const SignatureCheck kChecks[] = {
		{"UTIL_ClientPrintAll", kNeedsNothing, &GamedataSuite::VerifyClientPrintAll, nullptr},
		{"ClientPrint", kNeedsNothing, &GamedataSuite::VerifyClientPrint, nullptr},
		{"CCSPlayerController_SwitchTeam", kNeedsBot, &GamedataSuite::VerifySwitchTeam, nullptr},
		{"CBasePlayerController_SetPawn", kNeedsBot, &GamedataSuite::VerifySetPawn, nullptr},
		{"CCSPlayerPawnBase_PostThink", kNeedsBot, &GamedataSuite::VerifyPostThink, nullptr},
		{"CGameEventManager_Init", kNeedsNothing, &GamedataSuite::VerifyEventManagerInit, nullptr},
		{"GiveNamedItem", kNeedsBot | kNeedsItemServices, &GamedataSuite::VerifyGiveNamedItem,
			nullptr},
		{"UTIL_Remove", kNeedsCreate, &GamedataSuite::VerifyUtilRemove, nullptr},
		{"CBaseModelEntity_SetModel", kNeedsNothing, &GamedataSuite::VerifySetModel, nullptr},
		{"CCSPlayer_WeaponServices_CanUse", kNeedsBot | kNeedsWeapon,
			&GamedataSuite::VerifyCanUse, nullptr},
		{"CCSPlayer_ItemServices_CanAcquire", kNeedsBot | kNeedsItemServices | kNeedsWeaponData,
			&GamedataSuite::VerifyCanAcquire, nullptr},
		{"GetCSWeaponDataFromKey", kNeedsNothing, &GamedataSuite::VerifyWeaponData, nullptr},
		{"CCSGameRules_TerminateRound", kNeedsNothing, &GamedataSuite::VerifyTerminateRound,
			nullptr},
		{"UTIL_CreateEntityByName", kNeedsNothing, &GamedataSuite::VerifyCreateEntityByName,
			nullptr},
		{"CBaseEntity_DispatchSpawn", kNeedsCreate, &GamedataSuite::VerifyDispatchSpawn, nullptr},
		{"CBaseEntity_EmitSoundFilter", kNeedsNothing, &GamedataSuite::VerifyEmitSoundFilter,
			nullptr},
		{"CEntityInstance_AcceptInput", kNeedsCreate, &GamedataSuite::VerifyAcceptInput,
			nullptr},
		{"CEntitySystem_AddEntityIOEvent", kNeedsCreate, &GamedataSuite::VerifyAddEntityIOEvent,
			nullptr},
		{"LegacyGameEventListener", kNeedsNothing, &GamedataSuite::VerifyLegacyListener, nullptr},
		{"CBasePlayerPawn_RemovePlayerItem", kNeedsBot | kNeedsWeapon,
			&GamedataSuite::VerifyRemovePlayerItem, nullptr},
		{"CBaseEntity_TakeDamageOld", kNeedsBot, &GamedataSuite::VerifyTakeDamageOld, nullptr},
		{"CTakeDamageInfo_Constructor", kNeedsNothing,
			&GamedataSuite::VerifyTakeDamageInfoConstructor, nullptr},
		{"CBaseTrigger_StartTouch", kNeedsCreate, &GamedataSuite::VerifyStartTouch,
			nullptr},
		{"CBaseTrigger_EndTouch", kNeedsNothing, &GamedataSuite::VerifyEndTouch, nullptr},
		{"CEntityIOOutput_FireOutputInternal", kNeedsNothing,
			&GamedataSuite::VerifyFireOutputInternal, nullptr},
		{"IGameSystem_InitAllSystems_pFirst", kNeedsNothing,
			&GamedataSuite::VerifyGameSystemList, nullptr},
		{"NetworkStateChanged", kNeedsNothing, &GamedataSuite::VerifyNetworkStateChanged,
			nullptr},
		{"Host_Say", kNeedsNothing, &GamedataSuite::VerifyHostSay,
			nullptr},
		{"CCSNavArea_IsValidNavMesh", kNeedsNothing, &GamedataSuite::VerifyNavMesh, nullptr},
	};

	for (const SignatureCheck& check : kChecks)
	{
		if (name == check.name)
		{
			return &check;
		}
	}
	return nullptr;
}

void GamedataSuite::ExerciseSignatureRow(TestContext& context, const std::string& table)
{
	const std::string name = table.substr(std::strlen(kSignaturesPrefix));
	const ScanHit& hit = scans_[name];

	if (!hit.error.empty())
	{
		CS2ST_CALL(context, table, {
			context.Fail("%s", hit.error.c_str());
		});
		return;
	}

	char location[64];
	std::snprintf(location, sizeof(location), "server+0x%zx", hit.rva);
	row_location_ = location;

	const SignatureCheck* check = FindCheck(name);
	if (check == nullptr || check->verify == nullptr)
	{
		CS2ST_CALL(context, table, {
			RowDetail(context, "scan only (%s)",
				check != nullptr ? check->scan_only_reason
				                 : "no behaviour check is written for this key");
		});
		return;
	}

	const char* missing = MissingPrerequisite(check->needs);
	if (missing != nullptr)
	{
		context.SkipCall(table, "%s | scan ok, behaviour not run: %s",
			row_location_.c_str(), missing);
		return;
	}

	const Verifier verify = check->verify;
	CS2ST_CALL(context, table, {
		(this->*verify)(context);
	});
}

const char* GamedataSuite::MissingPrerequisite(unsigned needs) const
{
	if ((needs & kNeedsCreate) != 0 && Address("UTIL_CreateEntityByName") == nullptr)
	{
		return "needs UTIL_CreateEntityByName, which did not resolve";
	}
	if ((needs & kNeedsWeaponData) != 0 && Address("GetCSWeaponDataFromKey") == nullptr)
	{
		return "needs GetCSWeaponDataFromKey, which did not resolve";
	}
	if ((needs & kNeedsBot) != 0 && bot_pawn_ == nullptr)
	{
		return bot_skip_reason_.empty() ? "needs a bot, and none is in the server"
		                                : bot_skip_reason_.c_str();
	}
	if ((needs & kNeedsItemServices) != 0 && item_services_ == nullptr)
	{
		return "needs the bot pawn's m_pItemServices, which did not resolve";
	}
	if ((needs & kNeedsWeapon) != 0 && weapon_ == nullptr)
	{
		return "needs the AK the GiveNamedItem row gives; that row produced none";
	}
	return nullptr;
}

void* GamedataSuite::Address(const char* name) const
{
	const auto it = scans_.find(name);
	return it != scans_.end() ? const_cast<void*>(it->second.address) : nullptr;
}

void GamedataSuite::RowDetail(TestContext& context, const char* format, ...)
{
	char text[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	context.Detail("%s | %s", row_location_.c_str(), text);
}

void* GamedataSuite::CreateScratchEntity(bool spawn, const char* classname)
{
	// The second argument is the entity index to force, -1 for "any free
	// slot": the function forwards it as-is to the entity system's create
	// call (Linux build 25815307). 0 would ask for worldspawn's slot.
	void* entity = reinterpret_cast<CreateEntityByName_t>(
		Address("UTIL_CreateEntityByName"))(classname, -1);
	if (entity == nullptr || !LooksLikeObjectPointer(entity))
	{
		return nullptr;
	}
	void* dispatch = Address("CBaseEntity_DispatchSpawn");
	if (spawn && dispatch != nullptr)
	{
		// CSS calls this with a null CEntityKeyValues* - spawn with no
		// overrides. A spawned entity is one UTIL_Remove marks rather than
		// frees, which keeps its identity readable after the removal.
		reinterpret_cast<DispatchSpawn_t>(dispatch)(entity, nullptr);
	}
	return entity;
}

void GamedataSuite::RemoveScratchEntity(void* entity)
{
	void* remove = Address("UTIL_Remove");
	if (entity != nullptr && remove != nullptr)
	{
		reinterpret_cast<UtilRemove_t>(remove)(entity);
	}
}

void* GamedataSuite::LookupAk47VData()
{
	return reinterpret_cast<WeaponDataFromKey_t>(Address("GetCSWeaponDataFromKey"))(
		kAnyWeaponCategory, kAk47Key);
}

// -----------------------------------------------------------------------------
//  The bot the player-shaped rows call against
// -----------------------------------------------------------------------------

void GamedataSuite::PrepareBot(TestContext& context)
{
	const auto none = [&](const std::string& reason) {
		bot_skip_reason_ = reason;
		context.Note("bot: %s; the player-shaped rows skip", reason.c_str());
	};

	if (engine_ == nullptr)
	{
		none(std::string("engine2 does not answer ") + INTERFACEVERSION_VENGINESERVER);
		return;
	}

	// Without these offsets FindBotPair finds nothing whether or not a bot is
	// there, and adding one every run would leave a trail of them.
	std::size_t unused = 0;
	if (!SchemaOffset("CBasePlayerController", "m_steamID", unused)
		|| !SchemaOffset("CBasePlayerController", "m_hPawn", unused)
		|| !SchemaOffset("CBasePlayerController", "m_iszPlayerName", unused))
	{
		none(server_scope_ == nullptr
			? "no server type scope in the schema system, so a bot cannot be told from "
			  "any other controller"
			: "CBasePlayerController.m_steamID/m_hPawn/m_iszPlayerName did not resolve "
			  "through the schema");
		return;
	}

	CEntityInstance* controller = nullptr;
	CEntityInstance* pawn = nullptr;
	std::string name;
	if (!FindBotPair(controller, pawn, name))
	{
		// bot_add is queued, and the pawn is made a frame after the controller,
		// so it cannot be used in this call. The bot stays: nothing kicks it.
		// No profile name: the argument after the difficulty is a bot profile
		// lookup, and an unknown one ("cs_bot") adds no bot at all.
		engine_->ServerCommand("bot_add t\n");
		none("no bot in the server - 'bot_add t' queued and left in place, so the next run "
		     "has one (start the server with +bot_quota 1 to have it from the first)");
		return;
	}
	if (pawn == nullptr)
	{
		none("bot '" + name + "' has a controller but no pawn (dead, or still spawning)");
		return;
	}

	bot_controller_ = controller;
	bot_pawn_ = pawn;
	bot_name_ = name;

	std::size_t itemservices_offset = 0;
	if (SchemaOffsetAny(kItemServicesClasses, "m_pItemServices", itemservices_offset))
	{
		void* services = SlotAt(pawn, itemservices_offset);
		item_services_ = LooksLikeObjectPointer(services) ? services : nullptr;
	}

	context.Note("bot: '%s' (controller %p, pawn %p, item services %p)",
		bot_name_.c_str(), static_cast<void*>(controller), static_cast<void*>(pawn),
		item_services_);
}

// -----------------------------------------------------------------------------
//  Verifiers - each runs inside its key's row, after the scan has held
// -----------------------------------------------------------------------------

void GamedataSuite::VerifyClientPrintAll(TestContext& context)
{
	// UTIL_ClientPrintAll sends a TextMsg to everyone; on a dedicated server
	// with no human client the message is still routed, so the call must
	// simply survive it. The name strings are read by strlen, nulls are not
	// an option for a caller that passes them.
	reinterpret_cast<ClientPrintAll_t>(Address("UTIL_ClientPrintAll"))(
		4, "cs2sdktest gamedata behaviour sweep", nullptr, nullptr, nullptr, nullptr);
	RowDetail(context, "HUD_PRINTTALK line sent to all clients, call returned");
}

void GamedataSuite::VerifyClientPrint(TestContext& context)
{
	// ClientPrint(out, ...) fills a message-builder object. Every pointer
	// argument but the last is dereferenced before the builder is touched
	// (build 25947352: the filter's callback at +8, two vectors out of a3,
	// a5[0] and a5[2], sixteen bytes out of a6 plus its second qword) - so
	// each rides its own zeroed buffer, and a zeroed buffer is the shape
	// whose callback slot is null, which the body branches on and handles.
	unsigned char out[384] = {0};
	unsigned char filter_bytes[64] = {0};
	unsigned char origin[32] = {0}; // two unaligned loads reach +27
	unsigned char unknown12[12] = {0};
	unsigned char unknown32[32] = {0};
	reinterpret_cast<ClientPrint_t>(Address("ClientPrint"))(
		out, filter_bytes, origin, 4, unknown12, unknown32, 0, nullptr);

	// The binary writes a module vtable into out[0] before anything else; a
	// null out[0] means nothing was built.
	void* first = nullptr;
	std::memcpy(&first, out, sizeof(first));
	RowDetail(context, "builder out[0] = %p", first);
	if (first == nullptr)
	{
		context.Fail("behaviour: the builder came back empty (out[0] null); the address is "
		             "not ClientPrint");
	}
	else if (!InModule(context.GetModule("server"), first))
	{
		context.Fail("behaviour: out[0] does not point into the server module; the builder's "
		             "vtable is wrong");
	}
}

void GamedataSuite::VerifySwitchTeam(TestContext& context)
{
	// The ecosystem calls this on the controller (CSS's
	// CCSPlayerController.SwitchTeam), and the body compares the requested
	// team with the byte at this+m_iTeamNum before switching: that byte is
	// both the input and the assertion. CBaseEntity declares the field, so
	// one schema offset serves the controller.
	std::size_t team_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_iTeamNum", team_offset))
	{
		context.Fail("setup: CBaseEntity.m_iTeamNum did not resolve through the schema");
		return;
	}
	auto* bytes = reinterpret_cast<unsigned char*>(bot_controller_);
	const auto switch_team = reinterpret_cast<SwitchTeam_t>(Address(
		"CCSPlayerController_SwitchTeam"));

	const std::uint8_t before = bytes[team_offset];
	const std::uint8_t target = (before == 3) ? 2 : 3;
	switch_team(bot_controller_, target);
	const std::uint8_t after = bytes[team_offset];

	// Restore: back to the team the round started the bot in.
	if (after != before && (before == 2 || before == 3))
	{
		switch_team(bot_controller_, before);
	}
	const std::uint8_t restored = bytes[team_offset];

	RowDetail(context, "controller team %u -> %u (asked %u) -> restored %u",
		before, after, target, restored);
	if (after != target)
	{
		context.Fail("behaviour: m_iTeamNum did not follow the call; the address is not "
		             "SwitchTeam");
	}
}

void GamedataSuite::VerifySetPawn(TestContext& context)
{
	// SetPawn(controller, null) is the engine's own pawn-drop path: the
	// controller's m_hPawn is cleared. Handing the old pawn back must put the
	// original handle back - a round trip null -> old pawn proves both
	// directions, and leaves the bot whole.
	std::size_t hpawn_offset = 0;
	if (!SchemaOffset("CBasePlayerController", "m_hPawn", hpawn_offset))
	{
		context.Fail("setup: CBasePlayerController.m_hPawn did not resolve through the schema");
		return;
	}
	const auto read_handle = [&]() {
		std::uint32_t bits = 0;
		std::memcpy(&bits, reinterpret_cast<const unsigned char*>(bot_controller_)
			+ hpawn_offset, sizeof(bits));
		return bits;
	};
	const auto set_pawn = reinterpret_cast<SetPawn_t>(Address("CBasePlayerController_SetPawn"));

	const std::uint32_t original = read_handle();
	set_pawn(bot_controller_, nullptr, false, false, false, false);
	const std::uint32_t cleared = read_handle();
	set_pawn(bot_controller_, bot_pawn_, false, false, false, false);
	const std::uint32_t restored = read_handle();

	RowDetail(context, "m_hPawn 0x%08x -> null call 0x%08x -> restored 0x%08x",
		original, cleared, restored);
	if (restored != original)
	{
		context.Fail("behaviour: handing the pawn back did not restore m_hPawn");
	}
	else if (cleared == original)
	{
		context.Suspect("behaviour: m_hPawn did not change on the null call");
	}
}

void GamedataSuite::VerifyPostThink(TestContext& context)
{
	// PostThink runs the pawn's per-tick bookkeeping - buy-zone events
	// included, which is the anchor its signature carries. Calling it once
	// out of band must simply complete: the observable (the game event) fires
	// only on a zone transition, which this call does not cause.
	reinterpret_cast<PostThink_t>(Address("CCSPlayerPawnBase_PostThink"))(bot_pawn_);
	RowDetail(context, "ran once on the bot pawn %p, returned", static_cast<void*>(bot_pawn_));
}

void GamedataSuite::VerifyEventManagerInit(TestContext& context)
{
	// Not called on purpose: Init first calls the manager's Reset, which drops
	// every event descriptor and the listeners hanging off them - every later
	// test that listens for an event would silently hear nothing. What makes
	// the address Init is the first file it loads, so that is what is checked.
	constexpr std::size_t kDecodeWindow = 0x80;
	static const char kExpected[] = "resource/core.gameevents";

	const Module& server = context.GetModule("server");
	const auto* fn = static_cast<const unsigned char*>(Address("CGameEventManager_Init"));

	// lea r64, [rip+disp32]: REX.W 8D, then a ModRM with mod=00 rm=101.
	for (std::size_t i = 0; i + 7 <= kDecodeWindow; ++i)
	{
		if ((fn[i] != 0x48 && fn[i] != 0x4C) || fn[i + 1] != 0x8D
			|| (fn[i + 2] & 0xC7) != 0x05)
		{
			continue;
		}
		std::int32_t disp = 0;
		std::memcpy(&disp, fn + i + 3, sizeof(disp));
		const char* text = reinterpret_cast<const char*>(fn + i + 7 + disp);
		if (!InModule(server, text) || !InModule(server, text + sizeof(kExpected) - 1))
		{
			continue;
		}
		if (std::memcmp(text, kExpected, sizeof(kExpected)) == 0)
		{
			RowDetail(context, "decode: +0x%zx loads \"%s\" (not called: Init resets every "
			                   "listener)", i, kExpected);
			return;
		}
	}
	RowDetail(context, "decode: no match in the first 0x%zx bytes", kDecodeWindow);
	context.Fail("decode: no rip-relative lea names \"%s\"; the address is not "
	             "CGameEventManager::Init", kExpected);
}

void GamedataSuite::VerifyGiveNamedItem(TestContext& context)
{
	// The ecosystem's own call shape: subType 0, no info pointer, no default
	// attributes, no script scope. The name table check runs first,
	// "weapon_ak47" passes it, and the vdata path creates and returns the
	// entity - which the CanUse and RemovePlayerItem rows then use.
	void* weapon = reinterpret_cast<GiveNamedItem_t>(Address("GiveNamedItem"))(
		item_services_, "weapon_ak47", 0, nullptr, false, nullptr, 0.0, 0.0f);
	if (weapon == nullptr)
	{
		RowDetail(context, "weapon_ak47 -> (nil)");
		// Not Fail: GiveNamedItem can refuse for live-game reasons (freezetime,
		// buy restrictions) that say nothing about the signature.
		context.Suspect("behaviour: no entity came back; live-game rules may have refused "
		                "the give");
		return;
	}
	if (!LooksLikeObjectPointer(weapon))
	{
		RowDetail(context, "weapon_ak47 -> %p", weapon);
		context.Fail("behaviour: the call returned something that is not a pointer");
		return;
	}

	CEntityIdentity* identity = static_cast<CEntityInstance*>(weapon)->m_pEntity;
	const char* designer = LooksLikeObjectPointer(identity) ? identity->GetClassname() : nullptr;
	RowDetail(context, "weapon_ak47 -> entity %p (%s)", weapon,
		designer != nullptr ? designer : "no identity");
	if (designer == nullptr || std::strcmp(designer, "weapon_ak47") != 0)
	{
		context.Fail("behaviour: the entity that came back is not a weapon_ak47");
		return;
	}
	weapon_ = weapon;
}

void GamedataSuite::VerifyUtilRemove(TestContext& context)
{
	void* entity = CreateScratchEntity(true);
	if (entity == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity to remove");
		return;
	}
	reinterpret_cast<UtilRemove_t>(Address("UTIL_Remove"))(entity);

	// UTIL_Remove marks for deletion rather than freeing, so the flag read
	// back is the evidence the call reached the entity system at all.
	CEntityInstance* instance = static_cast<CEntityInstance*>(entity);
	if (!LooksLikeObjectPointer(instance->m_pEntity))
	{
		RowDetail(context, "info_target %p removed; its identity is already gone", entity);
		return;
	}
	const bool marked = (instance->m_pEntity->m_flags & EF_MARKED_FOR_DELETE) != 0;
	RowDetail(context, "info_target %p, EF_MARKED_FOR_DELETE %s", entity,
		marked ? "set" : "not set");
	if (!marked)
	{
		// Suspect rather than Fail: the deletion pass may simply not have run
		// yet, and that is the benign reading until proven otherwise.
		context.Suspect("behaviour: the entity is not marked for deletion");
	}
}

void GamedataSuite::VerifySetModel(TestContext& context)
{
	// Not called: SetModel(this) is a one-argument thunk that asks a global
	// object for its default model (build 25947352: lea rax,[global];
	// mov rdi,[rax]; mov rax,[rdi]; call [rax+0x68]) and tail-calls the
	// two-argument SetModel with the answer. That object does not exist on a
	// server in this state - calling faulted reading address 0 - so the row
	// decodes the thunk and reports the global instead of walking into it.
	constexpr std::size_t kDecodeWindow = 0x20;
	const auto* fn = static_cast<const unsigned char*>(Address("CBaseModelEntity_SetModel"));
	for (std::size_t i = 0; i + 7 <= kDecodeWindow; ++i)
	{
		const bool is_lea = fn[i] == 0x48 && fn[i + 1] == 0x8D && fn[i + 2] == 0x05;
		const bool is_mov = fn[i] == 0x48 && fn[i + 1] == 0x8B && (fn[i + 2] & 0xC7) == 0x05;
		if (!is_lea && !is_mov)
		{
			continue;
		}
		std::int32_t disp = 0;
		std::memcpy(&disp, fn + i + 3, sizeof(disp));
		const unsigned char* global = fn + i + 7 + disp;
		void* object = SlotAt(global, 0);
		RowDetail(context, "decode: +0x%zx reads global %p, which holds %p%s; not called "
		                   "(the thunk dereferences it)",
			i, static_cast<const void*>(global), object,
			object == nullptr ? " (null on this server)" : "");
		return;
	}
	RowDetail(context, "decode: no rip-relative load in the first 0x%zx bytes", kDecodeWindow);
	context.Fail("decode: the thunk does not read a global; the address is not "
	             "CBaseModelEntity::SetModel(this)");
}

void GamedataSuite::VerifyCanUse(TestContext& context)
{
	// CanUse takes the weapon services (the component that owns the arsenal),
	// not the item services; the pawn's weapon-services pointer sits under
	// its own schema field. The oracles come out of the body (build 25947352):
	// the null weapon is checked before any use of it (`if (!a2) return 0`),
	// so null is a deterministic answer that must be 0 - while the crash the
	// first live run took at 0x728 was the spawned-but-never-held AK: the body
	// walks deep into the weapon's own item components (the +1536 pointer,
	// then reads at +2184 off it, and virtual slot 341), which exist only on
	// an equipped weapon. The held AK is the real question, and its answer is
	// reported rather than asserted: the body answers 0 for a weapon already
	// in the holder's list (the early exit in the handle-array walk) and runs
	// the general path otherwise - both are CanUse answers, and which one
	// fires depends on rule state this row does not control.
	std::size_t weaponservices_offset = 0;
	if (!SchemaOffsetAny(kItemServicesClasses, "m_pWeaponServices", weaponservices_offset))
	{
		context.Fail("setup: m_pWeaponServices did not resolve through the schema");
		return;
	}
	void* weapon_services = SlotAt(bot_pawn_, weaponservices_offset);
	if (!LooksLikeObjectPointer(weapon_services))
	{
		context.Fail("setup: the pawn's m_pWeaponServices is not a pointer");
		return;
	}
	const auto can_use = reinterpret_cast<CanUse_t>(Address("CCSPlayer_WeaponServices_CanUse"));

	const int null_answer = can_use(weapon_services, nullptr);
	const int held_answer = can_use(weapon_services, weapon_); // kNeedsWeapon guarantees it
	RowDetail(context, "null weapon -> %d (0 expected); held AK -> %d (0 when it is already in "
	                   "the arsenal; the general path otherwise)", null_answer, held_answer);
	if (null_answer != 0)
	{
		context.Fail("behaviour: a null weapon was usable; the address is not CanUse");
	}
}

void GamedataSuite::VerifyCanAcquire(TestContext& context)
{
	// CanAcquire(services, vdata, method, out): the vdata comes from the
	// weapon table, the method is BUY VIA PICKUP (the permissive one), and
	// the out int is where the refusal reason lands when the answer is no.
	void* vdata = LookupAk47VData();
	if (vdata == nullptr)
	{
		context.Fail("setup: GetCSWeaponDataFromKey(-1, \"%s\") returned no vdata", kAk47Key);
		return;
	}
	int reason = -1;
	const int answer = reinterpret_cast<CanAcquire_t>(
		Address("CCSPlayer_ItemServices_CanAcquire"))(item_services_, vdata, 2, &reason);
	RowDetail(context, "AK vdata %p -> %d (reason %d)", vdata, answer, reason);

	// The real function clears the out parameter unconditionally at its top
	// (build 25947352: `*a4 = 0` before any branch), so a surviving -1 means
	// the address never wrote it at all.
	if (reason == -1)
	{
		context.Fail("behaviour: the out parameter was never written; the address is not "
		             "CanAcquire");
	}
}

void GamedataSuite::VerifyWeaponData(TestContext& context)
{
	// GetCSWeaponDataFromKey(category, key) on build 25947352: the key is
	// hashed into the item table, and the category is compared with
	// vdata+8 unless it is -1. The ecosystem's old (0, "weapon_ak47") call
	// fails both ways: names are not keys, and 0 is no weapon's category.
	const auto lookup = reinterpret_cast<WeaponDataFromKey_t>(Address("GetCSWeaponDataFromKey"));

	void* vdata = lookup(kAnyWeaponCategory, kAk47Key);
	if (vdata == nullptr)
	{
		RowDetail(context, "(-1, \"%s\") -> (nil)", kAk47Key);
		context.Fail("behaviour: (-1, \"%s\") did not resolve the AK-47's vdata; every "
		             "GiveNamedItem goes through this table", kAk47Key);
		return;
	}
	if (!LooksLikeObjectPointer(vdata))
	{
		RowDetail(context, "(-1, \"%s\") -> %p", kAk47Key, vdata);
		context.Fail("behaviour: the table returned something that is not a pointer");
		return;
	}

	std::int32_t category = 0;
	std::memcpy(&category, static_cast<const unsigned char*>(vdata) + 8, sizeof(category));
	void* same = lookup(category, kAk47Key);
	void* other = lookup(category + 1, kAk47Key);
	void* absent = lookup(kAnyWeaponCategory, "cs2sdktest_no_such_weapon");

	RowDetail(context, "(-1, \"%s\") -> %p; (%d) -> %p; (%d) -> %p; unknown key -> %p",
		kAk47Key, vdata, category, same, category + 1, other, absent);
	if (same != vdata)
	{
		context.Fail("behaviour: the vdata's own category %d did not resolve it again", category);
	}
	else if (other != nullptr)
	{
		context.Fail("behaviour: category %d resolved an item of category %d; the filter "
		             "does not filter", category + 1, category);
	}
	else if (absent != nullptr)
	{
		context.Fail("behaviour: a key no build contains resolved to vdata");
	}
}

void GamedataSuite::VerifyTerminateRound(TestContext& context)
{
	// TerminateRound(gamerules, reason, _, delay): writes the round-end state
	// into the gamerules synchronously. The gamerules object has no SDK
	// accessor and no ecosystem global; the route that exists is the map's
	// CCSGameRulesProxy entity, whose m_pGameRules field points at the object
	// every map runs with.
	std::size_t gamerules_offset = 0;
	if (!SchemaOffset("CCSGameRulesProxy", "m_pGameRules", gamerules_offset))
	{
		context.Fail("setup: CCSGameRulesProxy.m_pGameRules did not resolve - %s",
			server_scope_ == nullptr
				? "the schema system has no server type scope"
				: "the class or field is not in the server scope");
		return;
	}
	// EntityInstanceByClassIter_t matches designer names ("cs_gamerules"
	// here), not C++ class names, so the proxy is found by the class-info
	// walk instead.
	CEntityInstance* proxy = FindEntityByCppClassname("CCSGameRulesProxy");
	if (proxy == nullptr)
	{
		context.Fail("setup: no CCSGameRulesProxy entity in the list");
		return;
	}
	void* gamerules = SlotAt(proxy, gamerules_offset);
	if (!LooksLikeObjectPointer(gamerules))
	{
		context.Fail("setup: the proxy's m_pGameRules is not a pointer");
		return;
	}
	// Reason 10 is RoundDraw: no team is credited, so no scoreboard state a
	// later suite might read is disturbed. Delay 0: the state lands this frame.
	reinterpret_cast<TerminateRound_t>(Address("CCSGameRules_TerminateRound"))(
		gamerules, 10, 0, 0.0f);
	RowDetail(context, "gamerules %p (via proxy %p), round terminated as a draw, returned",
		gamerules, static_cast<void*>(proxy));
}

void GamedataSuite::VerifyCreateEntityByName(TestContext& context)
{
	// Spawned so the removal below marks rather than frees; the create is
	// what this row asserts, through the identity the entity system gave it.
	void* entity = CreateScratchEntity(true);
	if (entity == nullptr)
	{
		RowDetail(context, "info_target -> (nil)");
		context.Fail("behaviour: no entity came back for info_target");
		return;
	}
	CEntityIdentity* identity = static_cast<CEntityInstance*>(entity)->m_pEntity;
	const char* designer = LooksLikeObjectPointer(identity) ? identity->GetClassname() : nullptr;
	RowDetail(context, "info_target -> entity %p (%s)", entity,
		designer != nullptr ? designer : "no identity");
	RemoveScratchEntity(entity);
	if (designer == nullptr || std::strcmp(designer, "info_target") != 0)
	{
		context.Fail("behaviour: the entity that came back is not an info_target");
	}
}

void GamedataSuite::VerifyDispatchSpawn(TestContext& context)
{
	void* entity = CreateScratchEntity(false);
	if (entity == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity to spawn");
		return;
	}
	reinterpret_cast<DispatchSpawn_t>(Address("CBaseEntity_DispatchSpawn"))(entity, nullptr);

	// The proof the pointer is what the name says: an entity the engine
	// spawned carries an identity with an index into the fixed table.
	CEntityIdentity* identity = static_cast<CEntityInstance*>(entity)->m_pEntity;
	if (!LooksLikeObjectPointer(identity))
	{
		RowDetail(context, "info_target %p spawned", entity);
		context.Fail("behaviour: the spawned object carries no identity");
		return;
	}
	const int index = identity->GetEntityIndex().Get();
	RowDetail(context, "info_target %p spawned at entity index %d", entity, index);
	RemoveScratchEntity(entity);
	if (index < 0 || index >= MAX_TOTAL_ENTITIES)
	{
		context.Fail("behaviour: index %d is outside the fixed identity table", index);
	}
}

void GamedataSuite::VerifyEmitSoundFilter(TestContext& context)
{
	// EmitSoundFilter(out, filter, entindex, params): the sound-event guid comes
	// back through `out` (the body seeds it with 0xFFFFFFFF00000000 before
	// anything else), and `out` is returned. The filter's virtuals are called
	// in the body, so it has to be a real CRecipientFilter - the SDK's own,
	// built here with no recipients: a quiet call that reaches the sound
	// system and plays to nobody.
	void* scratch = CreateScratchEntity(true);
	if (scratch == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity to "
		             "emit from");
		return;
	}
	CEntityIdentity* identity = static_cast<CEntityInstance*>(scratch)->m_pEntity;
	if (!LooksLikeObjectPointer(identity))
	{
		context.Fail("setup: the scratch entity carries no identity, so it has no index");
		return;
	}
	const unsigned entity_index = static_cast<unsigned>(identity->GetEntityIndex().Get());

	CRecipientFilter filter;

	// EmitSound_t as the SDK lays it out (shareddefs.h), defaults from its
	// constructor: pitch 100, volume 1, no forced guid.
	struct EmitParams
	{
		const char* name;
		float origin[3];
		float volume;
		float sound_time;
		int speaker_entity;
		unsigned int force_guid;
		int source_soundscape;
		short pitch;
		unsigned char flags;
	};
	EmitParams params{};
	params.name = "UI.ButtonRollout";
	params.volume = 1.0f;
	params.speaker_entity = -1;
	params.pitch = 100;
	params.flags = 1;

	unsigned char out[64] = {0};
	void* returned = reinterpret_cast<EmitSoundFilter_t>(Address("CBaseEntity_EmitSoundFilter"))(
		out, &filter, entity_index, &params);
	RemoveScratchEntity(scratch);

	std::uint64_t guid = 0;
	std::memcpy(&guid, out, sizeof(guid));
	RowDetail(context, "from info_target #%u: returned %p (out %p), guid 0x%016llx",
		entity_index, returned, static_cast<void*>(out),
		static_cast<unsigned long long>(guid));
	if (returned != out)
	{
		context.Fail("behaviour: the output buffer did not come back; the address is not "
		             "CBaseEntity_EmitSoundFilter");
	}
	else if (guid == 0)
	{
		context.Fail("behaviour: the guid slot is still zero; the body seeds it before "
		             "anything else, so it never ran");
	}
}

void GamedataSuite::VerifyAddEntityIOEvent(TestContext& context)
{
	// CEntitySystem_AddEntityIOEvent queues an input for the next frame's
	// event pump. The pump runs on the next frame boundary, so gone-now is
	// the wrong reading; the assert is that the call survives, and the queued
	// Kill is the scratch entity's own cleanup.
	CGameEntitySystem* entity_system = GameEntitySystem();
	if (entity_system == nullptr)
	{
		context.Fail("setup: GameEntitySystem() is null (IGameResourceService did not "
		             "answer); no entity system to queue through");
		return;
	}
	void* target = CreateScratchEntity(true);
	if (target == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity");
		return;
	}
	// The ecosystem's call: (entitySystem, target, input, activator, caller,
	// value, delay, unk1, unk2). `value` is never null: with unk1 null the body
	// copies the variant unconditionally (reads *(value+8) first), which is
	// what faulted at 0x8 with a null here. A zeroed variant is the void one -
	// no payload, no type - which is what an input that takes no argument
	// carries.
	unsigned char void_variant[64] = {0};
	reinterpret_cast<AddEntityIOEvent_t>(Address("CEntitySystem_AddEntityIOEvent"))(
		entity_system, target, "Kill", nullptr, nullptr, void_variant, 0.0f, nullptr, nullptr);
	RowDetail(context, "Kill queued on info_target %p through entity system %p; the next "
	                   "frame's pump fires it", target, static_cast<void*>(entity_system));
}

void GamedataSuite::VerifyLegacyListener(TestContext& context)
{
	// LegacyGameEventListener is a pure function of one global: slot <= 0x3F
	// and the table non-null -> table+16*slot+64; anything else -> null. Both
	// halves are computable without calling, so both are asserted.
	void* fn_address = Address("LegacyGameEventListener");
	const auto listener = reinterpret_cast<LegacyListener_t>(fn_address);

	// mov rax,[rip+d]; test rax,rax; jz -> the global is at +7 past the
	// function start. Reading it is the expected value's base.
	const auto* fn = static_cast<const unsigned char*>(fn_address);
	std::int32_t disp = 0;
	std::memcpy(&disp, fn + 3, sizeof(disp));
	void* table = SlotAt(fn + 7 + disp, 0);
	if (table != nullptr && !LooksLikeObjectPointer(table))
	{
		RowDetail(context, "decode: global %p holds %p",
			static_cast<const void*>(fn + 7 + disp), table);
		context.Fail("decode: the listener table global holds something that is not a pointer");
		return;
	}

	void* at_zero = listener(0);
	void* past_end = listener(0x40);
	const void* expected = (table != nullptr)
		? static_cast<const void*>(static_cast<const unsigned char*>(table) + 64) : nullptr;

	RowDetail(context, "table %p; slot 0 -> %p (expected %p); slot 0x40 -> %p (expected nil)",
		table, at_zero, expected, past_end);
	if (at_zero != expected)
	{
		context.Fail("behaviour: slot 0 did not answer table+64; the address is not "
		             "LegacyGameEventListener");
	}
	else if (past_end != nullptr)
	{
		// 0x40 is the first slot past the table: whatever the global holds,
		// the answer must be null - `ja` out, no table arithmetic.
		context.Fail("behaviour: slot 0x40 did not answer null; the bounds check is not there");
	}
}

void GamedataSuite::VerifyRemovePlayerItem(TestContext& context)
{
	// RemovePlayerItem(pawn, item): compares against the pawn's active weapon
	// and ends in UTIL_Remove(item) - the entity is marked for deletion, the
	// same flag the UTIL_Remove row asserts.
	reinterpret_cast<RemovePlayerItem_t>(Address("CBasePlayerPawn_RemovePlayerItem"))(
		bot_pawn_, weapon_);
	CEntityInstance* item = static_cast<CEntityInstance*>(weapon_);
	bool marked = false;
	if (LooksLikeObjectPointer(item->m_pEntity))
	{
		marked = (item->m_pEntity->m_flags & EF_MARKED_FOR_DELETE) != 0;
	}
	RowDetail(context, "AK %p, EF_MARKED_FOR_DELETE %s", weapon_, marked ? "set" : "not set");
	weapon_ = nullptr; // the entity is on its way out either way
	if (!marked)
	{
		context.Suspect("behaviour: the item is not marked for deletion");
	}
}

void GamedataSuite::VerifyTakeDamageOld(TestContext& context)
{
	// TakeDamageOld(pawn, info, result, double, float): a zeroed info block is
	// the shape the engine builds for environment damage. The observable is
	// the pawn's health: build the info with 1 point of damage, read health
	// before and after.
	std::size_t health_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_iHealth", health_offset))
	{
		context.Fail("setup: CBaseEntity.m_iHealth did not resolve through the schema");
		return;
	}
	// CTakeDamageInfo as the body reads it (build 25947352): damage float at
	// +68, damage type at +76 (0 is generic), the inflictor/attacker/ability
	// handles at +56..64 (0xFFFFFFFF is none), a damage position at +44, and
	// the damage flags at +112 (the schema dump's CTakeDamageInfo layout;
	// +104, where the HitGroup row's chain sits, is padding - the hit group
	// is not a schema field). The flags decide everything: bit 0x100 is
	// DFLAG_SUPPRESS_DAMAGE_MODIFICATION, and the body's own gate reads it -
	// flags without it run the modification chain (three filter checks that
	// may block the damage outright, which is how the first live run's
	// 1-point call came back health 100 -> 100), flags with it apply the
	// damage directly. The block is larger than the fields - the body writes
	// CTakeDamageInfo as the game itself builds it. The zeroed block the
	// earlier runs used is not a CTakeDamageInfo: the real constructor
	// writes a vtable pointer at +0, FLT_MAX pairs at +32/+40, 1.0f at
	// +128 and -1 handles at +232..248, and the body walks those fields -
	// both crash runs died inside exactly this difference. The row resolves
	// the constructor through its own signature and runs it; only the
	// payload is then layered on top: 25 points of damage at +68, a damage
	// position at +44, the inflictor/attacker/ability handles at +56..64,
	// and the flags at +112
	// carrying DFLAG_SUPPRESS_DAMAGE_MODIFICATION (0x100) - without that
	// bit the modification chain's filter checks block the damage before it
	// is ever applied (run one: health 100 -> 100). `result` is null, the
	// way the game's own radius-damage caller passes it; the body then uses
	// its static empty singleton. The block is 0x200: the body writes as
	// far as +272.
	// The inflictor at +56 must resolve to a live entity: the player pawn's
	// OnTakeDamage_Alive path (server+0x1538920 on build 25947352) resolves
	// it unconditionally and hits a compiler BUG() trap (mov rax, [0], the
	// "SIGSEGV at (nil)" both earlier runs died on) when it is -1. The bot
	// pawn itself is the inflictor and the attacker - self-damage, the shape
	// of fall damage; the ability at +64 stays none.
	void* constructor = Address("CTakeDamageInfo_Constructor");
	if (constructor == nullptr)
	{
		context.SkipCall("signatures.CBaseEntity_TakeDamageOld",
			"needs CTakeDamageInfo_Constructor, which did not resolve");
		return;
	}
	unsigned char info[0x200];
	std::memset(info, 0, sizeof(info));
	reinterpret_cast<void (*)(void*)>(constructor)(info);
	const float damage = 25.0f;
	const float position[3] = {1.0f, 1.0f, 1.0f};
	const std::uint32_t self = bot_pawn_->GetRefEHandle().ToInt();
	const std::uint32_t none = 0xFFFFFFFFu;
	const std::uint32_t flags = 0x100; // DFLAG_SUPPRESS_DAMAGE_MODIFICATION
	std::memcpy(info + 68, &damage, sizeof(damage));
	std::memcpy(info + 44, position, sizeof(position));
	std::memcpy(info + 112, &flags, sizeof(flags));
	std::memcpy(info + 56, &self, sizeof(self)); // inflictor
	std::memcpy(info + 60, &self, sizeof(self)); // attacker
	std::memcpy(info + 64, &none, sizeof(none)); // ability

	const auto* bytes = reinterpret_cast<const unsigned char*>(bot_pawn_);
	std::int32_t before = 0;
	std::memcpy(&before, bytes + health_offset, sizeof(before));
	reinterpret_cast<TakeDamageOld_t>(Address("CBaseEntity_TakeDamageOld"))(
		bot_pawn_, info, nullptr, 0.0, 0.0f);
	std::int32_t after = 0;
	std::memcpy(&after, bytes + health_offset, sizeof(after));

	RowDetail(context, "bot health %d -> %d (25 points, constructor-built info, null result "
	                   "as the game passes it)", before, after);
	if (after >= before)
	{
		context.Fail("behaviour: the pawn's health did not move; the address is not "
		             "TakeDamageOld");
	}
}

void GamedataSuite::VerifyTakeDamageInfoConstructor(TestContext& context)
{
	// The constructor's own row: run it on a stack block and read back the
	// template it writes - the vtable pointer at +0 (which must point into
	// the server module, the same discipline as the vtable rows), the 0xFF
	// byte at +84, the FLT_MAX pair at +32, 1.0f at +128, and the -1
	// handles at +232/+240/+248. Those are the fields TakeDamageOld walks
	// before any damage moves, so the row the other two build on.
	unsigned char info[0x200];
	std::memset(info, 0, sizeof(info));
	reinterpret_cast<void (*)(void*)>(Address("CTakeDamageInfo_Constructor"))(info);

	const Module& server = context.GetModule("server");
	const void* vtable = SlotAt(info, 0);
	const std::uint32_t flt_max_pair_lo = *reinterpret_cast<const std::uint32_t*>(info + 32);
	const std::uint32_t flt_max = 0x7F7FFFFFu;
	const std::uint32_t one_f = *reinterpret_cast<const std::uint32_t*>(info + 128);
	std::uint32_t handle_248 = 0;
	std::memcpy(&handle_248, info + 248, sizeof(handle_248));

	RowDetail(context, "vtable %p (in server: %s), +32 %s, +128 %s, +84 0x%02x, +248 0x%08x",
		vtable, InModule(server, vtable) ? "yes" : "no",
		flt_max_pair_lo == flt_max ? "FLT_MAX" : "other",
		one_f == 0x3F800000u ? "1.0f" : "other", info[84], handle_248);
	if (!InModule(server, vtable) || flt_max_pair_lo != flt_max || one_f != 0x3F800000u
	    || info[84] != 0xFF || handle_248 != 0xFFFFFFFFu)
	{
		context.Fail("behaviour: the template the call wrote is not CTakeDamageInfo's");
	}
}

void GamedataSuite::VerifyEndTouch(TestContext& context)
{
	// CBaseTrigger_EndTouch(trigger, other): the binary decrements the
	// touch-list count at trigger+3176 when the other is on the list. A
	// hand-built trigger with an empty list takes the no-op path and must
	// leave the count at zero. The zeroed buffer is safe here and only here:
	// EndTouch makes no virtual calls of its own before the count check,
	// where StartTouch fires an entity I/O output and needs a real vtable -
	// its row constructs its trigger instead (see VerifyStartTouch).
	//
	// CBaseTrigger's layout past CBaseEntity: the count at +3176 and the list
	// pointer at +3184 (build 25947352). The body reads other's identity at
	// +0x10 - the one touch it does before the count check - then returns
	// outright when the count is zero: zeroed buffers take the no-op path end
	// to end. CEntityInstance is abstract in the SDK, so `other` is a raw
	// buffer rather than an instance.
	unsigned char trigger[0xC80] = {0};
	unsigned char other_bytes[0x100] = {0};
	reinterpret_cast<EndTouch_t>(Address("CBaseTrigger_EndTouch"))(trigger, other_bytes);
	std::int32_t count = 0;
	std::memcpy(&count, trigger + 3176, sizeof(count));
	RowDetail(context, "empty touch list, count after the call %d (expected 0)", count);
	if (count != 0)
	{
		context.Fail("behaviour: the touch count moved on an empty list; the address is not "
		             "EndTouch");
	}
}

void GamedataSuite::VerifyStartTouch(TestContext& context)
{
	// StartTouch's other half, on a *constructed* trigger this time. The
	// zeroed buffer the EndTouch row gets away with is fatal here: StartTouch
	// fires the trigger's m_OnStartTouch output, and firing an entity I/O
	// output walks virtual calls on the object's own vtable - a zeroed buffer
	// has none, and the first live run died at exactly that (SIGSEGV at
	// 0x888, slot 273 off a null vtable). UTIL_CreateEntityByName runs the
	// real constructor, so the object is a real CBaseTrigger with a real
	// vtable and initialised fields; it is deliberately not DispatchSpawned,
	// which would run model and collision setup this row does not need.
	//
	// The count is read as a delta, not an absolute: a real constructor is
	// free to leave the touch list non-empty, where the zeroed buffer could
	// not. The round trip is two signatures for the price of one call each:
	// StartTouch adds a touch (count +1), EndTouch takes it back off.
	// CBaseTrigger's layout past CBaseEntity: count at +3176, list pointer at
	// +3184 (build 25947352).
	void* trigger = CreateScratchEntity(false, "trigger_multiple");
	if (trigger == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(trigger_multiple) returned no trigger");
		return;
	}
	void* other = CreateScratchEntity(true);
	if (other == nullptr)
	{
		RemoveScratchEntity(trigger);
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity to touch");
		return;
	}
	const auto read_count = [&]() {
		std::int32_t count = 0;
		std::memcpy(&count, reinterpret_cast<const unsigned char*>(trigger) + 3176,
			sizeof(count));
		return count;
	};
	const std::int32_t before = read_count();

	reinterpret_cast<StartTouch_t>(Address("CBaseTrigger_StartTouch"))(trigger, other);
	const std::int32_t touched = read_count();
	void* end_touch = Address("CBaseTrigger_EndTouch");
	if (end_touch != nullptr)
	{
		reinterpret_cast<EndTouch_t>(end_touch)(trigger, other);
	}
	const std::int32_t restored = read_count();
	RemoveScratchEntity(trigger);
	RemoveScratchEntity(other);

	RowDetail(context, "constructed trigger %p, scratch entity %p: count %d -> %d after "
	                   "StartTouch -> %d after EndTouch (expected +1 then back)", trigger, other,
		before, touched, restored);
	if (touched != before + 1)
	{
		context.Fail("behaviour: the touch count did not move by 1; the address is not "
		             "StartTouch");
	}
	else if (end_touch != nullptr && restored != before)
	{
		context.Fail("behaviour: EndTouch did not take the touch back off the list");
	}
}

void GamedataSuite::VerifyAcceptInput(TestContext& context)
{
	// AcceptInput is the pump the AddEntityIOEvent row queues onto, called
	// directly: same entity, same Kill, same zeroed void variant - but the
	// mark lands in this call instead of the next frame. The observable is
	// EF_MARKED_FOR_DELETE, the flag the UTIL_Remove row asserts too.
	void* target = CreateScratchEntity(true);
	if (target == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity");
		return;
	}
	CEntityInstance* instance = static_cast<CEntityInstance*>(target);
	if (!LooksLikeObjectPointer(instance->m_pEntity))
	{
		RemoveScratchEntity(target);
		context.Fail("setup: the scratch entity carries no identity to mark");
		return;
	}
	unsigned char void_variant[64] = {0};
	const long long result = reinterpret_cast<AcceptInput_t>(
		Address("CEntityInstance_AcceptInput"))(target, "Kill", nullptr, nullptr,
		                                        void_variant, 0, nullptr);
	const bool marked = (instance->m_pEntity->m_flags & EF_MARKED_FOR_DELETE) != 0;
	RowDetail(context, "Kill on info_target %p -> %lld, EF_MARKED_FOR_DELETE %s", target,
		result, marked ? "set" : "not set");
	if (!marked)
	{
		context.Fail("behaviour: the entity was not marked for deletion; the address is "
		             "not AcceptInput");
	}
}

void GamedataSuite::VerifyHostSay(TestContext& context)
{
	// Host_Say needs the controller only, not the pawn: it speaks through
	// the controller whatever state the pawn is in.
	if (bot_controller_ == nullptr)
	{
		context.SkipCall("signatures.Host_Say", "%s", BotMissingReason());
		return;
	}
	// Host_Say with a hand-built CCommand - the same construction the
	// engine's own command path hands it (convar.cpp's two-argument
	// constructor). The bot speaks a line in team chat (0 humans read it;
	// the say path runs regardless), and the call returning is the row's
	// evidence: the body walks argc/argv and the filter chain end to end.
	// The two unknowns stay 0/nullptr, the values the detour records on
	// live traffic.
	const char* argv[2] = {"say_team", "cs2sdktest gamedata sweep"};
	CCommand args(2, argv);
	reinterpret_cast<HostSayDirect_t>(Address("Host_Say"))(
		bot_controller_, args, true, 0, nullptr);
	RowDetail(context, "bot '%s' said \"%.63s\" in team chat (argc %d, arg0 \"%s\"), call "
	                   "returned", bot_name_.c_str(), args.Arg(1), args.ArgC(), args.Arg(0));
}

void GamedataSuite::VerifyFireOutputInternal(TestContext& context)
{
	// FireOutputInternal on an output with no connections. The binary (build
	// 25947352) reads connections at output+8, finds null, and returns its
	// first argument untouched - an address that is not this function cannot
	// reproduce that identity under seven arguments.
	struct ZeroOutput
	{
		void* vtable;
		void* connections;
		void* desc;
	};
	ZeroOutput output{nullptr, nullptr, nullptr};

	// The ecosystem's typedef, called the way its detour forwards: null
	// activator/caller, a null CVariant (the binary switches on value[8] only
	// when firing, which no connection means it never reads), zero delay.
	void* returned = reinterpret_cast<FireOutputInternal_t>(
		Address("CEntityIOOutput_FireOutputInternal"))(
		&output, nullptr, nullptr, nullptr, 0.0f, nullptr, nullptr);
	RowDetail(context, "no connections: returned %p, passed %p", returned,
		static_cast<void*>(&output));
	if (returned != &output)
	{
		context.Fail("behaviour: a zeroed CEntityIOOutput did not come back unchanged; the "
		             "address is not FireOutputInternal");
	}
}

void GamedataSuite::VerifyGameSystemList(TestContext& context)
{
	// IGameSystem_InitAllSystems_pFirst names the instruction that walks the
	// game-system linked list in InitAllSystems: mov r14,[rip+d]; test r14,r14.
	// The global at +7+disp32 must be the head of a list this server walked
	// to get where it is.
	const auto* fn = static_cast<const unsigned char*>(
		Address("IGameSystem_InitAllSystems_pFirst"));
	std::int32_t disp = 0;
	std::memcpy(&disp, fn + 3, sizeof(disp));
	void* head = SlotAt(fn + 7 + disp, 0);

	RowDetail(context, "decode: global %p holds list head %p",
		static_cast<const void*>(fn + 7 + disp), head);
	if (head == nullptr)
	{
		context.Fail("decode: the game-system list head is null on a running server; the "
		             "decoded address is not the list head");
	}
	else if (!LooksLikeObjectPointer(head))
	{
		context.Fail("decode: the list head global holds something that is not a pointer");
	}
}

void GamedataSuite::VerifyNetworkStateChanged(TestContext& context)
{
	// NetworkStateChanged ends in a call through the entity's own vtable - the
	// slot vtables.SetStateChanged names. The displacement of that load is a
	// second, independent statement of the same slot number.
	constexpr std::size_t kDecodeWindow = 0x80;

	const TomlData::Entry* entry = data_.FindPlatform("vtables.SetStateChanged",
		CS2ST_WINDOWS != 0);
	if (entry == nullptr || !entry->is_number)
	{
		context.Fail("setup: vtables.SetStateChanged is missing from the file");
		return;
	}
	const auto* fn = static_cast<const unsigned char*>(Address("NetworkStateChanged"));

	// The first mov r64, [r64+disp32] (REX.W 8B, ModRM mod=10): every field
	// read before it fits a disp8, a vtable slot past 15 does not.
	for (std::size_t i = 0; i + 7 <= kDecodeWindow; ++i)
	{
		if ((fn[i] & 0xF8) != 0x48 || fn[i + 1] != 0x8B || (fn[i + 2] & 0xC0) != 0x80
			|| (fn[i + 2] & 0x07) == 0x04)
		{
			continue;
		}
		std::int32_t disp = 0;
		std::memcpy(&disp, fn + i + 3, sizeof(disp));
		RowDetail(context, "decode: +0x%zx loads vtable +0x%x = slot %d; "
		                   "vtables.SetStateChanged %lld",
			i, static_cast<unsigned>(disp), disp / 8, entry->number);
		if (disp % 8 != 0 || disp / 8 != entry->number)
		{
			context.Fail("decode: the slot the code calls and vtables.SetStateChanged disagree");
		}
		return;
	}
	RowDetail(context, "decode: no match in the first 0x%zx bytes", kDecodeWindow);
	context.Fail("decode: no vtable load found; the address is not NetworkStateChanged");
}

void GamedataSuite::VerifyNavMesh(TestContext& context)
{
	// The function body is `cmp qword ptr [rip+disp32], 0 / setne / ret`:
	// three opcode bytes, then the disp32, then the rest of the instruction
	// (8 bytes on Windows, 7 on Linux) - which lands exactly on the navmesh
	// global. That is the ecosystem's decode, transcribed rather than
	// reinvented.
	void* fn_address = Address("CCSNavArea_IsValidNavMesh");
	const auto* fn = static_cast<const unsigned char*>(fn_address);
	std::int32_t disp = 0;
	std::memcpy(&disp, fn + 3, sizeof(disp));
	const unsigned char* slot = fn + disp + (CS2ST_WINDOWS ? 8 : 7);
	void* navmesh = SlotAt(slot, 0);

	if (navmesh != nullptr && !LooksLikeObjectPointer(navmesh))
	{
		RowDetail(context, "decode: global %p holds %p", static_cast<const void*>(slot), navmesh);
		context.Fail("decode: the navmesh global holds something that is not a pointer");
		return;
	}

	// Two independent readings of one fact: executing the stub, and reading
	// the global it compares. A signature that landed on the wrong bytes
	// cannot keep the two in agreement.
	const bool valid = reinterpret_cast<NavMeshBool_t>(fn_address)();
	RowDetail(context, "returned %s; global %p holds %p",
		valid ? "true" : "false", static_cast<const void*>(slot), navmesh);
	if ((navmesh != nullptr) != valid)
	{
		context.Fail("behaviour: the stub's answer and the global it reads disagree");
	}
}

// ---- schema-resolved offsets ---------------------------------------------

bool GamedataSuite::SchemaOffset(const char* class_name, const char* field_name,
                                 std::size_t& offset)
{
	if (server_scope_ == nullptr)
	{
		return false;
	}
	SchemaMetaInfoHandle_t<CSchemaClassInfo> handle =
		server_scope_->FindDeclaredClass(class_name);
	CSchemaClassInfo* info = handle.Get();
	if (info == nullptr || info->m_pFields == nullptr)
	{
		return false;
	}
	for (int i = 0; i < static_cast<int>(info->m_nFieldCount); ++i)
	{
		const SchemaClassFieldData_t& field = info->m_pFields[i];
		if (field.m_pszName != nullptr && std::strcmp(field.m_pszName, field_name) == 0)
		{
			offset = static_cast<std::size_t>(field.m_nSingleInheritanceOffset);
			return true;
		}
	}
	return false;
}

bool GamedataSuite::SchemaOffsetAny(const char* const* class_names,
                                    const char* field_name, std::size_t& offset)
{
	for (const char* const* name = class_names; *name != nullptr; ++name)
	{
		if (SchemaOffset(*name, field_name, offset))
		{
			return true;
		}
	}
	return false;
}

void GamedataSuite::VerifyTakeDamageHitGroup(TestContext& context, long long offset)
{
	// The ecosystem reads the hit group as a pointer chain: CSS's
	// CTakeDamageInfo.GetHitGroup walks *(info+104) -> +16 -> +56. The
	// behavioural oracle is TakeDamageOld with the chain hand-built at the
	// offset key's address: the call must walk it without faulting and apply
	// the damage - the application path is where the chain's consumers live
	// (the wound/impact pass reads chains of exactly this shape at 104 -> 16
	// -> 52/56). On this build the chain's old observable, the pawn's
	// m_LastHitGroup schema field, no longer exists - the binary's field
	// table carries m_LastHitBox where the ecosystem's dump still says
	// m_LastHitGroup - so the row proves the offset's shape rather than a
	// propagation, and reports whether any last-hit field moved.
	if (bot_pawn_ == nullptr)
	{
		context.SkipCall("offsets.CTakeDamageInfo_HitGroup", "%s | not run",
			BotMissingReason());
		return;
	}
	// m_flDamage is not read through the schema: CTakeDamageInfo is not a
	// server-scope schema class on this build (the first live run failed the
	// row at exactly this lookup), and TakeDamageOld's own row has already
	// documented the info block's layout out of the binary - damage float at
	// +68. The health observable is a real entity field and goes through the
	// schema. m_LastHitBox is read the same way when the build carries it,
	// and its move is reported; its declaring class (CCSPlayerPawnBase in
	// the current dump) is inside the fallback list.
	std::size_t health_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_iHealth", health_offset))
	{
		context.Fail("setup: m_iHealth did not resolve through the schema");
		return;
	}
	std::size_t lasthit_offset = 0;
	const bool has_lasthit = SchemaOffsetAny(kLastHitGroupClasses, "m_LastHitBox",
		lasthit_offset);
	void* take_damage = Address("CBaseEntity_TakeDamageOld");
	if (take_damage == nullptr)
	{
		context.SkipCall("offsets.CTakeDamageInfo_HitGroup",
			"needs CBaseEntity_TakeDamageOld, which did not resolve");
		return;
	}

	// The info block is constructor-built like the TakeDamageOld row's (a
	// zeroed block is not a CTakeDamageInfo; the constructor writes the
	// vtable and the FLT_MAX/-1 template the body walks), plus the hit-group
	// chain at the offset key's address: a heap-shaped end object whose +56
	// carries the chosen hit group, reached through the +16 link. Hit group
	// 1 is HEAD. The damage flags carry DFLAG_SUPPRESS_DAMAGE_MODIFICATION
	// and the result is null, exactly the TakeDamageOld row's shape.
	void* constructor = Address("CTakeDamageInfo_Constructor");
	if (constructor == nullptr)
	{
		context.SkipCall("offsets.CTakeDamageInfo_HitGroup",
			"needs CTakeDamageInfo_Constructor, which did not resolve");
		return;
	}
	unsigned char head_object[0x80] = {0};
	const std::int32_t head = 1;
	std::memcpy(head_object + 56, &head, sizeof(head));

	unsigned char info[0x200];
	std::memset(info, 0, sizeof(info));
	reinterpret_cast<void (*)(void*)>(constructor)(info);
	// The inflictor and attacker are the bot pawn itself, as in the
	// TakeDamageOld row: a -1 inflictor trips the BUG() trap on the pawn's
	// OnTakeDamage_Alive path.
	const float damage = 25.0f;
	const float position[3] = {1.0f, 1.0f, 1.0f};
	const std::uint32_t self = bot_pawn_->GetRefEHandle().ToInt();
	const std::uint32_t none = 0xFFFFFFFFu;
	const std::uint32_t flags = 0x100; // DFLAG_SUPPRESS_DAMAGE_MODIFICATION
	std::memcpy(info + 68, &damage, sizeof(damage));
	std::memcpy(info + 44, position, sizeof(position));
	std::memcpy(info + 112, &flags, sizeof(flags));
	std::memcpy(info + 56, &self, sizeof(self)); // inflictor
	std::memcpy(info + 60, &self, sizeof(self)); // attacker
	std::memcpy(info + 64, &none, sizeof(none)); // ability
	// The chain: *(info+offset) = &chain; *(chain+16) = &head_object.
	unsigned char chain[0x40] = {0};
	void* link = head_object;
	std::memcpy(chain + 16, &link, sizeof(link));
	void* chain_ptr = chain;
	std::memcpy(info + offset, &chain_ptr, sizeof(chain_ptr));

	const auto* pawn_bytes = reinterpret_cast<const unsigned char*>(bot_pawn_);
	std::int32_t health_before = 0;
	std::memcpy(&health_before, pawn_bytes + health_offset, sizeof(health_before));
	std::int32_t hit_before = 0;
	if (has_lasthit)
	{
		std::memcpy(&hit_before, pawn_bytes + lasthit_offset, sizeof(hit_before));
	}
	reinterpret_cast<TakeDamageOld_t>(take_damage)(bot_pawn_, info, nullptr, 0.0, 0.0f);
	std::int32_t hit_after = 0;
	if (has_lasthit)
	{
		std::memcpy(&hit_after, pawn_bytes + lasthit_offset, sizeof(hit_after));
	}
	std::int32_t health_after = 0;
	std::memcpy(&health_after, pawn_bytes + health_offset, sizeof(health_after));

	std::string lasthit_text = "(not in this build's schema)";
	if (has_lasthit)
	{
		lasthit_text = std::to_string(hit_before) + " -> " + std::to_string(hit_after);
	}
	RowDetail(context, "chain *(info+%lld) -> +16 -> +56 = hitgroup %d; health %d -> %d; "
	                   "m_LastHitBox %s", offset, head, health_before, health_after,
		lasthit_text.c_str());
	// The hard assertion is the survival of the walk plus the applied damage:
	// a wrong offset dereferences garbage and faults inside the guard, and
	// the flags-bypassed application path is the only thing left to move the
	// health. Whether the chain's hit group propagates into any last-hit
	// field is reported, not asserted - the propagation runs through more
	// state than the offset alone names.
	if (health_after >= health_before)
	{
		context.Fail("behaviour: the pawn's health did not move; the chain may not have "
		             "been walked");
	}
	if (has_lasthit && hit_after != hit_before)
	{
		context.Detail("a last-hit field moved on the damage");
	}
}

bool GamedataSuite::FindBotPair(CEntityInstance*& controller, CEntityInstance*& pawn,
                                std::string& name)
{
	controller = nullptr;
	pawn = nullptr;
	name.clear();

	CGameEntitySystem* entity_system = GameEntitySystem();
	if (entity_system == nullptr)
	{
		return false;
	}

	// Every field read below goes through the schema first, so a layout
	// change skips the bot rows instead of misreading them.
	std::size_t steamid_offset = 0;
	std::size_t hpawn_offset = 0;
	std::size_t name_offset = 0;
	if (!SchemaOffset("CBasePlayerController", "m_steamID", steamid_offset)
		|| !SchemaOffset("CBasePlayerController", "m_hPawn", hpawn_offset)
		|| !SchemaOffset("CBasePlayerController", "m_iszPlayerName", name_offset))
	{
		return false;
	}

	// Walk the active-entity chain (the same walk GameEntitySystemSuite uses)
	// for controllers by C++ class name - every controller's designer name is
	// cs_player_controller, bot or not - and keep the first fake client
	// (m_steamID 0). One with a pawn is preferred over one without (SourceTV
	// is a fake client that never gets one).
	CEntityIdentity* scanned = entity_system->m_EntityList.m_pFirstActiveEntity;
	for (int walked = 0; scanned != nullptr && walked < 4096;
	     scanned = scanned->m_pNext, ++walked)
	{
		const char* cpp_name = (scanned->m_pClass != nullptr &&
		                       scanned->m_pClass->m_pClassInfo != nullptr)
			? scanned->m_pClass->m_pClassInfo->m_pszCPPClassname
			: nullptr;
		if (cpp_name == nullptr || std::strcmp(cpp_name, "CCSPlayerController") != 0
			|| (scanned->m_flags & EF_MARKED_FOR_DELETE) != 0)
		{
			continue;
		}
		CEntityInstance* candidate = scanned->m_pInstance;
		if (!LooksLikeObjectPointer(candidate))
		{
			continue;
		}
		const auto* bytes = reinterpret_cast<const unsigned char*>(candidate);

		std::uint64_t steam_id = 0;
		std::memcpy(&steam_id, bytes + steamid_offset, sizeof(steam_id));
		if (steam_id != 0)
		{
			continue;
		}

		std::uint32_t handle_bits = 0;
		std::memcpy(&handle_bits, bytes + hpawn_offset, sizeof(handle_bits));
		// A bot that has not spawned yet has no pawn: the handle is invalid
		// and the lookup answers null. That is "found, pawn pending", not
		// "no bot"; PrepareBot skips the rows with that reason.
		CEntityInstance* resolved = (handle_bits == 0xFFFFFFFFu)
			? nullptr
			: entity_system->GetEntityInstance(CEntityHandle{handle_bits});
		if (resolved != nullptr && !LooksLikeObjectPointer(resolved))
		{
			resolved = nullptr;
		}

		// m_iszPlayerName is a fixed char[128]; copied bounded, and quotes
		// dropped so the name prints cleanly in the row.
		char player_name[128] = {0};
		std::memcpy(player_name, bytes + name_offset, sizeof(player_name) - 1);
		std::string candidate_name;
		for (const char* c = player_name; *c != '\0'; ++c)
		{
			if (*c != '"')
			{
				candidate_name.push_back(*c);
			}
		}
		if (candidate_name.empty())
		{
			continue;
		}

		if (controller == nullptr || (pawn == nullptr && resolved != nullptr))
		{
			controller = candidate;
			pawn = resolved;
			name = candidate_name;
		}
		if (pawn != nullptr)
		{
			break; // a spawned bot is the best there is
		}
	}
	return controller != nullptr;
}

CEntityInstance* GamedataSuite::FindEntityByCppClassname(const char* cpp_class_name)
{
	CGameEntitySystem* entity_system = GameEntitySystem();
	if (entity_system == nullptr)
	{
		return nullptr;
	}
	CEntityIdentity* scanned = entity_system->m_EntityList.m_pFirstActiveEntity;
	for (int walked = 0; scanned != nullptr && walked < 4096;
	     scanned = scanned->m_pNext, ++walked)
	{
		const char* cpp_name = (scanned->m_pClass != nullptr &&
		                       scanned->m_pClass->m_pClassInfo != nullptr)
			? scanned->m_pClass->m_pClassInfo->m_pszCPPClassname
			: nullptr;
		if (cpp_name != nullptr && std::strcmp(cpp_name, cpp_class_name) == 0
			&& (scanned->m_flags & EF_MARKED_FOR_DELETE) == 0)
		{
			return scanned->m_pInstance;
		}
	}
	return nullptr;
}

// -----------------------------------------------------------------------------
//  vtable slots - one row per key, in file order
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseVtableSlots(TestContext& context)
{
	context.Note("[vtables] slot indexes, one row per key (two compared against "
	             "KHook::GetVtableIndex, ten called for behaviour)");

	for (const std::string& table : data_.tables())
	{
		if (table.rfind(kVtablesPrefix, 0) != 0)
		{
			continue;
		}
		const std::string name = table.substr(std::strlen(kVtablesPrefix));

		CS2ST_CALL(context, table, {
			const TomlData::Entry* entry = data_.FindPlatform(table, CS2ST_WINDOWS != 0);
			if (entry == nullptr || !entry->is_number)
			{
				context.Fail("the key has no %s number in the file",
					CS2ST_WINDOWS ? "windows" : "linux");
				return;
			}

			// The two keys an SDK declaration can also compute: two
			// independent sources, one number. Which one is wrong is a
			// question for the binary; that they disagree is the finding.
			std::int32_t sdk_index = -1;
			bool sdk_declared = false;
			if (table == kCheckTransmitTable)
			{
				sdk_index = KHook::GetVtableIndex(&ISource2GameEntities::CheckTransmit);
				sdk_declared = true;
			}
			else if (table == kAddResourceTable)
			{
				// The ecosystem's number is the one-argument AddResource. The
				// SDK declares three overloads: MSVC groups them by name in
				// reverse declaration order (single-argument last, slot 2),
				// Itanium keeps declaration order (slot 0) - which is exactly
				// the 2/0 the file carries, arrived at by the compiler rather
				// than copied.
				const auto single_arg =
					static_cast<void (IEntityResourceManifest::*)(const char*)>(
						&IEntityResourceManifest::AddResource);
				sdk_index = KHook::GetVtableIndex(single_arg);
				sdk_declared = true;
			}

			if (sdk_declared)
			{
				context.Detail("gamedata %lld, GetVtableIndex %d", entry->number,
					static_cast<int>(sdk_index));
				if (sdk_index < 0)
				{
					context.Fail("the SDK member pointer did not decode to a slot");
				}
				else if (static_cast<long long>(sdk_index) != entry->number)
				{
					context.Fail("the SDK declaration and the gamedata disagree about this slot");
				}
			}
			else if (name == "GameEventManager")
			{
				VerifyGameEventManagerSlot(context, *entry);
			}
			else if (name == "CServerSideClient")
			{
				context.Detail("%lld (RTTI offset-to-top discriminator, not a slot)",
					entry->number);
			}
			else if (name == "SetStateChanged")
			{
				context.Detail("slot %lld (cross-checked in the NetworkStateChanged row)",
					entry->number);
			}
			else
			{
				// The slots no SDK declaration can compute: called, the way
				// CounterStrikeSharp's own wrappers call them in production.
				// VtableSlotInServer bounds-checks the pointer first, so a
				// stale slot number fails the row before anything jumps.
				// Each check names its own prerequisites (the controller, the
				// pawn, the item services) so a row that needs no bot - like
				// Teleport, on a scratch entity - runs without one.
				const VerifierSlot verify_slot = FindSlotCheck(name);
				if (verify_slot == nullptr)
				{
					context.Detail("slot %lld (reported; no behaviour check is written for "
					               "this key)", entry->number);
					return;
				}
				(this->*verify_slot)(context, table, entry->number);
			}
		});
	}
}

void GamedataSuite::VerifyGameEventManagerSlot(TestContext& context, const TomlData::Entry& entry)
{
	// The ecosystem calls this slot on ISource2Server and subtracts 8 from what
	// comes back to reach IGameEventManager2. GameEventSuite records why this
	// project will not blind-call server vtable slots, so the slot is read, its
	// code decoded, and only an accessor-shaped body is ever called.
	if (entry.number < 0)
	{
		context.Fail("the slot number is negative");
		return;
	}
	if (server_ == nullptr)
	{
		context.Fail("server does not answer %s; there is no object to read the slot from",
			INTERFACEVERSION_SERVERGAMEDLL);
		return;
	}
	const Module& server = context.GetModule("server");
	if (!server.IsValid())
	{
		context.Fail("the server module is not loaded");
		return;
	}

	const auto* const* vtable = *static_cast<const unsigned char* const* const*>(server_);
	const unsigned char* fn = vtable[entry.number];
	context.Detail("slot %lld -> %p", entry.number, static_cast<const void*>(fn));
	if (!InModule(server, fn))
	{
		context.Fail("the slot points outside %s; the slot index is past the table",
			server.name().c_str());
		return;
	}
	if (fn[0] == 0xC3)
	{
		// The finding this row exists for: calling an empty stub returns
		// whatever rax happened to hold, and "minus 8" turns that into a
		// manager pointer every listener registration then dereferences.
		context.Fail("slot %lld is an empty 'ret' stub on this build; the ecosystem's "
		             "call returns an undefined register, not the manager", entry.number);
		return;
	}

	// The accessor shape: lea rax,[rip+d] / mov rax,[rip+d], then ret.
	const bool is_lea = fn[0] == 0x48 && fn[1] == 0x8D && fn[2] == 0x05;
	const bool is_mov = fn[0] == 0x48 && fn[1] == 0x8B && fn[2] == 0x05;
	if ((!is_lea && !is_mov) || fn[7] != 0xC3)
	{
		context.Fail("slot %lld does not have an accessor's shape (starts %02x %02x %02x); "
		             "not calling it", entry.number, fn[0], fn[1], fn[2]);
		return;
	}
	std::int32_t disp = 0;
	std::memcpy(&disp, fn + 3, sizeof(disp));
	const unsigned char* global = fn + 7 + disp;
	void* decoded = is_lea ? const_cast<unsigned char*>(global) : SlotAt(global, 0);

	using Accessor_t = void* (*)(void*);
	void* returned = reinterpret_cast<Accessor_t>(const_cast<unsigned char*>(fn))(server_);
	context.Detail("slot %lld returned %p, decoded %p", entry.number, returned, decoded);
	if (returned != decoded)
	{
		context.Fail("the slot's return value and its decoded global disagree");
		return;
	}

	void* manager = static_cast<unsigned char*>(returned) - 8;
	if (!LooksLikeObjectPointer(manager) || !InModule(server, SlotAt(manager, 0)))
	{
		context.Fail("result - 8 (%p) does not carry a vtable inside %s; it is not an "
		             "IGameEventManager2", manager, server.name().c_str());
		return;
	}
	context.Detail("slot %lld -> manager %p, vtable %p", entry.number, manager,
		SlotAt(manager, 0));
}

// ---- behavioural vtable checks --------------------------------------------
//
// One table, the same shape FindCheck gives the signature rows: every key
// whose slot is called, mapped to its verifier. The call shapes are
// CounterStrikeSharp's, transcribed from its wrappers (CCSPlayerController.cs,
// CBasePlayerPawn.cs, CBaseEntity.cs, CCSPlayer_ItemServices.cs,
// CGameSceneNode.cs, dump.h) - those wrappers are the ecosystem's production
// use of these slots, so matching them is calling the slot the way its
// consumers do.

GamedataSuite::VerifierSlot GamedataSuite::FindSlotCheck(const std::string& name)
{
	static const struct
	{
		const char* name;
		VerifierSlot verify;
	} kSlotChecks[] = {
		{"CCSPlayerController_ChangeTeam", &GamedataSuite::VerifyChangeTeamSlot},
		{"CCSPlayerController_Respawn", &GamedataSuite::VerifyRespawnSlot},
		{"CCSPlayer_ItemServices_GiveNamedItem", &GamedataSuite::VerifyGiveNamedItemSlot},
		{"CCSPlayer_ItemServices_DropActivePlayerWeapon",
		 &GamedataSuite::VerifyDropActiveWeaponSlot},
		{"CCSPlayer_ItemServices_RemoveWeapons", &GamedataSuite::VerifyRemoveWeaponsSlot},
		{"CGameSceneNode_GetSkeletonInstance", &GamedataSuite::VerifyGetSkeletonInstanceSlot},
		{"CCSGameRules_FindPickerEntity", &GamedataSuite::VerifyFindPickerEntitySlot},
		{"CBaseEntity_IsPlayerPawn", &GamedataSuite::VerifyIsPlayerPawnSlot},
		{"CBasePlayerPawn_CommitSuicide", &GamedataSuite::VerifyCommitSuicideSlot},
		{"CBaseEntity_Teleport", &GamedataSuite::VerifyTeleportSlot},
	};

	for (const auto& check : kSlotChecks)
	{
		if (name == check.name)
		{
			return check.verify;
		}
	}
	return nullptr;
}

const char* GamedataSuite::BotMissingReason() const
{
	return bot_skip_reason_.empty() ? "needs a bot, and none is in the server"
	                                : bot_skip_reason_.c_str();
}

void* GamedataSuite::VtableSlotInServer(TestContext& context, const void* object,
                                        long long slot)
{
	const Module& server = context.GetModule("server");
	if (slot < 0)
	{
		context.Fail("setup: slot %lld is negative", slot);
		return nullptr;
	}
	const void* vtable = SlotAt(object, 0);
	if (!InModule(server, vtable))
	{
		context.Fail("setup: the object's vtable is not inside %s; it is not a server "
		             "object", server.name().c_str());
		return nullptr;
	}
	void* fn = SlotAt(vtable, static_cast<std::size_t>(slot) * sizeof(void*));
	if (!InModule(server, fn))
	{
		context.Fail("behaviour: slot %lld points outside %s; the slot index is past the "
		             "table", slot, server.name().c_str());
		return nullptr;
	}
	// The row's location: where the slot's function landed, so a RowDetail
	// under a vtable row names the function it called rather than whatever
	// signature row wrote last.
	char location[64];
	std::snprintf(location, sizeof(location), "server+0x%zx",
		static_cast<std::size_t>(static_cast<const unsigned char*>(fn)
			- static_cast<const unsigned char*>(server.base())));
	row_location_ = location;
	return fn;
}

void GamedataSuite::VerifyChangeTeamSlot(TestContext& context, const std::string& table,
                                         long long slot)
{
	// CSS's ForceChangeTeam: (controller, team) - "the player will remain
	// alive and keep their weapons". The SwitchTeam signature row is the
	// template: m_iTeamNum is both the input and the assertion, and the team
	// byte is put back where the round started the bot.
	if (bot_controller_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	std::size_t team_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_iTeamNum", team_offset))
	{
		context.Fail("setup: CBaseEntity.m_iTeamNum did not resolve through the schema");
		return;
	}
	void* fn = VtableSlotInServer(context, bot_controller_, slot);
	if (fn == nullptr)
	{
		return;
	}
	const auto change_team = reinterpret_cast<SwitchTeam_t>(fn);

	auto* bytes = reinterpret_cast<unsigned char*>(bot_controller_);
	const std::uint8_t before = bytes[team_offset];
	const std::uint8_t target = (before == 3) ? 2 : 3;
	change_team(bot_controller_, target);
	const std::uint8_t after = bytes[team_offset];
	if (after != before && (before == 2 || before == 3))
	{
		change_team(bot_controller_, before);
	}
	const std::uint8_t restored = bytes[team_offset];

	RowDetail(context, "controller team %u -> %u (asked %u) -> restored %u",
		before, after, target, restored);
	if (after != target)
	{
		context.Fail("behaviour: m_iTeamNum did not follow the call; the slot is not "
		             "ChangeTeam");
	}
}

void GamedataSuite::VerifyRespawnSlot(TestContext& context, const std::string& table,
                                      long long slot)
{
	// CSS's Respawn: SetPawn first ("the Call To Arms update appears to have
	// invalidated the need for CCSPlayerPawn_Respawn", its comment), then the
	// slot. The oracle is the pawn handle and health: the respawn must leave
	// the controller holding a pawn whose health reads positive.
	if (bot_controller_ == nullptr || bot_pawn_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	std::size_t health_offset = 0;
	std::size_t pawn_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_iHealth", health_offset)
	    || !SchemaOffset("CBasePlayerController", "m_hPawn", pawn_offset))
	{
		context.Fail("setup: m_iHealth or m_hPawn did not resolve through the schema");
		return;
	}
	void* fn = VtableSlotInServer(context, bot_controller_, slot);
	if (fn == nullptr)
	{
		return;
	}
	const auto respawn = reinterpret_cast<void (*)(void*)>(fn);
	const auto set_pawn = reinterpret_cast<SetPawn_t>(Address("CBasePlayerController_SetPawn"));
	if (set_pawn == nullptr)
	{
		context.SkipCall(table, "slot %lld | needs CBasePlayerController_SetPawn, which did "
		                        "not resolve", slot);
		return;
	}

	// CSS's exact sequence: hand the controller its own pawn back through
	// SetPawn (its Respawn wrapper's first line), then the slot. The oracle
	// is the pawn handle and the pawn's health: a respawned bot holds a
	// pawn whose health reads positive.
	std::uint32_t original = 0;
	std::memcpy(&original, reinterpret_cast<const unsigned char*>(bot_controller_)
		+ pawn_offset, sizeof(original));
	set_pawn(bot_controller_, bot_pawn_, false, false, false, false);
	respawn(bot_controller_);

	std::uint32_t after = 0;
	std::memcpy(&after, reinterpret_cast<const unsigned char*>(bot_controller_)
		+ pawn_offset, sizeof(after));
	std::int32_t health = 0;
	std::memcpy(&health, reinterpret_cast<const unsigned char*>(bot_pawn_) + health_offset,
		sizeof(health));

	RowDetail(context, "m_hPawn 0x%08x before, 0x%08x after, pawn health %d", original, after,
		health);
	if (after == 0xFFFFFFFFu)
	{
		context.Suspect("behaviour: the controller has no pawn after the respawn");
	}
	else if (health <= 0)
	{
		context.Suspect("behaviour: the pawn's health is not positive after the respawn");
	}
}

void GamedataSuite::VerifyGiveNamedItemSlot(TestContext& context, const std::string& table,
                                            long long slot)
{
	// CSS's GiveNamedItem<T>: (itemServices, item) through the slot, the
	// two-argument shape the signature row's eight-argument direct call
	// forwards to internally. The AK again: the entity that comes back must
	// carry the weapon_ak47 identity, and the weapon stays in the bot's
	// arsenal for the DropActivePlayerWeapon row.
	if (item_services_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | the bot pawn's m_pItemServices did not resolve",
			slot);
		return;
	}
	void* fn = VtableSlotInServer(context, item_services_, slot);
	if (fn == nullptr)
	{
		return;
	}
	void* weapon = reinterpret_cast<void* (*)(void*, const char*)>(fn)(
		item_services_, "weapon_ak47");
	if (weapon == nullptr)
	{
		RowDetail(context, "weapon_ak47 -> (nil); buy or round rules may have refused");
		context.Suspect("behaviour: no entity came back through the slot");
		return;
	}
	CEntityIdentity* identity = static_cast<CEntityInstance*>(weapon)->m_pEntity;
	const char* designer = LooksLikeObjectPointer(identity) ? identity->GetClassname() : nullptr;
	RowDetail(context, "weapon_ak47 -> entity %p (%s)", weapon,
		designer != nullptr ? designer : "no identity");
	if (designer == nullptr || std::strcmp(designer, "weapon_ak47") != 0)
	{
		context.Fail("behaviour: the entity that came back is not a weapon_ak47; the slot "
		             "is not GiveNamedItem");
		return;
	}
}

void GamedataSuite::VerifyDropActiveWeaponSlot(TestContext& context, const std::string& table,
                                               long long slot)
{
	// CSS's DropActivePlayerWeapon: (itemServices, activeWeapon, dropMomentum)
	// - the slot is taken on the *item services* object, and the weapon passed
	// is the pawn's current m_hActiveWeapon resolved to an entity, never a
	// weapon chosen out of band (the first run dropped an AK that was never
	// equipped, and the handle rightly did not move). The third argument was
	// once passed uninitialized (CSS #1407); the fix passes a zeroed Vector,
	// so one rides with it here. The observable is the active-weapon handle:
	// what was held is dropped.
	if (bot_pawn_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	if (item_services_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | the bot pawn's m_pItemServices did not resolve",
			slot);
		return;
	}
	std::size_t weaponservices_offset = 0;
	if (!SchemaOffsetAny(kItemServicesClasses, "m_pWeaponServices", weaponservices_offset))
	{
		context.Fail("setup: m_pWeaponServices did not resolve through the schema");
		return;
	}
	void* weapon_services = SlotAt(bot_pawn_, weaponservices_offset);
	if (!LooksLikeObjectPointer(weapon_services))
	{
		context.Fail("setup: the pawn's m_pWeaponServices is not a pointer");
		return;
	}
	std::size_t active_offset = 0;
	if (!SchemaOffset("CPlayer_WeaponServices", "m_hActiveWeapon", active_offset))
	{
		context.Fail("setup: CPlayer_WeaponServices.m_hActiveWeapon did not resolve through "
		             "the schema");
		return;
	}
	const auto read_active = [&]() {
		std::uint32_t bits = 0;
		std::memcpy(&bits, reinterpret_cast<const unsigned char*>(weapon_services)
			+ active_offset, sizeof(bits));
		return bits;
	};
	const std::uint32_t before = read_active();
	if (before == 0xFFFFFFFFu)
	{
		context.SkipCall(table, "slot %lld | the bot holds no active weapon; nothing to drop",
			slot);
		return;
	}
	CGameEntitySystem* entity_system = GameEntitySystem();
	void* active = (entity_system != nullptr)
		? entity_system->GetEntityInstance(CEntityHandle{before})
		: nullptr;
	if (!LooksLikeObjectPointer(active))
	{
		context.SkipCall(table, "slot %lld | m_hActiveWeapon 0x%08x did not resolve to an "
		                        "entity", slot, before);
		return;
	}
	void* fn = VtableSlotInServer(context, item_services_, slot);
	if (fn == nullptr)
	{
		return;
	}
	// A zeroed Vector as the drop momentum - the #1407 fix's own shape, in
	// case the body reads it.
	const float zero_momentum[3] = {0.0f, 0.0f, 0.0f};
	reinterpret_cast<void (*)(void*, void*, const void*)>(fn)(item_services_, active,
	                                                          zero_momentum);
	const std::uint32_t after = read_active();

	RowDetail(context, "active weapon %p (handle 0x%08x) dropped through the item services, "
	                   "m_hActiveWeapon -> 0x%08x", active, before, after);
	if (after == before)
	{
		context.Suspect("behaviour: the active weapon handle did not move on the drop");
	}
}

void GamedataSuite::VerifyRemoveWeaponsSlot(TestContext& context, const std::string& table,
                                            long long slot)
{
	// CSS's RemoveWeapons(): (itemServices) - every weapon goes. The oracle
	// is the active-weapon handle the DropActivePlayerWeapon row reads: a
	// held weapon's handle is valid (not 0xFFFFFFFF), and after the strip it
	// must not be. The bot enters the next round bare, which the next
	// respawn refills.
	if (bot_pawn_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	if (item_services_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | the bot pawn's m_pItemServices did not resolve",
			slot);
		return;
	}
	std::size_t weaponservices_offset = 0;
	std::size_t active_offset = 0;
	if (!SchemaOffsetAny(kItemServicesClasses, "m_pWeaponServices", weaponservices_offset)
	    || !SchemaOffset("CPlayer_WeaponServices", "m_hActiveWeapon", active_offset))
	{
		context.Fail("setup: m_pWeaponServices or m_hActiveWeapon did not resolve through "
		             "the schema");
		return;
	}
	void* weapon_services = SlotAt(bot_pawn_, weaponservices_offset);
	if (!LooksLikeObjectPointer(weapon_services))
	{
		context.Fail("setup: the pawn's m_pWeaponServices is not a pointer");
		return;
	}
	void* fn = VtableSlotInServer(context, item_services_, slot);
	if (fn == nullptr)
	{
		return;
	}
	const auto read_active = [&]() {
		std::uint32_t bits = 0;
		std::memcpy(&bits, reinterpret_cast<const unsigned char*>(weapon_services)
			+ active_offset, sizeof(bits));
		return bits;
	};

	const std::uint32_t before = read_active();
	reinterpret_cast<void (*)(void*)>(fn)(item_services_);
	const std::uint32_t after = read_active();

	RowDetail(context, "m_hActiveWeapon 0x%08x -> 0x%08x", before, after);
	if (before != 0xFFFFFFFFu && after != 0xFFFFFFFFu)
	{
		context.Fail("behaviour: a held weapon survived the strip; the slot is not "
		             "RemoveWeapons");
	}
	else if (before == 0xFFFFFFFFu)
	{
		context.Detail("the bot held no active weapon to remove; handle still 0x%08x", after);
	}
}

void GamedataSuite::VerifyGetSkeletonInstanceSlot(TestContext& context, const std::string& table,
                                                  long long slot)
{
	// CSS's CGameSceneNode.SkeletonInstance: (node) -> CSkeletonInstance.
	// The pawn's node comes through the body component the schema declares
	// (CBodyComponent.m_pSceneNode), and the skeleton that comes back must be
	// a heap object: pointer-shaped, with a vtable inside the server module.
	if (bot_pawn_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	std::size_t body_offset = 0;
	std::size_t node_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_CBodyComponent", body_offset)
	    || !SchemaOffset("CBodyComponent", "m_pSceneNode", node_offset))
	{
		context.Fail("setup: m_CBodyComponent or CBodyComponent.m_pSceneNode did not "
		             "resolve through the schema");
		return;
	}
	void* body = SlotAt(bot_pawn_, body_offset);
	if (!LooksLikeObjectPointer(body))
	{
		context.Fail("setup: the pawn's m_CBodyComponent is not a pointer");
		return;
	}
	void* node = SlotAt(body, node_offset);
	if (!LooksLikeObjectPointer(node))
	{
		context.Fail("setup: the body component's m_pSceneNode is not a pointer");
		return;
	}
	void* fn = VtableSlotInServer(context, node, slot);
	if (fn == nullptr)
	{
		return;
	}
	void* skeleton = reinterpret_cast<void* (*)(void*)>(fn)(node);
	const Module& server = context.GetModule("server");
	const bool shaped = LooksLikeObjectPointer(skeleton) && InModule(server, SlotAt(skeleton, 0));
	RowDetail(context, "node %p -> skeleton %p%s", node, skeleton,
		shaped ? "" : " (not object-shaped)");
	if (!shaped)
	{
		context.Fail("behaviour: the slot did not return a skeleton object; the slot is not "
		             "GetSkeletonInstance");
	}
}

void GamedataSuite::VerifyFindPickerEntitySlot(TestContext& context, const std::string& table,
                                               long long slot)
{
	// CSS's FindPickerEntity: (gamerules, controller, filter, x, y) -> the
	// entity under the crosshair. On a dedicated server there is no view to
	// pick through, so the honest oracle is the call's own shape: CSS calls
	// it with a null filter and screen-centre coordinates, and the answer on
	// a dedicated server is a null or worldspawn - never garbage. The
	// gamerules object is the TerminateRound row's, through the proxy.
	if (bot_controller_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	std::size_t gamerules_offset = 0;
	if (!SchemaOffset("CCSGameRulesProxy", "m_pGameRules", gamerules_offset))
	{
		context.Fail("setup: CCSGameRulesProxy.m_pGameRules did not resolve through the "
		             "schema");
		return;
	}
	CEntityInstance* proxy = FindEntityByCppClassname("CCSGameRulesProxy");
	if (proxy == nullptr)
	{
		context.Fail("setup: no CCSGameRulesProxy entity in the list");
		return;
	}
	void* gamerules = SlotAt(proxy, gamerules_offset);
	if (!LooksLikeObjectPointer(gamerules))
	{
		context.Fail("setup: the proxy's m_pGameRules is not a pointer");
		return;
	}
	void* fn = VtableSlotInServer(context, gamerules, slot);
	if (fn == nullptr)
	{
		return;
	}
	void* picked = reinterpret_cast<void* (*)(void*, void*, void*, double, double)>(fn)(
		gamerules, bot_controller_, nullptr, 0.0, 0.0);
	const char* designer = nullptr;
	if (LooksLikeObjectPointer(picked))
	{
		CEntityIdentity* identity = static_cast<CEntityInstance*>(picked)->m_pEntity;
		designer = LooksLikeObjectPointer(identity) ? identity->GetClassname() : nullptr;
	}
	RowDetail(context, "bot's crosshair -> %p (%s)", picked,
		designer != nullptr ? designer : "nil");
	if (picked != nullptr && !LooksLikeObjectPointer(picked))
	{
		context.Fail("behaviour: the slot returned something that is not a pointer; the slot "
		             "is not FindPickerEntity");
	}
}

void GamedataSuite::VerifyIsPlayerPawnSlot(TestContext& context, const std::string& table,
                                           long long slot)
{
	// CSS's dump.h: CALL_VIRTUAL(bool, offset, this) - the cheapest row in
	// the file. A bot pawn must answer true and a spawned info_target false;
	// any other pair of answers means the slot is not IsPlayerPawn.
	if (bot_pawn_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	void* scratch = CreateScratchEntity(true);
	if (scratch == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity");
		return;
	}
	void* pawn_fn = VtableSlotInServer(context, bot_pawn_, slot);
	void* scratch_fn = (pawn_fn != nullptr)
		? VtableSlotInServer(context, scratch, slot) : nullptr;
	const bool pawn_answer = (pawn_fn != nullptr)
		? reinterpret_cast<bool (*)(void*)>(pawn_fn)(bot_pawn_) : false;
	const bool scratch_answer = (scratch_fn != nullptr)
		? reinterpret_cast<bool (*)(void*)>(scratch_fn)(scratch) : false;
	RemoveScratchEntity(scratch);
	RowDetail(context, "bot pawn -> %s, info_target -> %s", pawn_answer ? "true" : "false",
		scratch_answer ? "true" : "false");
	if (!pawn_answer || scratch_answer)
	{
		context.Fail("behaviour: pawn/other answers are wrong; the slot is not IsPlayerPawn");
	}
}

void GamedataSuite::VerifyCommitSuicideSlot(TestContext& context, const std::string& table,
                                            long long slot)
{
	// CSS's CommitSuicide: (pawn, explode, force) - the TestPlugin's own
	// call, which passes (true, true) for explode and force. The first live
	// run passed (false, false) and the pawn's health did not move; the
	// (true, true) shape is the one the ecosystem actually uses. The oracle
	// is the pawn's health and life state: the call must leave the bot dead.
	// The bot comes back on respawn or the next round, which is what the
	// respawn path exists for.
	if (bot_pawn_ == nullptr)
	{
		context.SkipCall(table, "slot %lld | %s", slot, BotMissingReason());
		return;
	}
	std::size_t health_offset = 0;
	std::size_t life_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_iHealth", health_offset)
	    || !SchemaOffset("CBaseEntity", "m_lifeState", life_offset))
	{
		context.Fail("setup: m_iHealth or m_lifeState did not resolve through the schema");
		return;
	}
	void* fn = VtableSlotInServer(context, bot_pawn_, slot);
	if (fn == nullptr)
	{
		return;
	}
	std::int32_t before = 0;
	std::memcpy(&before, reinterpret_cast<const unsigned char*>(bot_pawn_) + health_offset,
		sizeof(before));
	reinterpret_cast<void (*)(void*, bool, bool)>(fn)(bot_pawn_, true, true);
	std::int32_t after = 0;
	std::memcpy(&after, reinterpret_cast<const unsigned char*>(bot_pawn_) + health_offset,
		sizeof(after));
	std::uint8_t life = 0;
	std::memcpy(&life, reinterpret_cast<const unsigned char*>(bot_pawn_) + life_offset,
		sizeof(life));

	RowDetail(context, "bot health %d -> %d, m_lifeState %u", before, after, life);
	if (after > 0 && life == 0)
	{
		context.Suspect("behaviour: the pawn is still alive after CommitSuicide; round rules "
		                "may defer the death");
	}
	// The pawn is dead or dying; nothing else in this run may use it as a
	// live bot pawn.
	bot_pawn_dead_ = true;
}

void GamedataSuite::VerifyTeleportSlot(TestContext& context, const std::string& table,
                                       long long slot)
{
	// CSS's Teleport: (entity, origin, angle, velocity) - the TestPlugin's
	// css_teleportup moves the pawn 100 units up, on a scratch entity here.
	// The oracle is the scene node's m_vecAbsOrigin: read before, moved,
	// read after, and moved back - the round trip every mutation row here
	// does. This row needs no bot.
	(void)table;
	std::size_t body_offset = 0;
	std::size_t node_offset = 0;
	std::size_t origin_offset = 0;
	if (!SchemaOffset("CBaseEntity", "m_CBodyComponent", body_offset)
	    || !SchemaOffset("CBodyComponent", "m_pSceneNode", node_offset)
	    || !SchemaOffset("CGameSceneNode", "m_vecAbsOrigin", origin_offset))
	{
		context.Fail("setup: the scene-node origin chain did not resolve through the schema");
		return;
	}
	void* entity = CreateScratchEntity(true);
	if (entity == nullptr)
	{
		context.Fail("setup: UTIL_CreateEntityByName(info_target) returned no entity");
		return;
	}
	void* body = SlotAt(entity, body_offset);
	void* node = LooksLikeObjectPointer(body) ? SlotAt(body, node_offset) : nullptr;
	if (!LooksLikeObjectPointer(node))
	{
		RemoveScratchEntity(entity);
		context.Fail("setup: the scratch entity has no scene node to read the origin from");
		return;
	}
	void* fn = VtableSlotInServer(context, entity, slot);
	if (fn == nullptr)
	{
		RemoveScratchEntity(entity);
		return;
	}
	const auto teleport = reinterpret_cast<void (*)(void*, const void*, void*, void*)>(fn);
	const auto read_origin = [&](float out[3]) {
		std::memcpy(out, reinterpret_cast<const unsigned char*>(node) + origin_offset,
			3 * sizeof(float));
	};

	float before[3] = {0};
	read_origin(before);
	float moved[3] = {before[0], before[1], before[2] + 100.0f};
	teleport(entity, moved, nullptr, nullptr);
	float after[3] = {0};
	read_origin(after);
	teleport(entity, before, nullptr, nullptr);
	float restored[3] = {0};
	read_origin(restored);
	RemoveScratchEntity(entity);

	RowDetail(context, "origin z %.1f -> %.1f (asked +100) -> restored %.1f", before[2],
		after[2], restored[2]);
	if (std::fabs(after[2] - before[2] - 100.0f) > 0.5f)
	{
		context.Fail("behaviour: the origin did not follow the teleport; the slot is not "
		             "Teleport");
	}
}

// -----------------------------------------------------------------------------
//  byte offsets - offsetof() where the SDK declares the struct
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseStructOffsets(TestContext& context)
{
	context.Note("[offsets] byte offsets, one row per key (two against offsetof, one "
	             "behavioural)");

	const std::string entity_system = "offsets.GameEntitySystem";
	CS2ST_CALL(context, entity_system, {
		const TomlData::Entry* entry = data_.FindPlatform("offsets.GameEntitySystem",
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("the key is missing from the file");
			return;
		}

		// The SDK's own GameEntitySystem() accessor - which compiles into this
		// plugin - reads this member, so offsetof here and the ecosystem's
		// number are two descriptions sharing no code.
		const std::size_t sdk_offset =
			offsetof(CGameResourceService, m_pEntityResourceManifestHandler);
		context.Detail("gamedata %lld, offsetof %zu", entry->number, sdk_offset);
		if (static_cast<long long>(sdk_offset) != entry->number)
		{
			context.Fail("the SDK declaration and the gamedata disagree; every plugin "
			             "reading this offset reads the wrong bytes");
			return;
		}

		if (resource_service_ == nullptr)
		{
			context.Detail("gamedata %lld, offsetof %zu; no live service object to read",
				entry->number, sdk_offset);
			return;
		}
		void* at_offset = SlotAt(resource_service_, sdk_offset);
		context.Detail("gamedata %lld, offsetof %zu; live object reads %p", entry->number,
			sdk_offset, at_offset);
		if (!LooksLikeObjectPointer(at_offset))
		{
			context.Fail("the offset reads a non-pointer from the live service object; the "
			             "declaration has drifted from the binary");
		}
	});

	const std::string player_slot = "offsets.CheckTransmitPlayerSlot";
	CS2ST_CALL(context, player_slot, {
		const TomlData::Entry* entry = data_.FindPlatform("offsets.CheckTransmitPlayerSlot",
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("the key is missing from the file");
			return;
		}
		const std::size_t sdk_offset = offsetof(CCheckTransmitInfo, m_nPlayerSlot);
		context.Detail("gamedata %lld, offsetof %zu", entry->number, sdk_offset);
		if (static_cast<long long>(sdk_offset) != entry->number)
		{
			context.Fail("the SDK declaration and the gamedata disagree; every "
			             "CheckTransmit hook reads the wrong field");
		}
	});

	CS2ST_CALL(context, std::string("offsets.CTakeDamageInfo_HitGroup"), {
		const TomlData::Entry* entry = data_.FindPlatform("offsets.CTakeDamageInfo_HitGroup",
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("the key is missing from the file");
			return;
		}
		VerifyTakeDamageHitGroup(context, entry->number);
	});
}

// -----------------------------------------------------------------------------
//  Hooks - Host_Say and CEntityInstance_AcceptInput, observation only
// -----------------------------------------------------------------------------

int GamedataSuite::InstallHooks(TestContext& context)
{
	(void)context;

	if (hooks_installed_)
	{
		return 0;
	}

	// Same discipline as GameEntitySystemSuite: a detour writes a jump into
	// code this plugin did not compile, so it only goes in once the sweep has
	// said the address under it is the function the file names.
	if (!signatures_verified_)
	{
		CS2ST_LOG_WARN("%s: refusing to install - run 'meta_test run %s' first so the "
		               "signatures are resolved and verified\n",
			Id(), Id());
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before installing, so a hook that never fires still appears in
	// the dump as `never` - after a CS2 update that row is the finding.
	recorder.Declare(kHostSay, Id());
	recorder.Declare(kAcceptInput, Id());

	// Re-attach first, then configure. RemoveContext took this suite's
	// callbacks out; Configure on an address it has already detoured returns
	// early without restoring them, so a re-install after a remove would leave
	// a live detour that records nothing.
	hook_host_say_.AddContext(this, &GamedataSuite::Pre_HostSay,
		&GamedataSuite::Post_HostSay);
	hook_accept_input_.AddContext(this, &GamedataSuite::Pre_AcceptInput,
		&GamedataSuite::Post_AcceptInput);

	hook_host_say_.Configure(Address(kHostSay));
	hook_accept_input_.Configure(Address(kAcceptInput));

	hooks_installed_ = true;
	CS2ST_LOG_INFO("%s: Host_Say and CEntityInstance_AcceptInput detoured (observation "
	               "only)\n", Id());

	return kHookCount;
}

void GamedataSuite::RemoveHooks(TestContext& context)
{
	(void)context;

	if (!hooks_installed_)
	{
		return;
	}

	// KHook::Function has no uninstall short of destruction: Configure puts the
	// detour in, the destructor takes it out. RemoveContext detaches this
	// suite's callbacks - recording stops immediately, the detour itself stays
	// (harmlessly, forwarding to an empty context list) until the suite object
	// is destroyed at plugin unload. MetamodListenerSuite documents the same
	// no-inverse asymmetry.
	hook_host_say_.RemoveContext(this);
	hook_accept_input_.RemoveContext(this);

	hooks_installed_ = false;
}

KHook::Return<void> GamedataSuite::Pre_HostSay(CEntityInstance* controller, CCommand& args,
                                               bool teamonly, int unk1, const char* unk2)
{
	// No fault guard here. Reading ArgC() trusts the SDK's CCommand layout
	// against an object the engine built, so the count is bounds-checked
	// before anything indexes with it - the same discipline as
	// Source2GameClientsSuite::Pre_ClientCommand.
	const int argc = args.ArgC();
	const bool plausible = argc > 0 && argc <= kMaxPlausibleArgC;

	char arg0_scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kHostSay, "controller=%p argc=%d%s say=\"%.63s\" teamonly=%d",
		static_cast<void*>(controller),
		argc,
		plausible ? "" : " IMPLAUSIBLE",
		plausible ? SafeStringArg(args.Arg(0), arg0_scratch) : "<not read>",
		teamonly ? 1 : 0);

	// unk1 and unk2 are recorded nowhere: unknown semantics, and a hook that
	// reads what it cannot vet is the crash this project exists to prevent.
	(void)unk1;
	(void)unk2;
	return { KHook::Action::Ignore };
}

KHook::Return<void> GamedataSuite::Post_HostSay(CEntityInstance*, CCommand&, bool, int,
                                                const char*)
{
	CS2ST_HOOK_LEAVE(kHostSay);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GamedataSuite::Pre_AcceptInput(CEntityInstance* self, const char* input,
                                                   CEntityInstance* activator,
                                                   CEntityInstance* caller, void* value,
                                                   int output_id, void* unk)
{
	char input_scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kAcceptInput,
		"self=%p input=\"%.63s\" activator=%p caller=%p value=%p output_id=%d",
		static_cast<void*>(self),
		SafeStringArg(input, input_scratch),
		static_cast<void*>(activator),
		static_cast<void*>(caller),
		value,
		output_id);

	// The value parameter is a variant_t* in the ecosystem's typedef; void*
	// here because the payload is never read, only its address recorded.
	(void)unk;
	return { KHook::Action::Ignore };
}

KHook::Return<void> GamedataSuite::Post_AcceptInput(CEntityInstance*, const char*,
                                                    CEntityInstance*, CEntityInstance*,
                                                    void*, int, void*)
{
	CS2ST_HOOK_LEAVE(kAcceptInput);
	return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
//  Construction - the hook templates bind their callbacks here, the addresses
//  they detour arrive later, from the sweep
// -----------------------------------------------------------------------------

GamedataSuite::GamedataSuite()
	: hook_host_say_(this, &GamedataSuite::Pre_HostSay, &GamedataSuite::Post_HostSay),
	  hook_accept_input_(this, &GamedataSuite::Pre_AcceptInput, &GamedataSuite::Post_AcceptInput)
{
}

} // namespace cs2st
