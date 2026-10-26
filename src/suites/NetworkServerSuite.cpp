#include "suites/NetworkServerSuite.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <appframework/iappsystem.h>
#include <engine2/gameshareddefs.h>
#include <engine2/gamesessionconfiguration.h>
#include <engine2/iengineservicemgr.h>
#include <engine2/inetworkgameserver.h>
#include <engine2/ivengineserver2.h>
#include <engine2/networkgameserverbase.h>
#include <icvar.h>
#include <interfaces/interfaces.h>
#include <tier0/buffer_string.h>
#include <tier0/convar.h>
#include <tier0/netadr.h>
#include <tier0/utlvector.h>

#include "core/Log.h"
#include "hook/HookArg.h"
#include "hook/HookRecorder.h"
#include "testkit/PrivateField.h"
#include "testkit/StructInspector.h"
#include "testkit/UtlVectorGuard.h"

// The SH_DECL_HOOK block used to live here; under KHook the hook templates are
// declared as members in the header instead, bound to the SDK member pointers
// in the constructor at the bottom of this file. All five are on
// INetworkServerService and none on the game server, which is the whole design
// of this suite's hook half - see the header. StartChangeLevel and
// PreserveSteamID exist on *both* interfaces, but each KHook::Virtual carries
// its own member pointer, so binding the service's pair does not collide with
// anything. SetFinalSimulationTickThisFrame dropped off the list when the
// s2sdk rework removed it from the interface - the per-frame liveness probe
// duty now falls to SetGameLoadStarted being the next thing to fire on a level
// change, and to the sweep's own tick cross-checks.

