#include "suites/GameEventSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <appframework/iappsystem.h>
#include <const.h>
#include <engine2/igameeventsystem.h>
#include <igameevents.h>
#include <networksystem/inetworkmessages.h>

#include "core/Log.h"
#include "hook/HookArg.h"
#include "hook/HookRecorder.h"
#include "testkit/StructInspector.h"

// The SH_DECL_HOOK block used to live here; under KHook the hook templates are
// declared as members in the header instead, bound to the SDK member pointers
// in the constructor at the bottom of this file. PostEventAbstract is the one
// place that needs care: the two overloads are separate vtable slots, and the
// constructor disambiguates their member pointers with a static_cast each -
// which is also why the overload index that SH_DECL_HOOK used to carry is gone;
// the cast names the overload now.

namespace cs2st
{
namespace
{

// ---- hook ids ---------------------------------------------------------------
//
// Both the log label and the recorder key, so they are written once here.

constexpr const char* kRegisterGameEvent =
	"IGameEventSystem::RegisterGameEvent";
constexpr const char* kRegisterGameEventHandlerAbstract =
	"IGameEventSystem::RegisterGameEventHandlerAbstract";
constexpr const char* kUnregisterGameEventHandlerAbstract =
	"IGameEventSystem::UnregisterGameEventHandlerAbstract";
constexpr const char* kPostEventAbstractLocal =
	"IGameEventSystem::PostEventAbstract_Local";
constexpr const char* kPostEventAbstract =
	"IGameEventSystem::PostEventAbstract [clients]";
constexpr const char* kPostEventAbstractFiltered =
	"IGameEventSystem::PostEventAbstract [filter]";
constexpr const char* kPostEntityEventAbstract =
	"IGameEventSystem::PostEntityEventAbstract";
constexpr const char* kProcessQueuedEvents =
	"IGameEventSystem::ProcessQueuedEvents";
constexpr const char* kPurgeQueuedEvents =
	"IGameEventSystem::PurgeQueuedEvents";

/** Nine of IGameEventSystem's ten methods; only GetEventSource is left alone. */
constexpr int kHookedMethodCount = 9;

// ---- the IGameEventManager2 hunt --------------------------------------------

/**
 * Every version string this interface has ever answered to, asked of every
 * loaded module.
 *
 * s2sdk's cs2 branch supplies no constant for it, so there is nothing to
 * #include here - these are written out, with where each one comes from, so a
 * reader can tell a researched guess from a hopeful one. The list is cheap to
 * extend and costs one CreateInterface call per module per entry.
 */
struct ManagerVersionProbe
{
	const char* version;
	const char* provenance;
};

const ManagerVersionProbe kManagerVersions[] = {
	{"GAMEEVENTSMANAGER002", "INTERFACEVERSION_GAMEEVENTSMANAGER2 on the Source 1 SDK"},
	{"GAMEEVENTSMANAGER001", "the revision before it"},
	{"GameEventsManager002",  "the same name in Source 2 casing"},
	{"GameEventManagerServerV001",
	                          "the shape CS2 uses for its sibling, GameEventSystemServerV001"},
};

/**
 * One row per IGameEventManager2 method.
 *
 * `reason` is why the method is not called even when an instance is in hand;
 * null means it is called explicitly. Both branches - instance or not - walk
 * this table, so no method can quietly fall out of the log.
 */
struct MethodAccount
{
	const char* method;
	const char* reason;
};

const MethodAccount kManagerMethods[] = {
	{"IGameEventManager2::~IGameEventManager2",
	 "would destroy the engine's event manager"},
	{"IGameEventManager2::LoadEventsFromFile",
	 "parses a .res file straight into the live event registry; the return value "
	 "is the only readable part and it costs the write"},
	{"IGameEventManager2::Reset",
	 "drops every event descriptor and listener on a running server"},
	{"IGameEventManager2::AddListener",
	 "would register a listener into the engine's own list"},
	{"IGameEventManager2::FindListener",
	 "takes the listener to look for; this suite never registers one, and a "
	 "made-up pointer would only prove the engine did not find it"},
	{"IGameEventManager2::RemoveListener",
	 "the inverse of AddListener; nothing registered to remove"},
	{"IGameEventManager2::CreateEvent",
	 "allocates an engine-owned IGameEvent that FreeEvent has to hand back; a "
	 "diagnostic sweep is the wrong place to take on that pairing"},
	{"IGameEventManager2::FireEvent",
	 "fires a real event at every connected client"},
	{"IGameEventManager2::FireEventClientSide",
	 "client-side path; a dedicated server has no client to fire it at"},
	{"IGameEventManager2::DuplicateEvent",
	 "needs an IGameEvent, and produces one more that must be freed"},
	{"IGameEventManager2::FreeEvent",
	 "the other half of CreateEvent/DuplicateEvent; nothing allocated to free"},
	{"IGameEventManager2::SerializeEvent",
	 "needs a CNetMessagePB<CMsgSource1LegacyGameEvent> to write into, and the "
	 "netmessages protobufs are not among CS2SDKTEST_PROTO_FILES"},
	{"IGameEventManager2::UnserializeEvent",
	 "same protobuf, plus it allocates an event that must be freed"},
	{"IGameEventManager2::LookupEventId", nullptr},
	{"IGameEventManager2::PrintEventToString",
	 "needs an IGameEvent to print"},
	{"IGameEventManager2::HasEventDescriptor", nullptr},
};

/**
 * IGameEvent's surface, listed rather than summarised.
 *
 * None of it is reachable: the only way to get an IGameEvent is
 * IGameEventManager2::CreateEvent, and this build has no manager. Listing the
 * methods one per row is the difference between "checked, nothing to do" and
 * "nobody ever looked" - and this is the set a CS2 update would break for every
 * plugin that reads event fields, so it is worth keeping visible.
 *
 * Overloads carry their distinguishing parameter, because they are separate
 * vtable slots and can drift apart.
 */
const char* const kGameEventMethods[] = {
	"IGameEvent::~IGameEvent",
	"IGameEvent::GetName",
	"IGameEvent::GetID",
	"IGameEvent::IsReliable",
	"IGameEvent::IsLocal",
	"IGameEvent::IsEmpty",
	"IGameEvent::GetBool",
	"IGameEvent::GetInt",
	"IGameEvent::GetUint64",
	"IGameEvent::GetFloat",
	"IGameEvent::GetString",
	"IGameEvent::GetPtr",
	"IGameEvent::GetEHandle",
	"IGameEvent::GetEntity",
	"IGameEvent::GetEntityIndex",
	"IGameEvent::GetPlayerSlot",
	"IGameEvent::GetPlayerController",
	"IGameEvent::GetPlayerPawn",
	"IGameEvent::GetPawnEHandle",
	"IGameEvent::GetPawnEntityIndex",
	"IGameEvent::SetBool",
	"IGameEvent::SetInt",
	"IGameEvent::SetUint64",
	"IGameEvent::SetFloat",
	"IGameEvent::SetString",
	"IGameEvent::SetPtr",
	"IGameEvent::SetEntity [CEntityInstance*]",
	"IGameEvent::SetEntity [CEntityIndex]",
	"IGameEvent::SetPlayer [CEntityInstance*]",
	"IGameEvent::SetPlayer [CPlayerSlot]",
	"IGameEvent::SetPlayerRaw",
	"IGameEvent::HasKey",
	"IGameEvent::unk001",
	"IGameEvent::GetDataKeys",
};

/** IGameEventListener2 is two slots and needs the manager to be registered. */
const char* const kGameEventListenerMethods[] = {
	"IGameEventListener2::~IGameEventListener2",
	"IGameEventListener2::FireGameEvent",
};

// ---- the messages a game event travels inside -------------------------------

struct EventMessageProbe
{
	const char* name;
	const char* what;