namespace cs2st
{

// The one member GameSessionConfiguration_t adds over its protobuf base is
// protected in the SDK's CBaseCmdKeyValues template, so offsetof cannot name
// it - captured through the explicit-instantiation access hole instead. The
// member belongs to the template base, so the capture names that (a pointer
// to member of a base cannot name a derived class), which also puts a comma
// in the type - the typedef exists to keep it out of the macro argument.
// The derived class adds nothing, so the offset within the base is the offset
// within GameSessionConfiguration_t. At cs2st scope on purpose: the anonymous
// namespace below is not an enclosing namespace of the template. See
// testkit/PrivateField.h.
using GameSessionConfigOwner = CBaseCmdKeyValues<CSVCMsg_GameSessionConfiguration>;
CS2ST_PRIVATE_FIELD(GameSessionConfigKeyValues, GameSessionConfigOwner,
	KeyValues*, m_pKeyValues);

namespace
{

// ---- hook ids ---------------------------------------------------------------

constexpr const char* kStartChangeLevel =
	"INetworkServerService::StartChangeLevel";
constexpr const char* kSetGameLoadStarted =
	"INetworkServerService::SetGameLoadStarted";
constexpr const char* kSetGameLoadFailed =
	"INetworkServerService::SetGameLoadFailed";
constexpr const char* kDisconnectGameNow =
	"INetworkServerService::DisconnectGameNow";
constexpr const char* kPreserveSteamID =
	"INetworkServerService::PreserveSteamID";

constexpr int kHookedMethodCount = 5;

// ---- probe constants --------------------------------------------------------

/**
 * Player slot the read-only per-player queries use. Occupancy does not matter:
 * these are reads, and an empty slot probes the call just as well - what is
 * under test is that two interfaces answer the same thing about it.
 */
constexpr int kProbeSlot = 0;

/**
 * The same nonsense name Source2EngineSuite hands to
 * IVEngineServer2::FindSpawnGroupByName. Sharing the string is deliberate: it
 * is what makes the two answers comparable rather than merely similar.
 */
constexpr const char* kNoSuchSpawnGroup = "cs2sdktest_no_such_group";

/** A spawn group handle nothing will have loaded. */
constexpr SpawnGroupHandle_t kUnknownSpawnGroup = 0;

/** PauseGroup_t is a bare int with no enumerators in the SDK; 0 is the probe. */
constexpr PauseGroup_t kProbePauseGroup = 0;

/**
 * How far GetServerTick() and CGlobalVars::tickcount may differ.
 *
 * Zero would be defensible - a call sweep runs synchronously inside the
 * engine's own frame, so no tick can pass between the two reads - but one tick
 * of slack costs nothing and keeps the row honest if a sweep is ever driven
 * from somewhere else.
 */
constexpr int kTickSlack = 1;

// ---- helpers ----------------------------------------------------------------

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

const char* NetAdrTypeName(netadrtype_t type)
{
	switch (type)
	{
		case NA_NULL:      return "NA_NULL";
		case NA_LOOPBACK:  return "NA_LOOPBACK";
		case NA_BROADCAST: return "NA_BROADCAST";
		case NA_IP:        return "NA_IP";
		default:           return "<out of range>";
	}
}

std::uint32_t ByteSwap32(std::uint32_t value)
{
	return ((value & 0x000000FFu) << 24)
	     | ((value & 0x0000FF00u) << 8)
	     | ((value & 0x00FF0000u) >> 8)
	     | ((value & 0xFF000000u) >> 24);
}

/**
 * IVEngineServer2, used as a tool rather than as a subject.
 *
 * Almost every fact this suite reports is also reachable through the engine
 * interface, and that duplication is what the cross-checks are made of. Fetched
 * per use: the lookup is a cached module handle plus one factory call, and
 * keeping it local means no member has to be kept valid across a sweep.
 */
IVEngineServer2* Engine(TestContext& context)
{
	return static_cast<IVEngineServer2*>(
		context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
}

/**
 * ICvar, also a tool: two of the checks below compare what the server reports
 * against the convar it is supposed to be reporting.
 *
 * tier0 first and then anywhere, mirroring ConVarSuite - "tier0 stopped
 * answering VEngineCvar007" and "some other module answers it now" are
 * different findings, and that suite is the one that reports them. Here the
 * fallback exists only so a moved interface degrades these rows instead of
 * silently deleting them.
 */
ICvar* Cvar(TestContext& context)
{
	ICvar* cvar = static_cast<ICvar*>(
		context.QueryInterface("tier0", CVAR_INTERFACE_VERSION));
	if (cvar == nullptr)
	{
		cvar = static_cast<ICvar*>(context.QueryInterfaceAnywhere(CVAR_INTERFACE_VERSION));
	}
	return cvar;
}

// ---- local netadr_t copies --------------------------------------------------

/** Slots in the scratch storage the local netadr_t rows share. */
constexpr int kAddrBase      = 0; // 10.20.30.40:27015
constexpr int kAddrSiblingC  = 1; // 10.20.30.99:27015 - same class C as the base
constexpr int kAddrSiblingB  = 2; // 10.20.99.40:27015 - same class B, other class C
constexpr int kAddrStranger  = 3; // 10.99.30.40:27015 - other class B
constexpr int kAddrOtherPort = 4; // 10.20.30.40:27016 - the base with another port
constexpr int kAddrAltBuild  = 5; // the base again, built through SetIPAndPort
constexpr int kAddrParsed    = 6; // the base again, built through SetFromString
constexpr int kAddrCleared   = 7; // a copy of the base, then Clear()ed
constexpr int kAddrCount     = 8;

constexpr unsigned short kProbePort = 27015;

/**
 * One scratch slot as a netadr_t, zeroed and put through Clear + SetType(NA_IP).
 *
 * Raw storage and not netadr_t locals for the reason InspectNetworkAddress
 * gives: the default constructor calls three tier0 functions, and a local would
 * run them outside the fault guard. This helper is only ever called from inside
 * a CS2ST_CALL body.
 */
netadr_t* FreshAddress(unsigned char* storage, int index)
{
	unsigned char* slot = storage + static_cast<std::size_t>(index) * sizeof(netadr_t);
	std::memset(slot, 0, sizeof(netadr_t));
	netadr_t* address = reinterpret_cast<netadr_t*>(slot);
	address->Clear();
	address->SetType(NA_IP);
	return address;
}

netadr_t* AddressAt(unsigned char* storage, int index)
{
	return reinterpret_cast<netadr_t*>(storage + static_cast<std::size_t>(index) * sizeof(netadr_t));
}

netadr_t* BuildAddress(unsigned char* storage, int index,
	uint8 b1, uint8 b2, uint8 b3, uint8 b4, unsigned short port)
{
	netadr_t* address = FreshAddress(storage, index);
	address->SetIP(b1, b2, b3, b4);
	address->SetPort(port);
	return address;
}

/**
 * The netadr_t mutators and class comparisons, run against addresses this suite
 * builds itself. InspectNetworkAddress only reads the one the engine hands out,
 * which is why none of these could be called there; nothing below touches the
 * engine's copy, so there is no state to disturb. SetFromString is given a
 * literal address and the default bUseDNS = false, so it never leaves the
 * process.
 */
void ExerciseLocalNetworkAddress(TestContext& context)
{
	context.Note("[netadr_t] mutators and class comparisons on local copies; "
	             "the engine's address is not touched");

	alignas(netadr_t) unsigned char storage[kAddrCount * sizeof(netadr_t)] = {};
	bool built = false;

	CS2ST_CALL(context, "netadr_t::Clear / SetType / SetIP(b1..b4) / SetPort (build the local set)", {
		netadr_t* base = BuildAddress(storage, kAddrBase, 10, 20, 30, 40, kProbePort);
		BuildAddress(storage, kAddrSiblingC, 10, 20, 30, 99, kProbePort);
		BuildAddress(storage, kAddrSiblingB, 10, 20, 99, 40, kProbePort);
		BuildAddress(storage, kAddrStranger, 10, 99, 30, 40, kProbePort);
		BuildAddress(storage, kAddrOtherPort, 10, 20, 30, 40,
			static_cast<unsigned short>(kProbePort + 1));
		built = true;

		const unsigned char* ip_bytes = reinterpret_cast<const unsigned char*>(base)
			+ offsetof(netadr_t, ip);
		const char* rendered = base->ToString(false);
		context.Detail("base -> type %d, port %u, bytes %u.%u.%u.%u, \"%s\"",
			static_cast<int>(base->GetType()), static_cast<unsigned>(base->GetPort()),
			ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3],
			rendered != nullptr ? rendered : "(null)");

		if (base->GetType() != NA_IP)
		{
			context.Fail("SetType(NA_IP) did not read back as NA_IP");
		}
		if (base->GetPort() != kProbePort)
		{
			context.Fail("SetPort(%u) did not read back through GetPort",
				static_cast<unsigned>(kProbePort));
		}
		if (ip_bytes[0] != 10 || ip_bytes[1] != 20 || ip_bytes[2] != 30 || ip_bytes[3] != 40)
		{
			// Suspect and not Fail: the SDK does not say SetIP's b1 lands in ip[0].
			context.Suspect("SetIP(10,20,30,40) did not leave 10.20.30.40 in ip[]");
		}
		// The header documents the rendering as xxx.xxx.xxx.xxx:ppppp.
		if (rendered == nullptr || std::strcmp(rendered, "10.20.30.40:27015") != 0)
		{
			context.Fail("ToString did not render the address as xxx.xxx.xxx.xxx:ppppp");
		}
	});

	if (!built)
	{
		context.SkipCall("netadr_t (local copies)", "the build row did not complete");
		return;
	}

	CS2ST_CALL(context, "netadr_t::SetIP(uint) / SetIPAndPort", {
		// The header says the uint is host order, so GetIPHostByteOrder has to
		// give it back whatever the byte placement is.
		constexpr unsigned int kHostValue = 0x0A141E28u; // 10.20.30.40
		netadr_t* alt = FreshAddress(storage, kAddrAltBuild);
		alt->SetIPAndPort(kHostValue, kProbePort);
		const unsigned int host = alt->GetIPHostByteOrder();
		context.Detail("-> host order 0x%08lx, port %u", static_cast<unsigned long>(host),
			static_cast<unsigned>(alt->GetPort()));

		if (host != kHostValue)
		{
			context.Fail("SetIP(uint) is documented as host order, but GetIPHostByteOrder "
			             "did not return what was set");
		}
		if (alt->GetPort() != kProbePort)
		{
			context.Fail("SetIPAndPort did not set the port");
		}
		if (!alt->CompareAdr(*AddressAt(storage, kAddrBase)))
		{
			context.Suspect("SetIPAndPort(0x0A141E28) and SetIP(10,20,30,40) built "
			                "different addresses");
		}
	});

	CS2ST_CALL(context, "netadr_t::SetFromString (literal address, no DNS)", {
		netadr_t* parsed = FreshAddress(storage, kAddrParsed);
		const bool ok = parsed->SetFromString("10.20.30.40:27015");
		const char* rendered = parsed->ToString(false);
		context.Detail("-> %s, \"%s\"", ok ? "true" : "false",
			rendered != nullptr ? rendered : "(null)");

		if (!ok)
		{
			context.Suspect("SetFromString rejected a dotted-quad literal");
		}
		else if (!parsed->CompareAdr(*AddressAt(storage, kAddrBase)))
		{
			context.Suspect("the parsed address differs from the one built by setters");
		}
	});

	CS2ST_CALL(context, "netadr_t::Clear (on a copy)", {
		netadr_t* cleared = AddressAt(storage, kAddrCleared);
		std::memcpy(cleared, AddressAt(storage, kAddrBase), sizeof(netadr_t));
		cleared->Clear();
		context.Detail("-> type %d, port %u, IsValid %s",
			static_cast<int>(cleared->GetType()), static_cast<unsigned>(cleared->GetPort()),
			cleared->IsValid() ? "true" : "false");

		// ip & port != 0 is the stated rule for IsValid, and Clear is "invalids
		// Address" in the same header.
		if (cleared->IsValid())
		{
			context.Fail("a cleared address is still valid");
		}
		if (cleared->GetType() != NA_NULL)
		{
			context.Suspect("a cleared address does not have type NA_NULL");
		}
	});

	CS2ST_CALL(context, "netadr_t::CompareAdr (onlyBase against a port-only difference)", {
		const netadr_t* base = AddressAt(storage, kAddrBase);
		const netadr_t* other_port = AddressAt(storage, kAddrOtherPort);
		const bool full = base->CompareAdr(*other_port, false);
		const bool base_only = base->CompareAdr(*other_port, true);
		context.Detail("full -> %s, base only -> %s",
			full ? "true" : "false", base_only ? "true" : "false");

		if (full)
		{
			context.Suspect("two addresses that differ only by port compare equal in full");
		}
		if (!base_only)
		{
			context.Suspect("onlyBase = true still compares the port");
		}
	});

	CS2ST_CALL(context, "netadr_t::CompareClassBAdr / CompareClassCAdr", {
		const netadr_t* base = AddressAt(storage, kAddrBase);
		const netadr_t* sibling_c = AddressAt(storage, kAddrSiblingC);
		const netadr_t* sibling_b = AddressAt(storage, kAddrSiblingB);
		const netadr_t* stranger = AddressAt(storage, kAddrStranger);

		const bool b_c = base->CompareClassBAdr(*sibling_c);
		const bool c_c = base->CompareClassCAdr(*sibling_c);
		const bool b_b = base->CompareClassBAdr(*sibling_b);
		const bool c_b = base->CompareClassCAdr(*sibling_b);
		const bool b_s = base->CompareClassBAdr(*stranger);
		const bool c_s = base->CompareClassCAdr(*stranger);
		context.Detail("same /24: B %s C %s; same /16 only: B %s C %s; other /16: B %s C %s",
			b_c ? "y" : "n", c_c ? "y" : "n", b_b ? "y" : "n", c_b ? "y" : "n",
			b_s ? "y" : "n", c_s ? "y" : "n");

		// The classful reading of the names: B is the first two bytes, C the
		// first three. Not stated in the header, hence Suspect.
		if (!b_c || !c_c || !b_b || c_b || b_s || c_s)
		{
			context.Suspect("the class B / class C comparisons do not follow the "
			                "/16 and /24 reading of their names");
		}
	});

	CS2ST_CALL(context, "netadr_t::operator< (two distinct addresses)", {
		const netadr_t* base = AddressAt(storage, kAddrBase);
		const netadr_t* stranger = AddressAt(storage, kAddrStranger);
		const bool forward = (*base < *stranger);
		const bool backward = (*stranger < *base);
		context.Detail("base<stranger %s, stranger<base %s",
			forward ? "true" : "false", backward ? "true" : "false");
		if (forward == backward)
		{
			context.Suspect("two different addresses are not strictly ordered by operator<");
		}
	});
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool NetworkServerSuite::Acquire(TestContext& context)
{
	service_ = static_cast<INetworkServerService*>(
		context.QueryInterface("engine2", NETWORKSERVERSERVICE_INTERFACE_VERSION));

	if (service_ == nullptr)
	{
		// Worth distinguishing "engine2 is not loaded" from "engine2 is loaded
		// but no longer answers this version": the second one is the finding.
		const Module& engine_module = context.GetModule("engine2");
		if (!engine_module.IsValid())
		{
			context.UnavailableCall("INetworkServerService", "engine2 is not loaded");
		}
		else
		{
			context.UnavailableCall("INetworkServerService",
				"engine2 is loaded but does not answer '%s'",
				NETWORKSERVERSERVICE_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("interface at %p (%s @ engine2)", static_cast<void*>(service_),
		NETWORKSERVERSERVICE_INTERFACE_VERSION);

	// Nothing else here. The game server needs an engine call to fetch, and
	// this function also runs on the hook-install path, which has no fault
	// guard around it.
	return true;
}

void NetworkServerSuite::ExerciseCalls(TestContext& context)
{
	globals_ = nullptr;
	game_server_ = nullptr;

	ExerciseAppSystem(context);
	ExerciseEngineService(context);
	ResolveGameServer(context);
	ExerciseServerService(context);
	ExerciseGameServer(context);
	ExerciseGameServerBase(context);
	InspectNetworkAddress(context);
	ExerciseLocalNetworkAddress(context);
	NoteInheritanceShape(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void NetworkServerSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = service_->GetTier();
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
		const bool singleton = service_->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the network server service is a singleton; false here "
			             "suggests a slot shift");
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = service_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = service_->GetDependencies();
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
		void* self = service_->QueryInterface(NETWORKSERVERSERVICE_INTERFACE_VERSION);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(service_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run engine startup");
	context.SkipCall("IAppSystem::Init", "would re-run engine startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin engine shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the engine down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down engine interfaces");
}

// -----------------------------------------------------------------------------
//  IEngineService - 11 slots between IAppSystem and the service
//
//  No other suite reaches these, because no other suite's interface derives
//  from IEngineService. They are also where the best oracle in this file lives:
//  a service knows its own name, and the manager that registered it can be
//  asked to look that name back up.
// -----------------------------------------------------------------------------

void NetworkServerSuite::ExerciseEngineService(TestContext& context)
{
	context.Note("[IEngineService] slots 11-21; the only suite that covers them");

	const char* service_name = nullptr;

	CS2ST_CALL(context, "IEngineService::GetName", {
		service_name = service_->GetName();
		context.Detail("-> %s", service_name != nullptr ? service_name : "(null)");
		if (service_name == nullptr)
		{
			context.Fail("a service the manager registered by name has a name");
		}
	});

	CS2ST_CALL(context, "IEngineService::IsActive", {
		context.Detail("-> %s", service_->IsActive() ? "true" : "false");
	});

	CS2ST_CALL(context, "IEngineService::GetServiceIndex", {
		context.Detail("-> %u", static_cast<unsigned>(service_->GetServiceIndex()));
	});

	CS2ST_CALL(context, "IEngineService::GetServiceDependencies", {
		context.Detail("-> %p", service_->GetServiceDependencies());
	});

	// The round trip. Two routes to one object that share no code: a
	// CreateInterface lookup by version string, and the engine's own service
	// registry looked up by the name the service reports for itself. If they
	// disagree, either GetName is not GetName or the registry holds something
	// else under that name - and neither is visible from one side alone.
	CS2ST_CALL(context, "IEngineServiceMgr::FindService (round trip through GetName)", {
		IEngineServiceMgr* manager = static_cast<IEngineServiceMgr*>(
			context.QueryInterface("engine2", ENGINESERVICEMGR_INTERFACE_VERSION));
		if (manager == nullptr)
		{
			context.Detail("-> engine2 does not answer '%s'; no round trip possible",
				ENGINESERVICEMGR_INTERFACE_VERSION);
			return;
		}
		if (service_name == nullptr)
		{
			context.Detail("-> GetName returned null; nothing to look up");
			return;
		}

		IEngineService* found = manager->FindService(service_name);
		IEngineService* expected = static_cast<IEngineService*>(service_);
		context.Detail("FindService(\"%s\") -> %p, we hold %p", service_name,
			static_cast<void*>(found), static_cast<void*>(expected));

		if (found == nullptr)
		{
			context.Fail("the manager does not know the name this service reports");
		}
		else if (found != expected)
		{
			context.Fail("the registry's entry for this name is a different object");
		}
	});

	CS2ST_CALL(context, "IEngineService::ShouldActivate", {
		// A pure policy query. The argument is unnamed in the SDK, so an empty
		// string is the probe: the answer is not judged, only that it is one of
		// the three enumerators.
		const EngineServiceActivateType_t type = service_->ShouldActivate("");
		const int type_value = static_cast<int>(type);
		context.Detail("-> %d for an empty argument", type_value);
		if (type_value < static_cast<int>(ENGINE_SERVICE_ACTIVATE_NEVER)
			|| type_value > static_cast<int>(ENGINE_SERVICE_ACTIVATE_IF_ADDED_BY_LOOP_MODE))
		{
			context.Fail("%d is outside EngineServiceActivateType_t; the slot is "
			             "probably not ShouldActivate", type_value);
		}
	});

	context.SkipCall("IEngineService::SetActive", "would activate or deactivate the service");
	context.SkipCall("IEngineService::SetName", "would rename a registered service");
	context.SkipCall("IEngineService::SetServiceIndex", "would renumber it in the registry");
	context.SkipCall("IEngineService::OnLoopActivate", "drives the engine's loop transition");
	context.SkipCall("IEngineService::OnLoopDeactivate", "drives the engine's loop transition");
	context.SkipCall("IEngineService::RegisterEventMap",
		"registers or unregisters this service's event handlers on the dispatcher");
}

// -----------------------------------------------------------------------------
//  The game server object, and everything the cross-checks need
// -----------------------------------------------------------------------------

void NetworkServerSuite::ResolveGameServer(TestContext& context)
{
	context.Note("[INetworkServerService] the game server, and the engine-side "
	             "values the rest of this suite is checked against");

	CS2ST_CALL(context, "INetworkServerService::GetIGameServer", {
		game_server_ = service_->GetIGameServer();
		context.Detail("-> %p", static_cast<void*>(game_server_));

		if (!have_previous_game_server_)
		{
			// Nothing to compare against yet; the second sweep is where this
			// row starts saying something.
			return;
		}
		if (game_server_ != previous_game_server_)
		{
			// Not a failure - it is the documented-by-behaviour fact that the
			// engine rebuilds this object, measured instead of assumed. It is
			// also the reason no hook in this suite is attached to it.
			context.Detail("-> %p (was %p on the previous sweep: the engine "
			               "replaced the game server, as it does on level change)",
				static_cast<void*>(game_server_),
				static_cast<void*>(previous_game_server_));
		}
	});

	previous_game_server_ = game_server_;
	have_previous_game_server_ = true;

	CS2ST_CALL(context, "IVEngineServer2::GetServerGlobals (for the cross-checks)", {
		IVEngineServer2* engine = Engine(context);
		if (engine == nullptr)
		{
			context.Detail("-> engine2 does not answer '%s'; the cross-checks "
			               "below degrade to plain reads", INTERFACEVERSION_VENGINESERVER);
			return;
		}
		globals_ = engine->GetServerGlobals();
		context.Detail("-> %p", static_cast<void*>(globals_));
	});
}

void NetworkServerSuite::ExerciseServerService(TestContext& context)
{
	CS2ST_CALL(context, "INetworkServerService::IsActiveInGame", {
		context.Detail("-> %s", service_->IsActiveInGame() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkServerService::IsMultiplayer", {
		context.Detail("-> %s", service_->IsMultiplayer() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkServerService::IsServerRunning", {
		// Four related booleans in one row, reported and not judged. They
		// plainly describe overlapping states, but the SDK defines no relation
		// between them, and turning "plainly" into a FAIL is how this project
		// has manufactured false alarms before. The row is still worth having:
		// after an update, a combination that never occurred before is visible
		// here and nowhere else.
		context.Detail("running=%s inGame=%s gameServer=%s",
			service_->IsServerRunning() ? "yes" : "no",
			service_->IsActiveInGame() ? "yes" : "no",
			game_server_ != nullptr ? "present" : "null");
	});

	CS2ST_CALL(context, "INetworkServerService::GetServerNetworkAddress", {
		// Used to be declared as returning a bare int; the Oct 2026 SDK rework
		// types it as netadr_t, matching INetworkGameServer's declaration - and
		// the struct below is where the checkable structure lives.
		const netadr_t address = service_->GetServerNetworkAddress();
		char rendered[64] = {};
		address.ToString(rendered, sizeof(rendered), false);
		context.Detail("-> \"%s\"", rendered);
	});

	CS2ST_CALL(context, "INetworkServerService::GameLoadFailed", {
		context.Detail("-> %s", service_->GameLoadFailed() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkServerService::GetServerSerializersCRC", {
		const CRC32_t crc = service_->GetServerSerializersCRC();
		context.Detail("-> 0x%08lx", static_cast<unsigned long>(crc));
	});

	CS2ST_CALL(context, "INetworkServerService::GetServerSerializersMsg", {
		context.Detail("-> %p", service_->GetServerSerializersMsg());
	});

	CS2ST_CALL(context, "INetworkServerService::GetGameSpawnGroupMgr", {
		// Slot the Oct 2026 rework added between GetServerSerializersMsg and
		// IsSaveRestoreAllowed.
		context.Detail("-> %p", static_cast<void*>(service_->GetGameSpawnGroupMgr()));
	});

	CS2ST_CALL(context, "INetworkServerService::IsChangelevelPending", {
		// A read-only state query the rework named; whatever it answers about
		// this server, the call surviving is the slot check.
		context.Detail("-> %s", service_->IsChangelevelPending() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkServerService::IsEntityReportActive", {
		context.Detail("-> %s", service_->IsEntityReportActive() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkServerService::GetEntityReport", {
		// The header: nullptr unless the report is active and targets the slot
		// (or every slot). So an inactive report has to answer null.
		const bool active = service_->IsEntityReportActive();
		void* report = static_cast<void*>(service_->GetEntityReport(kProbeSlot));
		context.Detail("-> %p for slot %d (report %s)", report, kProbeSlot,
			active ? "active" : "inactive");
		if (!active && report != nullptr)
		{
			context.Fail("no entity report is active, but one was returned");
		}
	});

	CS2ST_CALL(context, "INetworkServerService::ThreadInPrimaryOrSecondaryMainThread", {
		// Reported, not judged: which thread a sweep runs on is the engine's
		// business, and the SDK does not name the expected answer.
		context.Detail("-> %s",
			service_->ThreadInPrimaryOrSecondaryMainThread() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkServerService::unk101", {
		// "AMNOTE: Does nothing" - the call surviving is the slot check.
		service_->unk101();
		context.Detail("-> returned");
	});

	CS2ST_CALL(context, "INetworkServerService::unk102", {
		// Declared void( void ) now, and also documented as doing nothing.
		service_->unk102();
		context.Detail("-> returned");
	});

	context.SkipCall("INetworkServerService::~INetworkServerService",
		"would destroy an engine service the manager still holds");
	context.SkipCall("INetworkServerService::StartupServer",
		"starts a server session; the GameSessionConfiguration_t it takes is "
		"now fully defined in the SDK, but a second startup against the live "
		"engine is still not this suite's to attempt");
	context.SkipCall("INetworkServerService::SetGameSpawnGroupMgr",
		"replaces the spawn group manager the engine is using");
	context.SkipCall("INetworkServerService::AddServerPrerequisites",
		"registers load prerequisites; the configuration type is now defined, "
		"but registering prerequisites into a live load is not this suite's to attempt");
	context.SkipCall("INetworkServerService::DisconnectGameNow",
		"disconnects the running game; hooked instead");
	context.SkipCall("INetworkServerService::PrintSpawnGroupStatus",
		"writes an unbounded dump to the game console rather than returning it");
	context.SkipCall("INetworkServerService::SetGameLoadFailed",
		"marks the level load as failed; hooked instead");
	context.SkipCall("INetworkServerService::SetGameLoadStarted",
		"moves the load state machine; hooked instead");
	context.SkipCall("INetworkServerService::StartChangeLevel",
		"begins a map change; hooked instead");
	context.SkipCall("INetworkServerService::FinishChangeLevel",
		"completes a map change; the engine calls it against its own "
		"changelevel state, which this suite has none of");
	context.SkipCall("INetworkServerService::PreserveSteamID",
		"changes how the next session's identity is assigned; hooked instead");
	context.SkipCall("INetworkServerService::IsSaveRestoreAllowed",
		"the rework-typed overload takes a CUtlString* reason out-parameter; "
		"the INetworkGameServer row below records why even a null one is not "
		"probed");
	context.SkipCall("INetworkServerService::EnterSyncInterval",
		"begins a serializer sync window; previously unk104");
	context.SkipCall("INetworkServerService::ExitSyncInterval",
		"ends that window; previously unk105");
}

// -----------------------------------------------------------------------------
//  INetworkGameServer
//
//  This is where the cross-checks live. Almost nothing here is a fact this
//  interface owns - the tick, the client limit, the map name and the globals
//  pointer are all also reachable through IVEngineServer2 or CGlobalVars, and
//  that is exactly what makes them checkable.
// -----------------------------------------------------------------------------

void NetworkServerSuite::ExerciseGameServer(TestContext& context)
{
	if (game_server_ == nullptr)
	{
		context.UnavailableCall("INetworkGameServer",
			"GetIGameServer returned null; the engine has no level loaded, which "
			"is a normal state and not a failure");
		return;
	}

	context.Note("[INetworkGameServer] cross-checked against IVEngineServer2 and CGlobalVars");

	CS2ST_CALL(context, "INetworkGameServer::GetGlobals", {
		// The sharpest row in the suite. Two unrelated interfaces are supposed
		// to hand out the same CGlobalVars singleton, and they reach it down
		// vtables that share no code. Equal addresses prove both slots at once;
		// unequal ones mean one of the two is not the method its name says.
		CGlobalVars* from_server = game_server_->GetGlobals();
		context.Detail("-> %p (IVEngineServer2::GetServerGlobals gave %p)",
			static_cast<void*>(from_server), static_cast<void*>(globals_));

		if (from_server == nullptr)
		{
			context.Fail("a running game server has globals");
		}
		else if (globals_ != nullptr && from_server != globals_)
		{
			context.Fail("two interfaces disagree about where CGlobalVars is");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetServerTick", {
		const int tick = game_server_->GetServerTick();
		if (globals_ == nullptr)
		{
			context.Detail("-> %d (no CGlobalVars to compare against)", tick);
			return;
		}
		const int from_globals = globals_->tickcount;
		context.Detail("-> %d (CGlobalVars::tickcount %d)", tick, from_globals);

		const int drift = tick > from_globals ? tick - from_globals : from_globals - tick;
		if (drift > kTickSlack)
		{
			context.Fail("the two tick counters are %d apart; a sweep runs inside "
			             "one engine frame, so they cannot legitimately diverge", drift);
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetMaxClients", {
		const int max_clients = game_server_->GetMaxClients();
		if (globals_ == nullptr)
		{
			context.Detail("-> %d (no CGlobalVars to compare against)", max_clients);
			return;
		}
		context.Detail("-> %d (CGlobalVars::maxClients %d)", max_clients, globals_->maxClients);
		if (max_clients != globals_->maxClients)
		{
			context.Fail("the server and the globals disagree about the client limit");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetMapName", {
		const char* map_name = game_server_->GetMapName();
		if (globals_ == nullptr)
		{
			context.Detail("-> \"%s\" (no CGlobalVars to compare against)",
				map_name != nullptr ? map_name : "(null)");
			return;
		}
		const char* from_globals = globals_->mapname.ToCStr();
		context.Detail("-> \"%s\" (CGlobalVars::mapname \"%s\")",
			map_name != nullptr ? map_name : "(null)", from_globals);

		if (map_name == nullptr)
		{
			context.Fail("a loaded level has a map name");
		}
		else if (std::strcmp(map_name, from_globals) != 0)
		{
			context.Fail("the server and the globals disagree about the map");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::IsPaused", {
		const bool paused = game_server_->IsPaused();
		IVEngineServer2* engine = Engine(context);
		if (engine == nullptr)
		{
			context.Detail("-> %s (no engine interface to compare against)",
				paused ? "true" : "false");
			return;
		}
		const bool engine_paused = engine->IsPaused();
		context.Detail("-> %s (ISource2Engine::IsPaused %s)",
			paused ? "true" : "false", engine_paused ? "true" : "false");
		if (paused != engine_paused)
		{
			context.Fail("two interfaces disagree about whether the game is paused");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::IsActive", {
		context.Detail("-> %s", game_server_->IsActive() ? "true" : "false");
	});

	// New in this SDK revision (iserver.h): the enum-based counterpart to the
	// IsActive/IsPaused bools. Three routes to one fact - the state enum, and
	// the two getters whose entire job is to compare against two of its
	// values. A disagreement is a Fail because both sides are declarations
	// this same header makes; the fifth enum value (SS_WaitingForGameSessionManifest)
	// has no bool to compare against and is only recorded.
	CS2ST_CALL(context, "INetworkGameServer::GetServerState", {
		const server_state_t state = game_server_->GetServerState();
		const char* name = "outside server_state_t";
		switch (state)
		{
		case SS_Dead: name = "SS_Dead"; break;
		case SS_WaitingForGameSessionManifest:
			name = "SS_WaitingForGameSessionManifest"; break;
		case SS_Loading: name = "SS_Loading"; break;
		case SS_Active: name = "SS_Active"; break;
		case SS_Paused: name = "SS_Paused"; break;
		}
		const bool active = game_server_->IsActive();
		const bool paused = game_server_->IsPaused();
		context.Detail("-> %d (%s); IsActive %s, IsPaused %s",
			static_cast<int>(state), name,
			active ? "true" : "false", paused ? "true" : "false");
		if (state != SS_Active && active)
		{
			context.Fail("IsActive is true while GetServerState is not SS_Active");
		}
		if (state != SS_Paused && paused)
		{
			context.Fail("IsPaused is true while GetServerState is not SS_Paused");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetTime", {
		// Deliberately one engine call and nothing else. An earlier version of
		// this row also read GetServerTick() and ISource2ServerConfig's
		// GetTickInterval() inside the same guarded block, which made every
		// crash in it unattributable - three suspects, one row. The formula
		// (tick * interval) lives in ServerConfigSuite instead, where both of
		// its sides get their own guarded rows.
		const float time = game_server_->GetTime();
		context.Detail("-> %.4f", static_cast<double>(time));
		if (!std::isfinite(time))
		{
			context.Fail("a time that is not finite is not a number this "
			             "function can have produced");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetTimescale", {
		const float timescale = game_server_->GetTimescale();
		context.Detail("-> %.4f", static_cast<double>(timescale));
		// Same reasoning as ISource2Engine::GetTimescale in the engine suite: a
		// value outside this means a float was read out of the wrong place.
		if (!(timescale > 0.0f && timescale <= 100.0f))
		{
			context.Fail("implausible timescale");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetAddonName", {
		const char* addon = game_server_->GetAddonName();
		context.Detail("-> \"%s\"", addon != nullptr ? addon : "(null)");
	});

	CS2ST_CALL(context, "INetworkGameServer::IsBackgroundMap", {
		context.Detail("-> %s", game_server_->IsBackgroundMap() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkGameServer::IsChangelevelPending", {
		context.Detail("-> %s", game_server_->IsChangelevelPending() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkGameServer::IsRecordingDemo", {
		context.Detail("-> %s", game_server_->IsRecordingDemo() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkGameServer::FindSpawnGroupByName", {
		// Both interfaces are asked for the same nonexistent name. Neither
		// answer means anything alone - a handle for a group nobody loaded is
		// whatever the engine uses for "no" - but the two have to agree.
		const SpawnGroupHandle_t here = game_server_->FindSpawnGroupByName(kNoSuchSpawnGroup);
		IVEngineServer2* engine = Engine(context);
		if (engine == nullptr)
		{
			context.Detail("-> %u (no engine interface to compare against)",
				static_cast<unsigned>(here));
			return;
		}
		const SpawnGroupHandle_t there = engine->FindSpawnGroupByName(kNoSuchSpawnGroup);
		context.Detail("-> %u (IVEngineServer2 gave %u) for a nonexistent name",
			static_cast<unsigned>(here), static_cast<unsigned>(there));
		if (here != there)
		{
			context.Fail("two interfaces answer differently for the same name");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetSpawnGroupLoadingStatus", {
		context.Detail("-> %d for handle %u",
			game_server_->GetSpawnGroupLoadingStatus(kUnknownSpawnGroup),
			static_cast<unsigned>(kUnknownSpawnGroup));
	});

	CS2ST_CALL(context, "INetworkGameServer::GetAllLoadingSpawnGroups", {
		// The vector is ours and goes through g_pMemAlloc, which resolves to
		// tier0's single shared allocator in both this module and the engine -
		// so the engine growing it and this function destroying it is not a
		// cross-allocator free. (Same reasoning as CUtlTSHash::GetElements in
		// the schema suite; unrelated to memoverride.cpp, which only covers
		// global new/delete.)
		CUtlVector<SpawnGroupHandle_t> loading;
		UtlVectorGuard<decltype(loading)> loading_guard(loading);
		game_server_->GetAllLoadingSpawnGroups(&loading);
		const int count = loading.Count();
		context.Detail("-> %d group(s) loading", count);
		if (count < 0)
		{
			context.Fail("a count cannot be negative");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetGameSessionConfig", {
		// The SDK used to forward-declare GameSessionConfiguration_t and
		// nothing more, so the pointer was all this row could report. The Oct
		// 2026 update ships the definition (public/engine2/
		// gamesessionconfiguration.h: CBaseCmdKeyValues<CSVCMsg_...>), so the
		// struct can now be walked as one - the same layout-vs-bytes check
		// netadr_t gets below. It is a protobuf message under an engine
		// header, so only the read-only probes apply.
		const GameSessionConfiguration_t* config = game_server_->GetGameSessionConfig();
		context.Detail("-> %p", static_cast<const void*>(config));
		if (config != nullptr && LooksLikeObjectPointer(config))
		{
			StructInspector inspect(context, "GameSessionConfiguration_t",
				config, sizeof(GameSessionConfiguration_t));
			// CBaseCmdKeyValues<T> adds m_pKeyValues after T's own fields; the
			// protobuf's vtable pointer sits at the front. Pointer probe only:
			// null is legal (the engine may never have parsed one), which is
			// also why the member is protected and reached through the capture.
			inspect.Pointer("m_pKeyValues",
				PrivateFieldOffset<GameSessionConfigKeyValues>(
					static_cast<const GameSessionConfigOwner*>(config)));
			inspect.Summarize();
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetClientConnectionType", {
		if (globals_ == nullptr || globals_->maxClients <= kProbeSlot)
		{
			context.Detail("-> slot %d is not known to be in range; not asked",
				kProbeSlot);
			return;
		}
		const uint8 type = game_server_->GetClientConnectionType(CPlayerSlot(kProbeSlot));
		context.Detail("-> %u for slot %d", static_cast<unsigned>(type), kProbeSlot);
	});

	CS2ST_CALL(context, "INetworkGameServer::GetClientSignonState", {
		if (globals_ == nullptr || globals_->maxClients <= kProbeSlot)
		{
			context.Detail("-> slot %d is not known to be in range; not asked", kProbeSlot);
			return;
		}
		// A read on a slot that may be empty; the state enum is opaque in the
		// SDK, so it goes through int. Compared against the engine's own getter.
		const int here = static_cast<int>(game_server_->GetClientSignonState(CPlayerSlot(kProbeSlot)));
		IVEngineServer2* engine = Engine(context);
		if (engine == nullptr)
		{
			context.Detail("-> %d for slot %d (no engine interface to compare against)",
				here, kProbeSlot);
			return;
		}
		const int there = static_cast<int>(engine->GetClientSignonState(CPlayerSlot(kProbeSlot)));
		context.Detail("-> %d (IVEngineServer2 gave %d) for slot %d", here, there, kProbeSlot);
		if (here != there)
		{
			context.Suspect("two interfaces disagree about the signon state of slot %d",
				kProbeSlot);
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::IsMultiplayer", {
		// The header: is_multiplayer of the session the server started with -
		// the same fact INetworkServerService::IsMultiplayer reports.
		const bool here = game_server_->IsMultiplayer();
		const bool there = service_->IsMultiplayer();
		context.Detail("-> %s (INetworkServerService gave %s)",
			here ? "true" : "false", there ? "true" : "false");
		if (here != there)
		{
			context.Suspect("the game server and the service disagree about multiplayer");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::IsReserved", {
		context.Detail("-> %s", game_server_->IsReserved() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkGameServer::IsPlayingSoloAgainstBots", {
		context.Detail("-> %s", game_server_->IsPlayingSoloAgainstBots() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkGameServer::HasPendingServerGameFrameJob", {
		// Where in the frame a sweep runs decides this answer, so it is recorded
		// and never judged.
		context.Detail("-> %s", game_server_->HasPendingServerGameFrameJob() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkGameServer::GetAverageFrameTime", {
		const float frame_time = game_server_->GetAverageFrameTime();
		context.Detail("-> %.6f", static_cast<double>(frame_time));
		if (!std::isfinite(frame_time) || frame_time < 0.0f)
		{
			context.Fail("a frame time is a finite, non-negative duration");
		}
	});

	CS2ST_CALL(context, "INetworkGameServer::GetGameServerSteamID", {
		// Compared with the engine's own getter and never printed: the id is a
		// stable identifier and logs get pasted into bug reports.
		const CSteamID steam_id = game_server_->GetGameServerSteamID();
		IVEngineServer2* engine = Engine(context);
		if (engine == nullptr)
		{
			context.Detail("-> valid=%s (no engine interface to compare against)",
				steam_id.IsValid() ? "true" : "false");
			return;
		}
		const CSteamID engine_id = engine->GetGameServerSteamID();
		const bool same = steam_id.ConvertToUint64() == engine_id.ConvertToUint64();
		context.Detail("-> valid=%s, %s IVEngineServer2's (id not logged)",
			steam_id.IsValid() ? "true" : "false", same ? "equals" : "differs from");
		if (!same)
		{
			context.Suspect("the game server and the engine report different server steam ids");
		}
	});
}

// -----------------------------------------------------------------------------
//  CNetworkGameServerBase
//
//  ⚠️ The SDK declares `virtual ~CNetworkGameServerBase() = 0;` *first* in this
//  class. Under the Itanium ABI a destructor takes two vtable slots at the
//  position it is declared, so everything this class adds sits two slots later
//  than a naive count of INetworkGameServer's methods would suggest. The first
//  method here that is safe to call is GetPlayerUserId - everything before it
//  either mutates or is an engine-to-server callback - which makes it this
//  section's canary: if the destructor slots are not where the SDK thinks, that
//  call does not come back.
// -----------------------------------------------------------------------------

void NetworkServerSuite::ExerciseGameServerBase(TestContext& context)
{
	if (game_server_ == nullptr)
	{
		context.UnavailableCall("CNetworkGameServerBase",
			"no game server object to call anything on");
		return;
	}

	context.Note("[CNetworkGameServerBase] past two destructor slots; "
	             "GetPlayerUserId is the canary");

	const bool slot_in_range = globals_ != nullptr && globals_->maxClients > kProbeSlot;

	CS2ST_CALL(context, "CNetworkGameServerBase::GetPlayerUserId", {
		if (!slot_in_range)
		{
			context.Detail("-> slot %d is not known to be in range; not asked", kProbeSlot);
			return;
		}
		const CPlayerUserId here = game_server_->GetPlayerUserId(CPlayerSlot(kProbeSlot));
		IVEngineServer2* engine = Engine(context);
		if (engine == nullptr)
		{
			context.Detail("-> %d (no engine interface to compare against)", here.Get());
			return;
		}
		const CPlayerUserId there = engine->GetPlayerUserId(CPlayerSlot(kProbeSlot));
		context.Detail("-> %d (IVEngineServer2 gave %d) for slot %d",
			here.Get(), there.Get(), kProbeSlot);
		if (here.Get() != there.Get())
		{
			context.Fail("two interfaces disagree about the user id in slot %d", kProbeSlot);
		}
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::GetPlayerNetworkIDString", {
		if (!slot_in_range)
		{
			context.Detail("-> slot %d is not known to be in range; not asked", kProbeSlot);
			return;
		}
		const char* here = game_server_->GetPlayerNetworkIDString(CPlayerSlot(kProbeSlot));
		IVEngineServer2* engine = Engine(context);
		if (engine == nullptr)
		{
			context.Detail("-> \"%s\" (no engine interface to compare against)",
				here != nullptr ? here : "(null)");
			return;
		}
		const char* there = engine->GetPlayerNetworkIDString(CPlayerSlot(kProbeSlot));
		// IVEngineServer2 answers "BOT" itself for a fake client. The base-class
		// getter (libengine2, reading CS2 b26087496) instead returns the string
		// pointer stored in the client object, which a fake client never sets,
		// so what it hands back is stale memory that differs every run - not a
		// network id, and not worth printing or comparing.
		if (there != nullptr && std::strcmp(there, "BOT") == 0)
		{
			context.Detail("-> slot %d is a bot (IVEngineServer2 gave \"BOT\"); the base "
			               "getter reads an id string a fake client never sets, so its "
			               "answer is not compared", kProbeSlot);
			return;
		}

		context.Detail("-> \"%s\" (IVEngineServer2 gave \"%s\")",
			here != nullptr ? here : "(null)", there != nullptr ? there : "(null)");

		// Suspect and not Fail: a real client's two answers should agree, but
		// the SDK does not promise it, so a disagreement goes yellow, not red.
		// The user-id row above keeps its Fail: its two sides do agree.
		if ((here == nullptr) != (there == nullptr))
		{
			context.Suspect("one interface has a network id for slot %d and the other "
			                "does not", kProbeSlot);
		}
		else if (here != nullptr && std::strcmp(here, there) != 0)
		{
			context.Suspect("two interfaces disagree about the network id in slot %d",
				kProbeSlot);
		}
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::GetHostName", {
		const char* host_name = game_server_->GetHostName();
		ICvar* cvar = Cvar(context);
		if (cvar == nullptr || host_name == nullptr)
		{
			context.Detail("-> \"%s\"", host_name != nullptr ? host_name : "(null)");
			if (host_name == nullptr)
			{
				context.Fail("a running server has a hostname; the browser shows it");
			}
			return;
		}

		ConVarRefAbstract hostname_cvar(cvar->FindConVar("hostname", true));
		if (!hostname_cvar.IsConVarDataAvailable())
		{
			context.Detail("-> \"%s\" (the hostname convar is not readable)", host_name);
			return;
		}

		CBufferStringN<256> value(false);
		hostname_cvar.GetValueAsString(value);
		context.Detail("-> \"%s\" (hostname convar \"%s\")", host_name, value.Get());

		if (std::strcmp(host_name, value.Get()) != 0)
		{
			// Suspect and not Fail: the SDK never says this getter reads that
			// convar. It plainly does on every build anyone has looked at, but
			// "plainly" is not a contract, and the row is just as useful yellow.
			context.Suspect("the server's hostname and the hostname convar differ");
		}
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::GetPassword", {
		// The value is never logged. The SDK documents the *shape* of the
		// answer - null when sv_password is unset or literally "none" - and
		// that is the whole of what a diagnostic needs to check. Logs get
		// pasted into bug reports.
		const char* password = game_server_->GetPassword();
		ICvar* cvar = Cvar(context);
		if (cvar == nullptr)
		{
			context.Detail("-> %s", password == nullptr ? "(null)" : "set (not logged)");
			return;
		}

		ConVarRefAbstract password_cvar(cvar->FindConVar("sv_password", true));
		if (!password_cvar.IsConVarDataAvailable())
		{
			context.Detail("-> %s (sv_password is not readable)",
				password == nullptr ? "(null)" : "set (not logged)");
			return;
		}

		CBufferStringN<128> value(false);
		password_cvar.GetValueAsString(value);
		const bool cvar_says_none = std::strcmp(value.Get(), "none") == 0;

		context.Detail("-> %s, sv_password is %s",
			password == nullptr ? "(null)" : "set (not logged)",
			cvar_says_none ? "\"none\"" : (value.Get()[0] == '\0' ? "empty" : "set (not logged)"));

		// The one documented rule, and only that one.
		if (cvar_says_none && password != nullptr)
		{
			context.Fail("the SDK says \"none\" comes back as null");
		}
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::GetUDPPort", {
		const uint16 port = game_server_->GetUDPPort();
		context.Detail("-> %u", static_cast<unsigned>(port));
		// Cross-checked against netadr_t::GetPort in InspectNetworkAddress,
		// where the address is in hand.
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::GetNetStats", {
		// Seeded with values the engine has to overwrite, so "did not write" is
		// distinguishable from "wrote a zero".
		float inflow = -31337.0f;
		float outflow = -31337.0f;
		game_server_->GetNetStats(inflow, outflow);
		context.Detail("-> in %.2f, out %.2f",
			static_cast<double>(inflow), static_cast<double>(outflow));

		if (inflow == -31337.0f && outflow == -31337.0f)
		{
			context.Fail("neither out parameter was written");
		}
		else if (!std::isfinite(inflow) || !std::isfinite(outflow))
		{
			context.Fail("a byte rate that is not finite is not a byte rate");
		}
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::IsHLTV", {
		context.Detail("-> %s", game_server_->IsHLTV() ? "true" : "false");
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::IsPausable", {
		context.Detail("-> %s for pause group %d",
			game_server_->IsPausable(kProbePauseGroup) ? "true" : "false",
			static_cast<int>(kProbePauseGroup));
	});

	context.SkipCall("CNetworkGameServerBase::~CNetworkGameServerBase",
		"would destroy the running game server");
	context.SkipCall("CNetworkGameServerBase::SetMaxClients", "changes the slot count live");
	context.SkipCall("CNetworkGameServerBase::CreateClient",
		"creates a client in a slot; named in the 2026-10 SDK, previously unk011");
	context.SkipCall("CNetworkGameServerBase::ProcessConnectionlessPacket",
		"would feed the engine a packet nobody sent");
	context.SkipCall("CNetworkGameServerBase::OnConVarCreated",
		"an engine-to-server callback; calling it would announce a convar that "
		"was never created");
	context.SkipCall("CNetworkGameServerBase::OnConCommandCreated",
		"the same for console commands");
	context.SkipCall("CNetworkGameServerBase::FillKV3ServerInfo",
		"needs a KeyValues3 to fill, and tier0_perproject/keyvalues3.cpp is not in this "
		"build (CS2SDKTEST_TIER1_KEYVALUES3 is off)");
	context.SkipCall("CNetworkGameServerBase::FillServerInfo",
		"needs a CSVCMsg_ServerInfo_t, and the netmessages protobufs are not "
		"among CS2SDKTEST_PROTO_FILES");
	context.SkipCall("CNetworkGameServerBase::RemoveClientFromGame", "kicks a player");
	context.SkipCall("CNetworkGameServerBase::UserInfoChanged",
		"tells the engine a player's userinfo changed when it did not");
	context.SkipCall("CNetworkGameServerBase::GetClassBaseline",
		"needs a ServerClass to ask about; the list is the game DLL's and there "
		"is no read-only way to pick one that is guaranteed live");
	context.SkipCall("CNetworkGameServerBase::StartHLTVMaster", "starts an HLTV master");
	context.SkipCall("CNetworkGameServerBase::ConnectClient", "connects a client");
	context.SkipCall("CNetworkGameServerBase::CreateNewClient", "allocates a client slot");
	context.SkipCall("CNetworkGameServerBase::FinishCertificateCheck",
		"drives Steam authentication for a connection in progress");
	context.SkipCall("CNetworkGameServerBase::GetChallengeType",
		"needs an ns_address for a connecting client; a made-up one would ask "
		"the engine about a peer that does not exist");
	context.SkipCall("CNetworkGameServerBase::CheckPassword",
		"probing an authentication path is not something a diagnostic sweep does");
	context.SkipCall("CNetworkGameServerBase::CalculateCPUUsage",
		"recomputes engine profiling state out of band");
}

// -----------------------------------------------------------------------------
//  netadr_t - the one struct here with a layout worth checking
//
//  Better than it looks. Its fields are public and its accessors live in tier0,
//  so reading `port` through offsetof and calling GetPort() are two descriptions
//  of one byte pair that share no code - agreement is a cross-module ABI check,
//  not a self-consistency check. Two of the accessors even come with their rule
//  written down in the header, which turns them into oracles rather than
//  readings.
// -----------------------------------------------------------------------------

void NetworkServerSuite::InspectNetworkAddress(TestContext& context)
{
	if (game_server_ == nullptr)
	{
		context.SkipCall("netadr_t", "no game server to ask for an address");
		return;
	}

	context.Note("[netadr_t] %zu bytes as the SDK declares it", sizeof(netadr_t));

	// Raw storage rather than a netadr_t local on purpose: netadr_t's default
	// constructor calls three tier0 functions, and a declaration at function
	// scope would run them outside the fault guard. Zero-initialised storage
	// runs nothing, and the copy below happens inside a guarded call. netadr_t
	// is trivially copyable, so memcpy is the right way in.
	alignas(netadr_t) unsigned char storage[sizeof(netadr_t)] = {};
	const netadr_t* address = reinterpret_cast<const netadr_t*>(storage);
	bool have_address = false;

	CS2ST_CALL(context, "INetworkGameServer::GetServerNetworkAddress", {
		const netadr_t returned = game_server_->GetServerNetworkAddress();
		std::memcpy(storage, &returned, sizeof(storage));
		have_address = true;
		context.Detail("-> returned by value into %zu bytes", sizeof(storage));
	});

	if (!have_address)
	{
		context.SkipCall("netadr_t", "GetServerNetworkAddress did not return");
		return;
	}

	StructInspector inspect(context, "netadr_t", address, sizeof(netadr_t));
	// Declaration order, offsets from offsetof - see StructInspector.h for why
	// a hand-written number would silently stop tracking a submodule bump.
	inspect.Enum("type", offsetof(netadr_t, type), NA_NULL, NA_IP);
	inspect.Raw ("ip",   offsetof(netadr_t, ip), sizeof(netadr_t::ip));
	inspect.Raw ("port", offsetof(netadr_t, port), sizeof(netadr_t::port));
	inspect.Summarize();
	inspect.HexDump();

	// ---- the same three fields, read back through tier0 ---------------------

	const unsigned char* ip_bytes =
		reinterpret_cast<const unsigned char*>(storage) + offsetof(netadr_t, ip);
	const std::uint32_t ip_big_endian =
		(static_cast<std::uint32_t>(ip_bytes[0]) << 24)
		| (static_cast<std::uint32_t>(ip_bytes[1]) << 16)
		| (static_cast<std::uint32_t>(ip_bytes[2]) << 8)
		| static_cast<std::uint32_t>(ip_bytes[3]);
	const std::uint32_t ip_little_endian = ByteSwap32(ip_big_endian);

	std::uint16_t port_field = 0;
	std::memcpy(&port_field, storage + offsetof(netadr_t, port), sizeof(port_field));

	CS2ST_CALL(context, "netadr_t::GetType vs the type field", {
		const netadrtype_t from_tier0 = address->GetType();
		netadrtype_t from_field = NA_NULL;
		std::memcpy(&from_field, storage + offsetof(netadr_t, type), sizeof(from_field));
		context.Detail("tier0 -> %s (%d), field -> %s (%d)",
			NetAdrTypeName(from_tier0), static_cast<int>(from_tier0),
			NetAdrTypeName(from_field), static_cast<int>(from_field));
		if (from_tier0 != from_field)
		{
			context.Fail("tier0's accessor and our offsetof read different bytes");
		}
	});

	CS2ST_CALL(context, "netadr_t::GetPort vs the port field", {
		const unsigned short from_tier0 = address->GetPort();
		context.Detail("tier0 -> %u, field -> %u",
			static_cast<unsigned>(from_tier0), static_cast<unsigned>(port_field));
		if (from_tier0 != port_field)
		{
			context.Fail("tier0's accessor and our offsetof read different bytes");
		}
	});

	CS2ST_CALL(context, "netadr_t::GetIPHostByteOrder / GetIPNetworkByteOrder vs ip[4]", {
		// netadr_t::GetIPNetworkByteOrder is the other half of this row.
		// Which of the two is which is not asserted - that would be guessing at
		// the host's endianness on the SDK's behalf. What is asserted is the
		// pair: between them they must be the two byte orderings of the four
		// bytes sitting at offsetof(netadr_t, ip), in some order.
		const std::uint32_t host = address->GetIPHostByteOrder();
		const std::uint32_t network = address->GetIPNetworkByteOrder();
		context.Detail("host 0x%08lx, network 0x%08lx, bytes %u.%u.%u.%u",
			static_cast<unsigned long>(host), static_cast<unsigned long>(network),
			ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);

		const bool matches = (host == ip_big_endian && network == ip_little_endian)
			|| (host == ip_little_endian && network == ip_big_endian);
		if (!matches)
		{
			context.Fail("neither ordering of the ip bytes produces what tier0 returned");
		}
		else if (ip_big_endian == 0)
		{
			// All-zero bytes make both orderings identical, so the check above
			// passes for free. Said out loud rather than counted as a pass.
			context.Detail("(the address is 0.0.0.0, so both orderings agree "
			               "trivially and this row proves nothing today)");
		}
	});

	CS2ST_CALL(context, "netadr_t::IsValid (the SDK states: ip & port != 0)", {
		const bool from_tier0 = address->IsValid();
		const bool from_rule = (ip_big_endian != 0) && (port_field != 0);
		context.Detail("tier0 -> %s, the stated rule -> %s",
			from_tier0 ? "true" : "false", from_rule ? "true" : "false");
		if (from_tier0 != from_rule)
		{
			context.Fail("tier0 disagrees with the rule the header writes for it");
		}
	});

	CS2ST_CALL(context, "netadr_t::IsBaseAdrValid (the SDK states: ip != 0)", {
		const bool from_tier0 = address->IsBaseAdrValid();
		const bool from_rule = ip_big_endian != 0;
		context.Detail("tier0 -> %s, the stated rule -> %s",
			from_tier0 ? "true" : "false", from_rule ? "true" : "false");
		if (from_tier0 != from_rule)
		{
			context.Fail("tier0 disagrees with the rule the header writes for it");
		}
	});

	CS2ST_CALL(context, "netadr_t::IsLoopback / IsLocalhost / IsReservedAdr", {
		// netadr_t::IsLocalhost and netadr_t::IsReservedAdr are the other two.
		// No stated rule for these three, so they are recorded. They are still
		// worth a row: all three read the same four bytes, and three answers
		// that stop making sense together is a signal even without a contract.
		context.Detail("loopback=%s localhost=%s reserved=%s",
			address->IsLoopback() ? "yes" : "no",
			address->IsLocalhost() ? "yes" : "no",
			address->IsReservedAdr() ? "yes" : "no");
	});

	CS2ST_CALL(context, "netadr_t::CompareAdr (against itself)", {
		// An address is itself. One correct answer, and both halves of the
		// comparison come from the engine rather than from us.
		const bool same = address->CompareAdr(*address, false);
		const bool same_base = address->CompareAdr(*address, true);
		context.Detail("full -> %s, base only -> %s",
			same ? "true" : "false", same_base ? "true" : "false");
		if (!same || !same_base)
		{
			context.Fail("an address does not compare equal to itself");
		}
	});

	CS2ST_CALL(context, "netadr_t::operator== (against itself)", {
		// The inline operator forwards to CompareAdr, so this row is thin - but
		// it is the operator plugins actually write, and "thin" is not "absent".
		const bool same = (*address == *address);
		context.Detail("-> %s", same ? "true" : "false");
		if (!same)
		{
			context.Fail("an address does not compare equal to itself");
		}
	});

	CS2ST_CALL(context, "netadr_t::operator< (against itself)", {
		// Irreflexivity is a property of the ordering itself, not of this
		// address: nothing sorts before itself, whatever the bytes are. That
		// makes it one of the few checks here with an answer known in advance.
		const bool less = (*address < *address);
		context.Detail("-> %s", less ? "true" : "false");
		if (less)
		{
			context.Fail("an address sorts before itself; this is not a strict ordering");
		}
	});

	CS2ST_CALL(context, "netadr_t::ToString (both overloads)", {
		// Documented to render as xxx.xxx.xxx.xxx:ppppp, so the full form has
		// to carry a colon and the base form has to not. Weak on its own, which
		// is why it is the last row rather than the headline - but the two
		// overloads rendering the same address differently would not be weak.
		const char* full = address->ToString(false);
		const char* base = address->ToString(true);

		// Stack buffer: a fault mid-call skips every destructor between there
		// and the guard, and a stack-only buffer cannot leak.
		char buffer[64] = {};
		address->ToString(buffer, static_cast<uint32>(sizeof(buffer)), false);

		context.Detail("full \"%s\", base \"%s\", into a buffer \"%s\"",
			full != nullptr ? full : "(null)", base != nullptr ? base : "(null)", buffer);

		if (full == nullptr || base == nullptr)
		{
			context.Fail("ToString returned null");
		}
		else if (std::strchr(full, ':') == nullptr)
		{
			context.Fail("the documented rendering includes \":ppppp\"");
		}
		else if (buffer[0] == '\0')
		{
			context.Fail("the buffer overload wrote nothing where the other rendered "
			             "\"%s\"", full);
		}
		else if (std::strcmp(buffer, full) != 0)
		{
			// Same address, same flag, two renderings. Nothing in the header
			// says the buffer overload must agree with the returning one, so
			// this is recorded rather than failed - but they are the same
			// function's two entry points and a difference is worth a colour.
			context.Suspect("the two ToString overloads rendered the same address "
			                "differently");
		}
	});

	CS2ST_CALL(context, "CNetworkGameServerBase::GetUDPPort vs netadr_t::GetPort", {
		// Two routes to the port: the server object's own getter, and the port
		// inside the address it just handed out.
		const uint16 from_server = game_server_->GetUDPPort();
		const unsigned short from_address = address->GetPort();
		context.Detail("server -> %u, address -> %u",
			static_cast<unsigned>(from_server), static_cast<unsigned>(from_address));
		if (from_address == 0 && from_server != 0)
		{
			// GetServerNetworkAddress (libengine2, CS2 b26087496) copies the
			// 12-byte netadr_t out of the network system's own record, and that
			// record holds the type and ip with the port left at 0: the port
			// lives behind GetUDPPort, which asks the network system for it by
			// socket index. So an address with no port is how this build
			// answers, not a disagreement between the two.
			context.Detail("the address carries the ip only; the port is reported by "
			               "GetUDPPort");
		}
		else if (from_server != from_address)
		{
			// Suspect, not Fail: the SDK does not say the address this server
			// reports is the address of the socket GetUDPPort names. On a
			// single-socket dedicated server they are the same thing, but that
			// is an inference about deployment, not a declaration.
			context.Suspect("the server's UDP port and its network address disagree");
		}
	});

	// Clear, SetType, SetPort, both SetIP overloads, SetIPAndPort, SetFromString
	// (literal, no DNS), CompareClassBAdr and CompareClassCAdr are called on
	// local copies by ExerciseLocalNetworkAddress.
	context.SkipCall("netadr_t::SetFromSockadr", "needs a sockaddr we do not have");
	context.SkipCall("netadr_t::SetFromSocket", "needs a live socket handle");
	context.SkipCall("netadr_t::ToSockadr", "writes into a sockaddr we would have to build");
	context.SkipCall("netadr_t::GetIP", "an inline alias for GetIPNetworkByteOrder, covered above");
	context.SkipCall("netadr_t::Serialize",
		"needs a bf_write; tier1/bitbuf.cpp is not in this build "
		"(CS2SDKTEST_TIER1_BITBUF is off)");
	context.SkipCall("netadr_t::Unserialize", "the same buffer, and it would overwrite the address");
}

// -----------------------------------------------------------------------------
//  The inheritance shape
//
//  CNetworkGameServerBase is the only place in the SDK this project touches
//  that uses multiple inheritance, and the only place where a pointer to the
//  same object has two different values depending on which base it is viewed
//  through. Reported, never judged: the adjustment below is computed by the
//  compiler from the SDK's declaration, so there is no second source to check
//  it against - what makes it worth a row is that it is a number, and a number
//  that changes after an update is a changed declaration.
// -----------------------------------------------------------------------------

void NetworkServerSuite::NoteInheritanceShape(TestContext& context)
{
	if (game_server_ == nullptr)
	{
		context.SkipCall("CNetworkGameServerBase (inheritance shape)",
			"no game server object to measure against");
		return;
	}

	CS2ST_CALL(context, "CNetworkGameServerBase -> IConVarListener (base adjustment)", {
		// static_cast, not a cast through char* with a hand-added offset: the
		// adjustment has to be the one the compiler derives from the header,
		// because that assumption is precisely what is being reported.
		IConVarListener* as_listener = static_cast<IConVarListener*>(game_server_);
		const std::ptrdiff_t delta = reinterpret_cast<const char*>(as_listener)
			- reinterpret_cast<const char*>(game_server_);

		context.Detail("server %p, as IConVarListener %p, delta %+ld bytes",
			static_cast<void*>(game_server_), static_cast<void*>(as_listener),
			static_cast<long>(delta));

		if (delta == 0)
		{
			// Would mean the SDK's declaration no longer puts two polymorphic
			// bases ahead of IConVarListener. Not wrong by itself, but it is a
			// shape change and the log should say so.
			context.Detail("(no adjustment: IConVarListener is now the primary base "
			               "shape, which the current declaration does not produce)");
		}
	});

	// IConnectionlessPacketHandler is a *protected* base, so there is no cast to
	// it from out here and no adjustment to measure. Its one virtual is
	// re-declared on CNetworkGameServerBase and accounted for there.
	context.SkipCall("IConnectionlessPacketHandler::~IConnectionlessPacketHandler",
		"a protected base of CNetworkGameServerBase; not reachable from outside "
		"the class, and destroying it would destroy the server");
	context.SkipCall("IConnectionlessPacketHandler::ProcessConnectionlessPacket",
		"same base; the reachable declaration is CNetworkGameServerBase's");
	context.SkipCall("IConVarListener::OnConVarCreated",
		"an engine-to-listener callback; the reachable declaration is "
		"CNetworkGameServerBase's");
	context.SkipCall("IConVarListener::OnConCommandCreated", "the same for commands");
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void NetworkServerSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[INetworkGameServer] not called - would change server state");

	context.SkipCall("INetworkGameServer::Init", "re-initialises the game server");
	context.SkipCall("INetworkGameServer::Shutdown", "shuts the game server down");
	context.SkipCall("INetworkGameServer::AddRef",
		"refcount; only safe paired with Release, and an unbalanced pair either "
		"leaks the server or frees it out from under the engine");
	context.SkipCall("INetworkGameServer::Release", "the other half of that pair");
	context.SkipCall("INetworkGameServer::SetGameSpawnGroupMgr",
		"replaces the spawn group manager");
	context.SkipCall("INetworkGameServer::SetGameSessionManifest",
		"replaces the resource manifest the session loads from");
	context.SkipCall("INetworkGameServer::RegisterLoadingSpawnGroups",
		"adds to the set of groups the engine is waiting on");
	context.SkipCall("INetworkGameServer::SetServerTick", "moves the game clock");
	context.SkipCall("INetworkGameServer::ServerAdvanceTick", "advances the simulation");
	context.SkipCall("INetworkGameServer::ServerPollNetworking", "drives the network loop");
	context.SkipCall("INetworkGameServer::ServerProcessNetworking", "drives the network loop");
	context.SkipCall("INetworkGameServer::ServerSimulate", "runs a simulation step");
	context.SkipCall("INetworkGameServer::ServerEndSimulate", "ends a simulation step");
	context.SkipCall("INetworkGameServer::ServerPostSimulate", "runs post-simulation work");
	context.SkipCall("INetworkGameServer::PreWorldUpdate", "drives the frame");
	context.SkipCall("INetworkGameServer::DirectUpdate", "drives the frame");
	context.SkipCall("INetworkGameServer::LoadSpawnGroup", "loads level content");
	context.SkipCall("INetworkGameServer::AsyncUnloadSpawnGroup", "unloads level content");
	context.SkipCall("INetworkGameServer::MakeSpawnGroupActive", "changes what is live");
	context.SkipCall("INetworkGameServer::SynchronouslySpawnGroup", "spawns entities");
	context.SkipCall("INetworkGameServer::SetSpawnGroupDescription", "mutates spawn group state");
	context.SkipCall("INetworkGameServer::PrintSpawnGroupStatus",
		"writes an unbounded dump to the game console rather than returning it");
	context.SkipCall("INetworkGameServer::IsSaveRestoreAllowed", "causes a crash if called");
	context.SkipCall("INetworkGameServer::SetMapName", "renames the running level");
	context.SkipCall("INetworkGameServer::ActivateServer", "activates the server session");
	context.SkipCall("INetworkGameServer::PrepareForAssetLoad", "moves the load state machine");
	context.SkipCall("INetworkGameServer::SetServerState", "forces a server state transition");
	context.SkipCall("INetworkGameServer::SpawnServer", "starts a new level");
	context.SkipCall("INetworkGameServer::StartChangeLevel", "begins a map change");
	context.SkipCall("INetworkGameServer::FinishChangeLevel", "completes a map change");
	context.SkipCall("INetworkGameServer::PreserveSteamID", "changes session identity handling");
	context.SkipCall("INetworkGameServer::ReserveServerForQueuedGame",
		"takes the server out of general availability");
	context.SkipCall("INetworkGameServer::BroadcastPrintf", "prints to every connected client");
	context.SkipCall("INetworkGameServer::BroadcastMessage", "sends a net message to every client");
	context.SkipCall("INetworkGameServer::BroadcastEntityVoice", "sends voice data to every client");
	// The 2026-10 SDK named these; the row labels follow, so a diff against an
	// old log shows renames rather than methods vanishing.
	context.SkipCall("INetworkGameServer::SetRecyclePlayerSlot",
		"changes slot recycling for a player slot");
	context.SkipCall("INetworkGameServer::ServerBeginSimulate",
		"runs a simulation step and queues the ServerGameFrame job on another thread");
	context.SkipCall("INetworkGameServer::SetBroadcastDebugOverlays",
		"installs a broadcast overlay object");
	context.SkipCall("INetworkGameServer::PrepareSendClientUpdatesMainThread",
		"drives the client update pass");
	context.SkipCall("INetworkGameServer::PrepareSendClientUpdatesAsync",
		"drives the client update pass");
}

// -----------------------------------------------------------------------------
//  Install / remove
//
//  Every hook is on INetworkServerService, and that is the design rather than
//  an omission. CNetworkGameServerBase is rebuilt when the level changes, so a
//  hook on it stops firing at the exact moment the interesting thing happens -
//  and a hook that has stopped firing looks identical to a method the engine no
//  longer calls, which is the one confusion this whole plugin exists to avoid.
//  The service is a process-lifetime singleton, and between them these five
//  cover the level-load lifecycle the sweep cannot see.
// -----------------------------------------------------------------------------

int NetworkServerSuite::InstallHooks(TestContext& context)
{
	if (service_ == nullptr || hooks_installed_)
	{
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before installing, so a hook that never fires still shows up in
	// the dump as `never`. After a CS2 update that row is the finding.
	recorder.Declare(kStartChangeLevel, Id());
	recorder.Declare(kSetGameLoadStarted, Id());
	recorder.Declare(kSetGameLoadFailed, Id());
	recorder.Declare(kDisconnectGameNow, Id());
	recorder.Declare(kPreserveSteamID, Id());

	// Each hook object carries its pre and post handler from the constructor;
	// Add installs the detour on the service instance, Remove takes it off.
	// KHook::Virtual::Add returns void - a failed attach surfaces later as a
	// Declare() row that never fires, which the recorder's dump already shows.
	hook_StartChangeLevel_.Add(service_);
	hook_SetGameLoadStarted_.Add(service_);
	hook_SetGameLoadFailed_.Add(service_);
	hook_DisconnectGameNow_.Add(service_);
	hook_PreserveSteamID_.Add(service_);

	hooks_installed_ = true;

	context.Note("StartChangeLevel carries the map and landmark as arguments "
	             "since the s2sdk rework, so the hook log now shows where the "
	             "change is headed");
	CS2ST_LOG_INFO("%s: the rest only fire on a level change or a disconnect - "
	               "change the map, then dump\n", Id());
	return kHookedMethodCount;
}

void NetworkServerSuite::RemoveHooks(TestContext& context)
{
	if (service_ == nullptr || !hooks_installed_)
	{
		return;
	}

	// Remove detaches from the live instance; the hook objects themselves stay
	// bound and are re-Add()ed if the suite is installed again.
	hook_StartChangeLevel_.Remove(service_);
	hook_SetGameLoadStarted_.Remove(service_);
	hook_SetGameLoadFailed_.Remove(service_);
	hook_DisconnectGameNow_.Remove(service_);
	hook_PreserveSteamID_.Remove(service_);

	hooks_installed_ = false;
	(void)context;
}

// -----------------------------------------------------------------------------
//  Hook handlers
//
//  These run in the engine's own frame, where CallGuard is not armed. POD
//  parameters need no vetting; the string arguments StartChangeLevel grew in
//  the s2sdk rework go through SafeStringArg before any %s sees them, per the
//  no-unvetted-dereference rule in HookArg.h - which is part of why these five
//  were the ones chosen.
//
//  Every one answers Action::Ignore, the KHook equivalent of MRES_IGNORED:
//  observe, never steer. The KHook::Virtual members are bound to these in the
//  constructor at the bottom of this file.
// -----------------------------------------------------------------------------

KHook::Return<void> NetworkServerSuite::Pre_StartChangeLevel(INetworkServerService*,
                                                             const char* pszMap,
                                                             const char* pszLandmark,
                                                             void* pUnknown)
{
	// Vetted before the format call, never straight off the argument: a wrong
	// signature here is a float or a bool in the register, and touching it raw
	// faults inside the callback that exists to report exactly that.
	char map_scratch[64];
	char landmark_scratch[64];
	CS2ST_HOOK_ENTER_ARGS(kStartChangeLevel,
		"map=\"%.63s\" landmark=\"%.63s\" unknown=%p",
		SafeStringArg(pszMap, map_scratch),
		SafeStringArg(pszLandmark, landmark_scratch),
		pUnknown);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Post_StartChangeLevel(INetworkServerService*,
                                                              const char* /*pszMap*/,
                                                              const char* /*pszLandmark*/,
                                                              void* /*pUnknown*/)
{
	CS2ST_HOOK_LEAVE(kStartChangeLevel);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Pre_SetGameLoadStarted(INetworkServerService*)
{
	CS2ST_HOOK_ENTER(kSetGameLoadStarted);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Post_SetGameLoadStarted(INetworkServerService*)
{
	CS2ST_HOOK_LEAVE(kSetGameLoadStarted);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Pre_SetGameLoadFailed(INetworkServerService*, bool bFailed)
{
	CS2ST_HOOK_ENTER_ARGS(kSetGameLoadFailed, "failed=%d", bFailed ? 1 : 0);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Post_SetGameLoadFailed(INetworkServerService*, bool)
{
	CS2ST_HOOK_LEAVE(kSetGameLoadFailed);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Pre_DisconnectGameNow(INetworkServerService*, ENetworkDisconnectionReason nReason)
{
	// Typed as the enum since the 2026-10 SDK (was a bare int); the protobuf
	// validator is still the honest way to judge it - the same check
	// Source2GameClientsSuite runs on ClientDisconnect, against the real value
	// set rather than a guessed range.
	const bool known = ENetworkDisconnectionReason_IsValid(static_cast<int>(nReason));
	CS2ST_HOOK_ENTER_ARGS(kDisconnectGameNow, "reason=%d%s", static_cast<int>(nReason),
		known ? "" : " NOT-A-VALID-REASON");
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Post_DisconnectGameNow(INetworkServerService*, ENetworkDisconnectionReason)
{
	CS2ST_HOOK_LEAVE(kDisconnectGameNow);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Pre_PreserveSteamID(INetworkServerService*)
{
	CS2ST_HOOK_ENTER(kPreserveSteamID);
	return { KHook::Action::Ignore };
}

KHook::Return<void> NetworkServerSuite::Post_PreserveSteamID(INetworkServerService*)
{
	CS2ST_HOOK_LEAVE(kPreserveSteamID);
	return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
//  Hook binding
//
//  The KHook::Virtual constructor resolves the vtable slot from the member
//  pointer at static-init time (the MFP's own bits name the slot), so this
//  list is the plugin's ABI contract with the SDK: the day a hook stops
//  firing after an update, these lines are what to re-check first.
// -----------------------------------------------------------------------------

// convar.h ends with `#pragma warning(default : 4355)`, which re-enables the warning
// for the rest of the translation unit and overrides the /wd4355 on the command
// line. Passing `this` as the hook context is intentional (stored, not used).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4355)
#endif
NetworkServerSuite::NetworkServerSuite() :
	hook_StartChangeLevel_(&INetworkServerService::StartChangeLevel, this,
		&NetworkServerSuite::Pre_StartChangeLevel,
		&NetworkServerSuite::Post_StartChangeLevel),
	hook_SetGameLoadStarted_(&INetworkServerService::SetGameLoadStarted, this,
		&NetworkServerSuite::Pre_SetGameLoadStarted,
		&NetworkServerSuite::Post_SetGameLoadStarted),
	hook_SetGameLoadFailed_(&INetworkServerService::SetGameLoadFailed, this,
		&NetworkServerSuite::Pre_SetGameLoadFailed,
		&NetworkServerSuite::Post_SetGameLoadFailed),
	hook_DisconnectGameNow_(&INetworkServerService::DisconnectGameNow, this,
		&NetworkServerSuite::Pre_DisconnectGameNow,
		&NetworkServerSuite::Post_DisconnectGameNow),
	hook_PreserveSteamID_(&INetworkServerService::PreserveSteamID, this,
		&NetworkServerSuite::Pre_PreserveSteamID,
		&NetworkServerSuite::Post_PreserveSteamID)
{
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace cs2st