	/** The one whose descriptor gets inspected and whose methods get called. */
	bool primary;
};

const EventMessageProbe kEventMessages[] = {
	{"CMsgSource1LegacyGameEvent",
	 "the wrapper every IGameEventManager2::FireEvent ends up inside", true},
	{"CMsgSource1LegacyGameEventList",
	 "the event descriptor table the server ships to clients", false},
	{"CMsgSource1LegacyListenEvents",
	 "the per-client subscription bitfield", false},
};

/**
 * Prefix shared by the three above, so the partial lookup gets a string that is
 * a genuine prefix rather than a substring - "partial" is not defined anywhere
 * in the SDK, and a substring match is the more optimistic of the two readings.
 */
constexpr const char* kEventMessagePartial = "CMsgSource1Legacy";

/** A message name nothing will ever register, so null is the only right answer. */
constexpr const char* kNoSuchMessage = "CMsgCs2SdkTestNoSuchMessage";

/** Event every CS2 build has had; used only as a lookup key. */
constexpr const char* kProbeEventName = "player_death";

// ---- small helpers ----------------------------------------------------------

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

/**
 * NetChannelBufType_t is an int8 enum with exactly four values, which makes it
 * the cheapest signature check in this suite: it is the last argument of
 * PostEventAbstract, so it is the first thing to turn into nonsense if an
 * argument is inserted ahead of it.
 */
const char* BufTypeName(NetChannelBufType_t type)
{
	switch (type)
	{
		case BUF_DEFAULT:    return "DEFAULT";
		case BUF_UNRELIABLE: return "UNRELIABLE";
		case BUF_RELIABLE:   return "RELIABLE";
		case BUF_VOICE:      return "VOICE";
		default:             return "NOT-A-BUF-TYPE";
	}
}

/**
 * Renders the recipient arguments of PostEventAbstract without dereferencing
 * anything that has not been vetted.
 *
 * The SDK documents nClientCount == -1 with a null pointer as "everyone", and
 * otherwise a real count against a uint64 bitmask. Anything else is a shifted
 * argument list, not an unusual broadcast.
 */
void DescribeRecipients(int client_count, const uint64* clients, char* out, std::size_t size)
{
	if (client_count == -1 && clients == nullptr)
	{
		std::snprintf(out, size, "to=all");
		return;
	}
	if (client_count < -1 || client_count > ABSOLUTE_PLAYER_LIMIT)
	{
		std::snprintf(out, size, "to=%d IMPLAUSIBLE-COUNT", client_count);
		return;
	}
	if (!LooksLikeObjectPointer(clients))
	{
		std::snprintf(out, size, "to=%d mask=<bad ptr %p>", client_count,
			static_cast<const void*>(clients));
		return;
	}
	std::snprintf(out, size, "to=%d mask=0x%016llx", client_count,
		static_cast<unsigned long long>(clients[0]));
}

/**
 * Renders the payload pointer, plus the one field of CNetMessage that can be
 * read without walking into the protobuf behind it.
 *
 * GetMargin() is an inline load of a float member, not a virtual call - the
 * same class of read as CCommand::ArgC() in the clients suite. The value itself
 * is not judged: CNetMessage's constructor is deleted and the SDK's own comment
 * calls the declaration a stub, so a message the engine never set a margin on
 * holds whatever was there.
 */
void DescribeNetMessage(const CNetMessage* data, char* out, std::size_t size)
{
	if (data == nullptr)
	{
		std::snprintf(out, size, "data=(null)");
		return;
	}
	if (!LooksLikeObjectPointer(data))
	{
		std::snprintf(out, size, "data=<bad ptr %p>", static_cast<const void*>(data));
		return;
	}
	std::snprintf(out, size, "data=%p margin=%.3f", static_cast<const void*>(data),
		static_cast<double>(data->GetMargin()));
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool GameEventSuite::Acquire(TestContext& context)
{
	event_system_ = static_cast<IGameEventSystem*>(
		context.QueryInterface("engine2", GAMEEVENTSYSTEM_INTERFACE_VERSION));

	if (event_system_ == nullptr)
	{
		// Worth distinguishing "engine2 is not loaded" from "engine2 is loaded
		// but no longer answers this version": the second one is the finding.
		const Module& engine_module = context.GetModule("engine2");
		if (!engine_module.IsValid())
		{
			context.UnavailableCall("IGameEventSystem", "engine2 is not loaded");
		}
		else
		{
			context.UnavailableCall("IGameEventSystem",
				"engine2 is loaded but does not answer '%s'",
				GAMEEVENTSYSTEM_INTERFACE_VERSION);
		}
	}
	else
	{
		context.Note("IGameEventSystem at %p (%s @ engine2)",
			static_cast<void*>(event_system_), GAMEEVENTSYSTEM_INTERFACE_VERSION);
	}

	// Silent here; ReportManagerProbe prints a row per candidate under the fault
	// guard. This pass exists so the hook-install path - which has no guard, and
	// so does nothing but query - still knows whether a manager turned up.
	manager_ = nullptr;
	manager_version_ = nullptr;
	manager_module_.clear();

	for (const ManagerVersionProbe& probe : kManagerVersions)
	{
		std::string found_in;
		void* iface = context.QueryInterfaceAnywhere(probe.version, &found_in);
		if (iface != nullptr)
		{
			manager_ = static_cast<IGameEventManager2*>(iface);
			manager_version_ = probe.version;
			manager_module_ = found_in;
			break;
		}
	}

	// Either half is enough to be worth running. A suite that disappears from
	// the log reads exactly like a suite nobody ever wrote, and "IGameEventSystem
	// stopped answering its version string" is precisely the kind of update
	// damage this plugin exists to name.
	return event_system_ != nullptr || manager_ != nullptr;
}

void GameEventSuite::ExerciseCalls(TestContext& context)
{
	legacy_event_message_ = nullptr;

	if (event_system_ != nullptr)
	{
		ExerciseAppSystem(context);
		ExerciseEventSystem(context);
	}
	else
	{
		context.Note("[IGameEventSystem] not acquired; its calls and hooks are skipped");
	}

	ResolveEventMessages(context);
	InspectNetMessageInfo(context);
	ReportObservedMessages(context);

	ReportManagerProbe(context);
	ExerciseGameEventManager(context);
	AccountForGameEvent(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void GameEventSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = event_system_->GetTier();
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
		const bool singleton = event_system_->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the event system is a singleton; false here suggests a slot shift");
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = event_system_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = event_system_->GetDependencies();
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
		void* self = event_system_->QueryInterface(GAMEEVENTSYSTEM_INTERFACE_VERSION);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(event_system_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run engine startup");
	context.SkipCall("IAppSystem::Init", "would re-run engine startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin engine shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the engine down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down engine interfaces");
}

// -----------------------------------------------------------------------------
//  IGameEventSystem
//
//  Ten methods, of which nine either send an event or edit the engine's
//  registry. The one that only asks is called; the other nine are listed with
//  what they would do, and hooked instead - which is where this suite's
//  evidence actually comes from.
// -----------------------------------------------------------------------------

void GameEventSuite::ExerciseEventSystem(TestContext& context)
{
	context.Note("[IGameEventSystem] one read-only method out of ten; the rest are hooked");

	CS2ST_CALL(context, "IGameEventSystem::GetEventSource", {
		const CEntityIndex source = event_system_->GetEventSource();
		const int index = source.Get();

		// Reported, not judged. CEntityIndex wraps a plain int and the SDK
		// promises it no range, so the only honest thing is to print it and say
		// what shape a sane answer has: -1 outside a dispatch, otherwise an
		// entity index below MAX_EDICTS. Turning that into a Fail would be
		// encoding a semantic guess, which is how this project gets false
		// alarms it then learns to ignore.
		const bool shaped_like_an_index = (index == -1) || (index >= 0 && index < MAX_EDICTS);
		context.Detail("-> %d%s", index,
			shaped_like_an_index ? "" : "  (outside [-1, MAX_EDICTS); not an entity index)");
	});

	context.SkipCall("IGameEventSystem::RegisterGameEvent",
		"adds a message to the engine's event registry; hooked instead");
	context.SkipCall("IGameEventSystem::RegisterGameEventHandlerAbstract",
		"binds a delegate into engine-owned state, and CUtlSlot has no class "
		"body in the SDK to build one against; hooked instead");
	context.SkipCall("IGameEventSystem::UnregisterGameEventHandlerAbstract",
		"the inverse of the above; nothing of ours registered to remove");
	context.SkipCall("IGameEventSystem::PostEventAbstract_Local",
		"injects an event nobody raised; hooked instead");
	context.SkipCall("IGameEventSystem::PostEventAbstract [clients]",
		"broadcasts an event to connected clients; hooked instead");
	context.SkipCall("IGameEventSystem::PostEventAbstract [filter]",
		"same, through a recipient filter; hooked instead");
	context.SkipCall("IGameEventSystem::PostEntityEventAbstract",
		"despite the name it posts to every client; hooked instead");
	context.SkipCall("IGameEventSystem::ProcessQueuedEvents",
		"drains the engine's event queue outside its own frame; hooked instead");
	context.SkipCall("IGameEventSystem::PurgeQueuedEvents",
		"throws away events the engine has not delivered yet; hooked instead");
}

// -----------------------------------------------------------------------------
//  The message registry, reached through INetworkMessages
//
//  Everything IGameEventSystem takes is an INetworkMessageInternal*, and the
//  only read-only way to obtain one by name is INetworkMessages. It is used
//  here the way the clients suite uses IVEngineServer2 for maxClients: as a
//  tool. Its own coverage is docs/TEST_TARGETS.md #8 and is not attempted.
// -----------------------------------------------------------------------------

void GameEventSuite::ResolveEventMessages(TestContext& context)
{
	context.Note("[INetworkMessages] used as a tool, not a subject - it is what turns "
	             "a message pointer into a name, and a name into a descriptor");

	INetworkMessages* messages = static_cast<INetworkMessages*>(
		context.QueryInterface("networksystem", NETWORKMESSAGES_INTERFACE_VERSION));

	if (messages == nullptr)
	{
		context.UnavailableCall("INetworkMessages::FindNetworkMessage",
			"networksystem does not answer '%s'; the descriptor pass below has "
			"nothing to start from", NETWORKMESSAGES_INTERFACE_VERSION);
		return;
	}

	for (const EventMessageProbe& probe : kEventMessages)
	{
		const std::string target =
			std::string("INetworkMessages::FindNetworkMessage(\"") + probe.name + "\")";

		CS2ST_CALL(context, target, {
			INetworkMessageInternal* message = messages->FindNetworkMessage(probe.name);
			context.Detail("-> %p  (%s)", static_cast<void*>(message), probe.what);
			if (probe.primary)
			{
				legacy_event_message_ = message;
			}
		});
	}

	CS2ST_CALL(context, "INetworkMessages::FindNetworkMessage (nonexistent name)", {
		// A matcher that only ever says yes is as broken as one that only ever
		// says no, and this is the half that costs nothing to check.
		INetworkMessageInternal* message = messages->FindNetworkMessage(kNoSuchMessage);
		context.Detail("-> %p for \"%s\"", static_cast<void*>(message), kNoSuchMessage);
		if (message != nullptr)
		{
			context.Fail("a message name nothing registers came back non-null");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::FindNetworkMessagePartial", {
		// The second entry point to the same registry. It is allowed to return
		// any of the three Source1Legacy messages, so the check is that it
		// returns one of them at all - not which.
		INetworkMessageInternal* message = messages->FindNetworkMessagePartial(kEventMessagePartial);
		context.Detail("-> %p for \"%s\"", static_cast<void*>(message), kEventMessagePartial);
		if (message == nullptr && legacy_event_message_ != nullptr)
		{
			// Suspect, not Fail: what "partial" means is nowhere in the SDK, so
			// the two entry points disagreeing might be a prefix-vs-substring
			// difference rather than a broken registry. Still printed, because
			// this is the call plugins use to find messages by fragment.
			context.Suspect("the exact-name lookup found a Source1Legacy message "
			                "but the partial lookup found none");
		}
	});

	if (legacy_event_message_ == nullptr)
	{
		context.UnavailableCall("INetworkMessageInternal",
			"no Source1Legacy game event message is registered, so there is no "
			"engine-built instance to call anything on");
		return;
	}

	context.Note("[INetworkMessageInternal] on the CMsgSource1LegacyGameEvent registry entry");

	CS2ST_CALL(context, "INetworkMessageInternal::GetUnscopedName", {
		const char* name = legacy_event_message_->GetUnscopedName();
		context.Detail("-> %s", name != nullptr ? name : "(null)");
		if (name == nullptr)
		{
			context.Fail("a registered message with no name cannot be looked up");
		}
	});

	CS2ST_CALL(context, "INetworkMessageInternal::GetNetMessageInfo", {
		// The oracle: the same descriptor asked for down two vtables that share
		// no code - the message's own, and the registry's. Disagreement means
		// one of the two slots is not the method its name says.
		NetMessageInfo_t* direct = legacy_event_message_->GetNetMessageInfo();
		NetMessageInfo_t* via_registry = messages->GetNetMessageInfo(legacy_event_message_);
		context.Detail("-> %p, INetworkMessages says %p", static_cast<void*>(direct),
			static_cast<void*>(via_registry));
		if (direct == nullptr)
		{
			context.Fail("a registered message with no descriptor cannot be serialised");
		}
		else if (via_registry != nullptr && via_registry != direct)
		{
			context.Fail("two routes to one descriptor returned different addresses");
		}
	});

	context.SkipCall("INetworkMessageInternal::SetMessageId",
		"renumbers a message the engine has already handed to clients");
	context.SkipCall("INetworkMessageInternal::AddCategoryMask",
		"changes how the engine routes every message of this type");
	context.SkipCall("INetworkMessageInternal::SwitchMode",
		"flips the registry between server and client serialisation");
	context.SkipCall("INetworkMessageInternal::AllocateMessage",
		"allocates an engine-owned CNetMessage that only "
		"INetworkMessages::DeallocateNetMessageAbstract can return");
	context.SkipCall("INetworkMessageInternal::Serialize",
		"needs a bf_write and a filled CNetMessage; tier1/bitbuf.cpp is not in "
		"this build (CS2SDKTEST_TIER1_BITBUF is off)");
	context.SkipCall("INetworkMessageInternal::Unserialize",
		"the same buffer, plus a message to write the result into");
	context.SkipCall("INetworkMessageInternal::~INetworkMessageInternal",
		"would destroy a registry entry the engine still owns");
}

// -----------------------------------------------------------------------------
//  NetMessageInfo_t - the descriptor behind a game event
//
//  This is the "event description structure" worth looking at: it is what the
//  engine consults to decide a game event's id, group, channel and reliability.
//  It is also a good shape for the inspector - one pointer, one string, one
//  bool - and it comes with three independent readings of its own contents,
//  because IProtobufBinding answers the same questions through a vtable.
// -----------------------------------------------------------------------------

void GameEventSuite::InspectNetMessageInfo(TestContext& context)
{
	if (legacy_event_message_ == nullptr)
	{
		context.SkipCall("NetMessageInfo_t",
			"no CMsgSource1LegacyGameEvent registry entry to read it from");
		return;
	}

	NetMessageInfo_t* info = nullptr;

	CS2ST_CALL(context, "NetMessageInfo_t (fetch)", {
		info = legacy_event_message_->GetNetMessageInfo();
		context.Detail("-> %p, %zu bytes as the SDK declares it",
			static_cast<void*>(info), sizeof(NetMessageInfo_t));
	});

	if (info == nullptr)
	{
		context.SkipCall("NetMessageInfo_t", "GetNetMessageInfo did not return an instance");
		return;
	}

	// One inspector, one declaring struct. NetMessageInfo_t has no base, so
	// there is only this pass. Offsets come from offsetof() on purpose - the
	// point is to compare the layout the SDK *assumes* against the memory the
	// engine handed over, so the assumption has to be computed from the header.
	StructInspector inspect(context, "NetMessageInfo_t", info, sizeof(NetMessageInfo_t));

	inspect.Int32  ("m_nCategories",       offsetof(NetMessageInfo_t, m_nCategories));
	inspect.Pointer("m_pBinding",          offsetof(NetMessageInfo_t, m_pBinding));
	// CUtlString's whole representation is one char*, so the string check
	// applies to the member directly: plausible pointer, printable, terminated.
	inspect.String ("m_szGroup",           offsetof(NetMessageInfo_t, m_szGroup));
	// uint16 / uint8 fields: the inspector has no judgement to offer on an
	// integer, so they are reported as raw bytes at their declared widths.
	inspect.Raw    ("m_MessageId",         offsetof(NetMessageInfo_t, m_MessageId),
	                                       sizeof(NetworkMessageId));
	inspect.Raw    ("m_GroupId",           offsetof(NetMessageInfo_t, m_GroupId),
	                                       sizeof(NetworkGroupId));
	// The 2026-10 SDK replaced this byte's m_nFlags/m_unk001 with a named
	// bitfield triple - m_nBufType:6, m_bCannotBeNetworked:1, m_unk001:1.
	// offsetof() cannot name a bitfield (C2618), so the byte is addressed
	// relative to m_GroupId, the last ordinary member before it.
	inspect.Raw    ("m_nBufType/m_bCannotBeNetworked/m_unk001 (packed byte)",
	                                       offsetof(NetMessageInfo_t, m_GroupId) + 1,
	                                       sizeof(uint8));
	inspect.Int32  ("m_nRequiredHandlerCategories",
	                                       offsetof(NetMessageInfo_t, m_nRequiredHandlerCategories));
	// The sharpest probe in the struct: one byte with two legal values, sitting
	// past every field above it.
	inspect.Bool   ("m_bOkayToRedispatch", offsetof(NetMessageInfo_t, m_bOkayToRedispatch));

	inspect.Summarize();
	inspect.HexDump();

	// ---- IProtobufBinding, and the three oracles it gives ------------------

	IProtobufBinding* binding = info->m_pBinding;

	if (!LooksLikeObjectPointer(binding))
	{
		context.UnavailableCall("IProtobufBinding",
			"m_pBinding is %p, which is not an object pointer",
			static_cast<void*>(binding));
		return;
	}

	context.Note("[IProtobufBinding] the same descriptor read down a vtable instead of "
	             "by offset - where the two disagree, one of them is wrong");

	CS2ST_CALL(context, "IProtobufBinding::GetName", {
		const char* binding_name = binding->GetName();
		const char* message_name = legacy_event_message_->GetUnscopedName();
		context.Detail("-> \"%s\" vs INetworkMessageInternal::GetUnscopedName \"%s\"",
			binding_name != nullptr ? binding_name : "(null)",
			message_name != nullptr ? message_name : "(null)");
		if (binding_name == nullptr)
		{
			context.Fail("the binding a registered message points at has no name");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::GetSize", {
		const int size = binding->GetSize();
		context.Detail("-> %d", size);
		// A negative size is impossible whatever the number means; zero is not,
		// because the SDK never says whether this is the C++ object or the
		// serialised payload, and an empty message has an empty payload.
		if (size < 0)
		{
			context.Fail("a size cannot be negative");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::GetGroup", {
		// The group name down the binding's vtable against the m_szGroup the
		// descriptor stores by value: two descriptions of one fact that share
		// no code. Recorded as Suspect rather than Fail on a mismatch - the SDK
		// never says the descriptor's copy comes from the binding, and inventing
		// that contract is how this project has produced false alarms before.
		const char* group = binding->GetGroup();
		const char* stored = info->m_szGroup.Get();
		context.Detail("-> \"%s\" vs NetMessageInfo_t::m_szGroup \"%s\"",
			group != nullptr ? group : "(null)", stored != nullptr ? stored : "(null)");
		if (group != nullptr && stored != nullptr && std::strcmp(group, stored) != 0)
		{
			context.Suspect("the binding and the descriptor disagree on the group");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::GetGroupColor", {
		const Color color = binding->GetGroupColor();
		context.Detail("-> r=%d g=%d b=%d a=%d", color.r(), color.g(), color.b(), color.a());
	});

	CS2ST_CALL(context, "IProtobufBinding::GetBufType", {
		const NetChannelBufType_t buf_type = binding->GetBufType();
		context.Detail("-> %s (%d)", BufTypeName(buf_type), static_cast<int>(buf_type));
		if (buf_type < BUF_DEFAULT || buf_type > BUF_VOICE)
		{
			context.Fail("outside NetChannelBufType_t's four enumerators");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::OkToRedispatch", {
		// The sharpest of the three: a bool read two ways, and the descriptor's
		// byte was already checked for being 0 or 1 above, so this asks whether
		// it is the *same* 0 or 1 the binding reports.
		//
		// Still Suspect and not Fail. These are two objects, not a getter and
		// the field behind it: the names line up exactly and the engine almost
		// certainly copies one into the other at registration, but "almost
		// certainly" is not something the SDK states, and a FAIL that turns out
		// to be our inference teaches people to skip the column.
		const bool from_binding = binding->OkToRedispatch();
		const bool from_descriptor = info->m_bOkayToRedispatch;
		context.Detail("-> %s vs NetMessageInfo_t::m_bOkayToRedispatch %s",
			from_binding ? "true" : "false", from_descriptor ? "true" : "false");
		if (from_binding != from_descriptor)
		{
			context.Suspect("the binding and the descriptor disagree on redispatch");
		}
	});

	context.SkipCall("IProtobufBinding::ToString",
		"needs a filled message to render, and this suite never allocates one");
	context.SkipCall("IProtobufBinding::AllocateMessage",
		"allocates an engine-owned CNetMessage with no matching free on this interface");
	context.SkipCall("IProtobufBinding::unk001", "unidentified in the SDK");
}

// -----------------------------------------------------------------------------
//  What the hooks have seen
//
//  The hooks cannot resolve a name: they run in the engine's own frame, where
//  the fault guard is not armed and walking an engine object is how a
//  diagnostic becomes an outage. So they remember the distinct pointers, and
//  the naming happens here, under the guard, on the next sweep.
// -----------------------------------------------------------------------------

void GameEventSuite::ReportObservedMessages(TestContext& context)
{
	context.Note("[hooks] %zu distinct INetworkMessageInternal pointer(s) seen so far%s",
		observed_message_count_,
		observed_message_overflow_ > 0 ? " (further ones dropped; the cap is deliberate)" : "");

	if (observed_message_count_ == 0)
	{
		context.Note("nothing yet - 'cst hooks install event', let a round play, "
		             "then sweep again; that is what turns hook counts into event names");
		return;
	}

	for (std::size_t i = 0; i < observed_message_count_; ++i)
	{
		INetworkMessageInternal* message = observed_messages_[i];

		char index_text[24];
		std::snprintf(index_text, sizeof(index_text), " (observed #%zu)", i);
		const std::string target =
			std::string("INetworkMessageInternal::GetUnscopedName") + index_text;

		CS2ST_CALL(context, target, {
			// Re-vetted even though the hook vetted it: these have sat in a
			// member across an unknown amount of engine activity.
			if (!LooksLikeObjectPointer(message))
			{
				context.Detail("-> %p is not an object pointer", static_cast<void*>(message));
				context.Fail("a hook recorded something that is not a message");
				return;
			}
			const char* name = message->GetUnscopedName();
			context.Detail("%p -> %s", static_cast<void*>(message),
				name != nullptr ? name : "(null)");
		});
	}
}

// -----------------------------------------------------------------------------
//  IGameEventManager2
// -----------------------------------------------------------------------------

void GameEventSuite::ReportManagerProbe(TestContext& context)
{
	context.Note("[IGameEventManager2] s2sdk's cs2 branch declares the class and no "
	             "version constant, so every name it has ever answered to is asked of "
	             "every loaded module");

	manager_ = nullptr;
	manager_version_ = nullptr;
	manager_module_.clear();

	for (const ManagerVersionProbe& probe : kManagerVersions)
	{
		const std::string target =
			std::string("CreateInterface(\"") + probe.version + "\")";

		CS2ST_CALL(context, target, {
			std::string found_in;
			void* iface = context.QueryInterfaceAnywhere(probe.version, &found_in);
			context.Detail("-> %p%s%s  [%s]", iface,
				iface != nullptr ? " from " : "",
				iface != nullptr ? found_in.c_str() : "no module answers",
				probe.provenance);
			if (iface != nullptr && manager_ == nullptr)
			{
				manager_ = static_cast<IGameEventManager2*>(iface);
				manager_version_ = probe.version;
				manager_module_ = found_in;
			}
		});
	}

	if (manager_ == nullptr)
	{
		context.Note("none answered. CS2 plugins reach this object by calling a virtual "
		             "on ISource2Server at an offset their gamedata supplies and "
		             "subtracting 8 - a moving number this project has no source for, "
		             "and blind-calling server vtable slots to find it would hit things "
		             "like PreFatalShutdown. So the manager stays out of reach on "
		             "purpose, and the row above is the durable record of that.");
	}
}

void GameEventSuite::ExerciseGameEventManager(TestContext& context)
{
	if (manager_ == nullptr)
	{
		// One row per method rather than one row for the interface. A reader has
		// to be able to tell "checked, nothing to do" from "nobody ever looked",
		// and an absent row cannot say which.
		for (const MethodAccount& entry : kManagerMethods)
		{
			context.UnavailableCall(entry.method,
				"no IGameEventManager2 instance on this build");
		}
		return;
	}

	context.Note("an IGameEventManager2 answered '%s' from %s - that is new, and worth "
	             "writing down before anything else in this log",
		manager_version_ != nullptr ? manager_version_ : "?", manager_module_.c_str());

	int event_id = -1;

	CS2ST_CALL(context, "IGameEventManager2::LookupEventId", {
		event_id = manager_->LookupEventId(kProbeEventName);
		context.Detail("-> %d for \"%s\"", event_id, kProbeEventName);
	});

	CS2ST_CALL(context, "IGameEventManager2::HasEventDescriptor", {
		// Oracle: the two answers are the same fact asked twice. A descriptor
		// that exists must have an id, and an id that resolves must have come
		// from a descriptor.
		const bool has = manager_->HasEventDescriptor(kProbeEventName);
		context.Detail("-> %s for \"%s\" (LookupEventId said %d)",
			has ? "true" : "false", kProbeEventName, event_id);
		if (has != (event_id >= 0))
		{
			// Suspect: the SDK does not say whether LookupEventId returns -1 for
			// "no descriptor" or for "descriptor with no id assigned yet".
			context.Suspect("one route says the descriptor exists and the other does not");
		}
	});

	for (const MethodAccount& entry : kManagerMethods)
	{
		if (entry.reason != nullptr)
		{
			context.SkipCall(entry.method, "%s", entry.reason);
		}
	}
}

void GameEventSuite::AccountForGameEvent(TestContext& context)
{
	context.Note("[IGameEvent / IGameEventListener2] listed one row per method: this is "
	             "the surface every plugin that reads an event field depends on, and a "
	             "silent gap here would be indistinguishable from coverage");

	const char* const reason = (manager_ == nullptr)
		? "IGameEventManager2::CreateEvent is the only source of an IGameEvent, and "
		  "there is no manager on this build"
		: "CreateEvent is not exercised on a live server, so there is no event instance";

	for (const char* method : kGameEventMethods)
	{
		context.UnavailableCall(method, "%s", reason);
	}

	for (const char* method : kGameEventListenerMethods)
	{
		context.UnavailableCall(method,
			"the engine only calls this through IGameEventManager2::AddListener, "
			"which needs the manager");
	}
}

// -----------------------------------------------------------------------------
//  Install / remove
// -----------------------------------------------------------------------------

int GameEventSuite::InstallHooks(TestContext& context)
{
	if (event_system_ == nullptr || hooks_installed_)
	{
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before installing, so a hook that never fires still shows up in
	// the dump as `never`. After a CS2 update that row is the finding, and a
	// silently missing row would read like a hook nobody wrote.
	recorder.Declare(kRegisterGameEvent, Id());
	recorder.Declare(kRegisterGameEventHandlerAbstract, Id());
	recorder.Declare(kUnregisterGameEventHandlerAbstract, Id());
	recorder.Declare(kPostEventAbstractLocal, Id());
	recorder.Declare(kPostEventAbstract, Id());
	recorder.Declare(kPostEventAbstractFiltered, Id());
	recorder.Declare(kPostEntityEventAbstract, Id());
	recorder.Declare(kProcessQueuedEvents, Id());
	recorder.Declare(kPurgeQueuedEvents, Id());

	// Each hook object carries its pre and post handler from the constructor;
	// Add installs the detour on the event system, Remove takes it off.
	hook_RegisterGameEvent_.Add(event_system_);
	hook_RegisterGameEventHandlerAbstract_.Add(event_system_);
	hook_UnregisterGameEventHandlerAbstract_.Add(event_system_);
	hook_PostEventAbstract_Local_.Add(event_system_);
	hook_PostEventAbstract_.Add(event_system_);
	hook_PostEventAbstractFiltered_.Add(event_system_);
	hook_PostEntityEventAbstract_.Add(event_system_);
	hook_ProcessQueuedEvents_.Add(event_system_);
	hook_PurgeQueuedEvents_.Add(event_system_);

	hooks_installed_ = true;

	context.Note("ProcessQueuedEvents runs every frame, so a count of zero on it means "
	             "the hooks did not attach at all - check that row first");
	CS2ST_LOG_INFO("%s: sweep again after a round to resolve the message pointers these "
	               "hooks collect into names\n", Id());
	return kHookedMethodCount;
}

void GameEventSuite::RemoveHooks(TestContext& context)
{
	if (event_system_ == nullptr || !hooks_installed_)
	{
		return;
	}

	hook_RegisterGameEvent_.Remove(event_system_);
	hook_RegisterGameEventHandlerAbstract_.Remove(event_system_);
	hook_UnregisterGameEventHandlerAbstract_.Remove(event_system_);
	hook_PostEventAbstract_Local_.Remove(event_system_);
	hook_PostEventAbstract_.Remove(event_system_);
	hook_PostEventAbstractFiltered_.Remove(event_system_);
	hook_PostEntityEventAbstract_.Remove(event_system_);
	hook_ProcessQueuedEvents_.Remove(event_system_);
	hook_PurgeQueuedEvents_.Remove(event_system_);

	hooks_installed_ = false;
	(void)context;
}

// -----------------------------------------------------------------------------
//  Hook handlers
//
//  These run in the engine's own frame, where CallGuard is not armed: a fault
//  here kills the server rather than producing a CRASHED line. So every pointer
//  is vetted before it is read, nothing virtual is called on an argument, and
//  no structure is walked. Naming a message is deliberately deferred to the
//  next call sweep, which does have a guard around it.
//
//  Every one answers Action::Ignore, the KHook equivalent of MRES_IGNORED.
// -----------------------------------------------------------------------------

void GameEventSuite::NoteObservedMessage(INetworkMessageInternal* message)
{
	if (!LooksLikeObjectPointer(message))
	{
		return;
	}

	for (std::size_t i = 0; i < observed_message_count_; ++i)
	{
		if (observed_messages_[i] == message)
		{
			return;
		}
	}

	if (observed_message_count_ >= kMaxObservedMessages)
	{
		++observed_message_overflow_;
		return;
	}

	observed_messages_[observed_message_count_++] = message;
}

KHook::Return<void> GameEventSuite::Pre_RegisterGameEvent(IGameEventSystem*, INetworkMessageInternal* pEvent)
{
	// Fires while the engine builds its event registry, which on a server this
	// plugin loads late into has usually already happened. A count of zero here
	// is normal; a count above zero means an event type was registered after
	// load, which is worth knowing on its own.
	NoteObservedMessage(pEvent);
	CS2ST_HOOK_ENTER_ARGS(kRegisterGameEvent, "event=%p", static_cast<void*>(pEvent));
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_RegisterGameEvent(IGameEventSystem*, INetworkMessageInternal*)
{
	CS2ST_HOOK_LEAVE(kRegisterGameEvent);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_RegisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot* nSlot,
                                                                         const CUtlAbstractDelegate& delegate,
                                                                         INetworkMessageInternal* pEvent,
                                                                         int nPriority)
{
	// The delegate is passed by reference and its members are private, so only
	// its address is recorded. CUtlSlot has no class body in the SDK at all,
	// which is why nothing is read through that pointer either.
	NoteObservedMessage(pEvent);
	CS2ST_HOOK_ENTER_ARGS(kRegisterGameEventHandlerAbstract,
		"slot=%p deleg=%p event=%p prio=%d",
		static_cast<void*>(nSlot), static_cast<const void*>(&delegate),
		static_cast<void*>(pEvent), nPriority);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_RegisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot*,
                                                                          const CUtlAbstractDelegate&,
                                                                          INetworkMessageInternal*, int)
{
	CS2ST_HOOK_LEAVE(kRegisterGameEventHandlerAbstract);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_UnregisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot* nSlot,
                                                                           const CUtlAbstractDelegate& delegate,
                                                                           INetworkMessageInternal* pEvent)
{
	NoteObservedMessage(pEvent);
	CS2ST_HOOK_ENTER_ARGS(kUnregisterGameEventHandlerAbstract,
		"slot=%p deleg=%p event=%p",
		static_cast<void*>(nSlot), static_cast<const void*>(&delegate),
		static_cast<void*>(pEvent));
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_UnregisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot*,
                                                                            const CUtlAbstractDelegate&,
                                                                            INetworkMessageInternal*)
{
	CS2ST_HOOK_LEAVE(kUnregisterGameEventHandlerAbstract);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_PostEventAbstract_Local(IGameEventSystem*, CSplitScreenSlot nSlot,
                                                                INetworkMessageInternal* pEvent,
                                                                const CNetMessage* pData, unsigned long nSize)
{
	NoteObservedMessage(pEvent);

	char payload[80];
	DescribeNetMessage(pData, payload, sizeof(payload));

	CS2ST_HOOK_ENTER_ARGS(kPostEventAbstractLocal, "slot=%d event=%p %s size=%lu",
		nSlot.Get(), static_cast<void*>(pEvent), payload, nSize);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_PostEventAbstract_Local(IGameEventSystem*, CSplitScreenSlot, INetworkMessageInternal*,
                                                                 const CNetMessage*, unsigned long)
{
	CS2ST_HOOK_LEAVE(kPostEventAbstractLocal);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_PostEventAbstract(IGameEventSystem*, CSplitScreenSlot nSlot, bool bLocalOnly,
                                                          int nClientCount, const uint64* clients,
                                                          INetworkMessageInternal* pEvent,
                                                          const CNetMessage* pData, unsigned long nSize,
                                                          NetChannelBufType_t bufType)
{
	// The busiest and most valuable hook in this suite: this is the slot every
	// user message and every networked game event goes through, and its
	// argument list has grown before. Two of those arguments have a small legal
	// set, so a shifted list shows up as a flagged sample instead of as a hook
	// that keeps counting happily.
	NoteObservedMessage(pEvent);

	char recipients[64];
	DescribeRecipients(nClientCount, clients, recipients, sizeof(recipients));

	char payload[80];
	DescribeNetMessage(pData, payload, sizeof(payload));

	CS2ST_HOOK_ENTER_ARGS(kPostEventAbstract,
		"slot=%d local=%d %s event=%p %s size=%lu buf=%s",
		nSlot.Get(), bLocalOnly ? 1 : 0, recipients,
		static_cast<void*>(pEvent), payload, nSize, BufTypeName(bufType));
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_PostEventAbstract(IGameEventSystem*, CSplitScreenSlot, bool, int, const uint64*,
                                                           INetworkMessageInternal*, const CNetMessage*,
                                                           unsigned long, NetChannelBufType_t)
{
	CS2ST_HOOK_LEAVE(kPostEventAbstract);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_PostEventAbstractFiltered(IGameEventSystem*, CSplitScreenSlot nSlot, bool bLocalOnly,
                                                                  IRecipientFilter* pFilter,
                                                                  INetworkMessageInternal* pEvent,
                                                                  const CNetMessage* pData, unsigned long nSize)
{
	NoteObservedMessage(pEvent);

	char payload[80];
	DescribeNetMessage(pData, payload, sizeof(payload));

	// Only the filter's address. GetRecipients() would be the interesting part,
	// but it is a virtual call on an object built by whoever sent the message,
	// and a hook is not the place to make one.
	CS2ST_HOOK_ENTER_ARGS(kPostEventAbstractFiltered,
		"slot=%d local=%d filter=%p%s event=%p %s size=%lu",
		nSlot.Get(), bLocalOnly ? 1 : 0, static_cast<void*>(pFilter),
		LooksLikeObjectPointer(pFilter) ? "" : " NOT-A-POINTER",
		static_cast<void*>(pEvent), payload, nSize);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_PostEventAbstractFiltered(IGameEventSystem*, CSplitScreenSlot, bool, IRecipientFilter*,
                                                                   INetworkMessageInternal*,
                                                                   const CNetMessage*, unsigned long)
{
	CS2ST_HOOK_LEAVE(kPostEventAbstractFiltered);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_PostEntityEventAbstract(IGameEventSystem*, const CBaseHandle& hndl,
                                                                INetworkMessageInternal* pEvent,
                                                                const CNetMessage* pData, unsigned long nSize,
                                                                NetChannelBufType_t bufType, bool bAllClients)
{
	NoteObservedMessage(pEvent);

	char payload[80];
	DescribeNetMessage(pData, payload, sizeof(payload));

	// CEntityHandle's accessors are inline bitfield reads, not calls into the
	// entity system, so they are safe here - and an entry index outside
	// [-1, MAX_EDICTS) says the handle argument is not a handle.
	const int entry = hndl.GetEntryIndex();
	const bool plausible = (entry >= -1) && (entry < MAX_EDICTS);

	CS2ST_HOOK_ENTER_ARGS(kPostEntityEventAbstract,
		"ent=%d%s serial=%d valid=%d event=%p %s size=%lu buf=%s all=%d",
		entry, plausible ? "" : " IMPLAUSIBLE-INDEX", hndl.GetSerialNumber(),
		hndl.IsValid() ? 1 : 0, static_cast<void*>(pEvent), payload, nSize,
		BufTypeName(bufType), bAllClients ? 1 : 0);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_PostEntityEventAbstract(IGameEventSystem*, const CBaseHandle&, INetworkMessageInternal*,
                                                                 const CNetMessage*, unsigned long,
                                                                 NetChannelBufType_t, bool)
{
	CS2ST_HOOK_LEAVE(kPostEntityEventAbstract);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_ProcessQueuedEvents(IGameEventSystem*)
{
	// Runs every frame, which makes it this suite's liveness probe: if its
	// count is zero, no hook here attached and every other row is meaningless.
	// HookRecorder stops formatting once its sample quota is full, which is
	// what keeps a per-frame hook affordable.
	CS2ST_HOOK_ENTER(kProcessQueuedEvents);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_ProcessQueuedEvents(IGameEventSystem*)
{
	CS2ST_HOOK_LEAVE(kProcessQueuedEvents);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Pre_PurgeQueuedEvents(IGameEventSystem*)
{
	CS2ST_HOOK_ENTER(kPurgeQueuedEvents);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GameEventSuite::Post_PurgeQueuedEvents(IGameEventSystem*)
{
	CS2ST_HOOK_LEAVE(kPurgeQueuedEvents);
	return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
//  Hook binding
//
//  The KHook::Virtual constructor resolves the vtable slot from the member
//  pointer. PostEventAbstract's two overloads share a name, so each member's
//  constructor names its slot with a static_cast to the exact overload - the
//  replacement for the overload index SH_DECL_HOOK used to carry.
// -----------------------------------------------------------------------------

// convar.h ends with `#pragma warning(default : 4355)`, which re-enables the warning
// for the rest of the translation unit and overrides the /wd4355 on the command
// line. Passing `this` as the hook context is intentional (stored, not used).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4355)
#endif
GameEventSuite::GameEventSuite() :
	hook_RegisterGameEvent_(&IGameEventSystem::RegisterGameEvent, this,
		&GameEventSuite::Pre_RegisterGameEvent,
		&GameEventSuite::Post_RegisterGameEvent),
	hook_RegisterGameEventHandlerAbstract_(&IGameEventSystem::RegisterGameEventHandlerAbstract, this,
		&GameEventSuite::Pre_RegisterGameEventHandlerAbstract,
		&GameEventSuite::Post_RegisterGameEventHandlerAbstract),
	hook_UnregisterGameEventHandlerAbstract_(&IGameEventSystem::UnregisterGameEventHandlerAbstract, this,
		&GameEventSuite::Pre_UnregisterGameEventHandlerAbstract,
		&GameEventSuite::Post_UnregisterGameEventHandlerAbstract),
	hook_PostEventAbstract_Local_(&IGameEventSystem::PostEventAbstract_Local, this,
		&GameEventSuite::Pre_PostEventAbstract_Local,
		&GameEventSuite::Post_PostEventAbstract_Local),
	hook_PostEventAbstract_(static_cast<void (IGameEventSystem::*)(
		CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*,
		const CNetMessage*, unsigned long, NetChannelBufType_t)>(
		&IGameEventSystem::PostEventAbstract), this,
		&GameEventSuite::Pre_PostEventAbstract,
		&GameEventSuite::Post_PostEventAbstract),
	hook_PostEventAbstractFiltered_(static_cast<void (IGameEventSystem::*)(
		CSplitScreenSlot, bool, IRecipientFilter*, INetworkMessageInternal*,
		const CNetMessage*, unsigned long)>(
		&IGameEventSystem::PostEventAbstract), this,
		&GameEventSuite::Pre_PostEventAbstractFiltered,
		&GameEventSuite::Post_PostEventAbstractFiltered),
	hook_PostEntityEventAbstract_(&IGameEventSystem::PostEntityEventAbstract, this,
		&GameEventSuite::Pre_PostEntityEventAbstract,
		&GameEventSuite::Post_PostEntityEventAbstract),
	hook_ProcessQueuedEvents_(&IGameEventSystem::ProcessQueuedEvents, this,
		&GameEventSuite::Pre_ProcessQueuedEvents,
		&GameEventSuite::Post_ProcessQueuedEvents),
	hook_PurgeQueuedEvents_(&IGameEventSystem::PurgeQueuedEvents, this,
		&GameEventSuite::Pre_PurgeQueuedEvents,
		&GameEventSuite::Post_PurgeQueuedEvents)
{
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace cs2st
