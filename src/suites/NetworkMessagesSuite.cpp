#include "suites/NetworkMessagesSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <google/protobuf/descriptor.h>
#include <networksystem/inetworkmessages.h>
#include <networksystem/inetworkserializer.h>
#include <networksystem/netmessage.h>
#include <netmessages.pb.h>

#include "hook/HookArg.h"
#include "testkit/StructInspector.h"

namespace cs2st
{
namespace
{

// ---- the probe messages ------------------------------------------------------
//
// Three registry entries every CS2 build has, chosen so the tables below stay
// honest on a dedicated server: one with scalar-only protobuf fields (the
// round-trip subject), one with sub-messages (the tables the engine ships
// clients), and one whose protobuf class the SDK does not ship at all.
//
// The third is a finding in its own right: CMsgSource1LegacyGameEvent is
// registered by the engine but declared in no .proto under s2sdk/common, so
// the descriptor can be read but the payload cannot be walked. Every plugin
// that fires a Source 1-style event holds the same line in its own copy of
// this table.

struct ProbeMessage
{
	const char* name;
	const char* what;
};

const ProbeMessage kProbeMessages[] = {
	{"CNETMsg_Tick", "the per-tick message; scalar fields only, the round-trip subject"},
	{"CSVCMsg_GameEvent", "the networked form of a game event; sub-messages the engine "
	                      "ships to clients"},
	{"CMsgSource1LegacyGameEvent",
	 "registered by the engine, declared in no .proto the SDK ships"},
	// svc_EncryptedData = 78 is this SDK revision's addition to the SVC_Messages
	// range. Resolving it by name pins the registry entry without touching its
	// payload: the message carries encrypted bytes, so there is nothing to walk.
	{"CSVCMsg_EncryptedData",
	 "new in this SDK revision (svc_EncryptedData = 78); payload is opaque "
	 "encrypted bytes"},
};

/** A name nothing will ever register; null is the only right answer. */
constexpr const char* kNoSuchMessage = "CMsgCs2SdkTestNoSuchMessage";

/** A prefix only CNETMsg_Tick starts with; a genuine prefix for the partial lookup. */
constexpr const char* kPartialPrefix = "CNETMsg_Ti";

/** The value planted in CNETMsg_Tick::tick before serialising. */
constexpr std::uint32_t kRoundTripTickValue = 0x12345678u;

/** kRoundTripTickValue as protobuf text format renders it: decimal. */
constexpr const char* kRoundTripTickDecimal = "305419896";

// ---- bitbuf availability -----------------------------------------------------
//
// The round trip needs bf_write/bf_read. Their constructors live in
// tier1/bitbuf.cpp, which no prebuilt library exports - tier0 contains exactly
// two bitbuf-mentioning symbols, netadr_t::Serialize and netadr_t::Unserialize,
// and neither is a constructor. The CS2SDKTEST_TIER1_BITBUF option compiles
// that translation unit into hl2sdk::tier1 (this suite is why it defaults to
// ON); a build with it off reports the serialization methods as SKIP with this
// reason rather than pretending to cover them.

#if defined(CS2SDKTEST_WITH_BITBUF)
	#define CS2ST_NETMSG_BITBUF 1
#else
	#define CS2ST_NETMSG_BITBUF 0
#endif

constexpr const char* kBitbufSkipReason =
	"needs bf_write/bf_read; tier1/bitbuf.cpp is not in this build "
	"(CS2SDKTEST_TIER1_BITBUF is off)";

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool NetworkMessagesSuite::Acquire(TestContext& context)
{
	messages_ = static_cast<INetworkMessages*>(
		context.QueryInterface("networksystem", NETWORKMESSAGES_INTERFACE_VERSION));

	if (messages_ == nullptr)
	{
		// The two failures say different things, and the second is the
		// finding: a loaded networksystem that stopped answering the version
		// string is update damage this plugin exists to name.
		const Module& module = context.GetModule("networksystem");
		if (!module.IsValid())
		{
			context.UnavailableCall("INetworkMessages", "networksystem is not loaded");
		}
		else
		{
			context.UnavailableCall("INetworkMessages",
				"networksystem is loaded but does not answer '%s'",
				NETWORKMESSAGES_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("INetworkMessages at %p (%s @ networksystem) - and not an IAppSystem: "
	             "its vtable starts at RegisterNetworkCategory, with none of the eleven "
	             "base slots every other Version001 interface here begins with. An "
	             "update that adds them renumbers every row below",
		static_cast<void*>(messages_), NETWORKMESSAGES_INTERFACE_VERSION);
	return true;
}

// -----------------------------------------------------------------------------
//  ExerciseCalls
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::ExerciseCalls(TestContext& context)
{
	tick_message_ = nullptr;
	proto_offsets_agree_ = false;

	ResolveMessages(context);
	ExerciseRegistryLookups(context);
	ExerciseBindingMatrix(context);
	ExerciseAllocation(context);
	ExerciseRoundTrip(context);
	ExerciseMiscReadonly(context);
	InspectLoggingChannel(context);
	NoteSkippedWrites(context);
}

// -----------------------------------------------------------------------------
//  ResolveMessages - the name half of the registry
//
//  Every later section needs a registry entry in hand. This resolves the
//  probe messages through the two name-based entry points, because the whole
//  suite leans on their agreement with the id-based ones below.
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::ResolveMessages(TestContext& context)
{
	context.Note("[registry] the name half: exact and partial lookup have to agree "
	             "about which messages this build registers");

	for (const ProbeMessage& probe : kProbeMessages)
	{
		const std::string target =
			std::string("INetworkMessages::FindNetworkMessage(\"") + probe.name + "\")";

		CS2ST_CALL(context, target, {
			INetworkMessageInternal* message = messages_->FindNetworkMessage(probe.name);
			context.Detail("-> %p  (%s)", static_cast<void*>(message), probe.what);

			if (message == nullptr)
			{
				// Suspect, not Fail: a build can legitimately drop a message
				// type, and the row this leaves is the finding. Which messages
				// each build registers is exactly what a diff across updates
				// compares, so the value is in the row existing, not in a
				// verdict attached to it.
				if (std::strcmp(probe.name, "CSVCMsg_EncryptedData") == 0)
				{
					// Registered by name only for its presence row; this build
					// not having it yet is the ordinary state of a new message.
					context.Detail("not registered by this build (added by a newer "
					               "SDK than the running engine, or scoped out)");
				}
				else
				{
					context.Suspect("a message every prior build registered is absent");
				}
			}
			else if (std::strcmp(probe.name, "CNETMsg_Tick") == 0)
			{
				tick_message_ = message;
			}
		});
	}

	CS2ST_CALL(context, "INetworkMessages::FindNetworkMessage (nonexistent name)", {
		// The negative half. A registry that only ever says yes is as broken
		// as one that only ever says no, and this is the half that costs
		// nothing to check.
		INetworkMessageInternal* message = messages_->FindNetworkMessage(kNoSuchMessage);
		context.Detail("-> %p for \"%s\"", static_cast<void*>(message), kNoSuchMessage);
		if (message != nullptr)
		{
			context.Fail("a message name nothing registers came back non-null");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::FindNetworkMessagePartial", {
		INetworkMessageInternal* message = messages_->FindNetworkMessagePartial(kPartialPrefix);
		context.Detail("-> %p for \"%s\"", static_cast<void*>(message), kPartialPrefix);

		if (message == nullptr)
		{
			if (tick_message_ != nullptr)
			{
				// Suspect: what "partial" means is nowhere in the SDK, so the
				// two entry points disagreeing might be prefix-vs-substring
				// rather than a broken registry. Still printed - this is the
				// call plugins use to find messages by fragment.
				context.Suspect("the exact lookup found CNETMsg_Tick but the partial "
				                "lookup found nothing");
			}
		}
		else
		{
			// "CNETMsg_Ti" prefixes exactly one registered name, whichever way
			// partial matching is defined, so the answer is checkable.
			const char* name = message->GetUnscopedName();
			context.Detail("   resolves to \"%s\"", name != nullptr ? name : "(null)");
			if (name == nullptr || std::strcmp(name, "CNETMsg_Tick") != 0)
			{
				context.Suspect("the partial lookup answered with a name the prefix "
				                "does not match");
			}
		}
	});
}

// -----------------------------------------------------------------------------
//  ExerciseRegistryLookups - the id half of the registry
//
//  FindNetworkMessageById and the no-create form of FindOrCreateNetMessage
//  are two more paths to the same table. Four paths, one entry.
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::ExerciseRegistryLookups(TestContext& context)
{
	context.Note("[registry] the id half: id lookup and the no-create form of the "
	             "creating method, both checked against the name lookup's answer");

	if (tick_message_ == nullptr)
	{
		context.UnavailableCall("INetworkMessages::FindNetworkMessageById",
			"no CNETMsg_Tick entry to take the id from");
		context.UnavailableCall("INetworkMessages::FindOrCreateNetMessage",
			"no CNETMsg_Tick binding to ask with");
		return;
	}

	NetMessageInfo_t* info = nullptr;

	CS2ST_CALL(context, "INetworkMessageInternal::GetNetMessageInfo", {
		info = tick_message_->GetNetMessageInfo();
		context.Detail("-> %p", static_cast<void*>(info));
	});

	if (info == nullptr)
	{
		context.UnavailableCall("INetworkMessages::FindNetworkMessageById",
			"GetNetMessageInfo returned no descriptor, so there is no id to look up");
		return;
	}

	const int message_id = static_cast<int>(info->m_MessageId);

	CS2ST_CALL(context, "INetworkMessages::FindNetworkMessageById", {
		INetworkMessageInternal* by_id = messages_->FindNetworkMessageById(message_id);
		context.Detail("-> %p for id %d (the descriptor's own id)",
			static_cast<void*>(by_id), message_id);
		if (by_id == nullptr)
		{
			context.Fail("an id the descriptor itself reports resolves to nothing");
		}
		else if (by_id != tick_message_)
		{
			context.Fail("name lookup and id lookup returned different registry "
			             "entries for CNETMsg_Tick");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::FindNetworkMessageById (unused id)", {
		// Sounding the id space from the top: the SDK promises no count API
		// and no bound, and 40000 is far above any plausible message count.
		const int unused_id = 40000;
		INetworkMessageInternal* by_id = messages_->FindNetworkMessageById(unused_id);
		context.Detail("-> %p for id %d", static_cast<void*>(by_id), unused_id);
		if (by_id != nullptr)
		{
			// Suspect: an id that far out answering is either a much larger
			// id space than expected or a slot that is no longer the lookup.
			context.Suspect("id 40000 resolved to a message");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::FindOrCreateNetMessage (lookup, no create)", {
		// The creating method with bCreateIfNotFound=false is a fourth
		// read-only path to the table, and the only one that takes the
		// binding and the size alongside the id. Passing the exact
		// descriptor-reported id (not -1, which auto-assigns by name hash)
		// asks whether the creating path agrees with the lookups about which
		// entry an id means.
		INetworkMessageInternal* found = messages_->FindOrCreateNetMessage(
			message_id, info->m_pBinding, sizeof(CNETMsg_Tick), nullptr, false, false);
		context.Detail("-> %p for id %d with bCreateIfNotFound=false",
			static_cast<void*>(found), message_id);
		if (found != nullptr && found != tick_message_)
		{
			context.Fail("the creating path and the name lookup disagree on id %d",
				message_id);
		}
		if (found == nullptr)
		{
			// Suspect: the SDK never says the no-create form must return an
			// existing entry rather than null; printed because four paths to
			// one table agreeing is the point of this section.
			context.Suspect("the no-create lookup returned null for an id the "
			                "descriptor reports");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::GetNetMessageInfo", {
		NetMessageInfo_t* via_registry = messages_->GetNetMessageInfo(tick_message_);
		context.Detail("-> %p (GetNetMessageInfo on the entry said %p)",
			static_cast<void*>(via_registry), static_cast<void*>(info));
		if (via_registry != nullptr && via_registry != info)
		{
			context.Fail("two vtables, one descriptor: they returned different "
			             "addresses for the same message");
		}
	});
}

// -----------------------------------------------------------------------------
//  ExerciseBindingMatrix - IProtobufBinding, and the group tables
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::ExerciseBindingMatrix(TestContext& context)
{
	context.Note("[bindings] the descriptor points at a binding; the binding answers "
	             "the same questions through a vtable, and the protobuf the SDK ships "
	             "answers them a third way");

	if (tick_message_ == nullptr)
	{
		context.UnavailableCall("IProtobufBinding",
			"no CNETMsg_Tick entry to read a binding from");
		return;
	}

	NetMessageInfo_t* info = nullptr;

	CS2ST_CALL(context, "INetworkMessageInternal::GetNetMessageInfo (binding fetch)", {
		info = tick_message_->GetNetMessageInfo();
		context.Detail("-> %p", static_cast<void*>(info));
	});

	if (info == nullptr || !LooksLikeObjectPointer(info->m_pBinding))
	{
		context.UnavailableCall("IProtobufBinding",
			"no descriptor or no binding to call anything on");
		return;
	}

	IProtobufBinding* binding = info->m_pBinding;

	// NetMessageInfo_t's layout is registered by GameEventSuite (see
	// scripts/list-struct-coverage.sh); it is read here, not re-inspected.

	CS2ST_CALL(context, "INetworkMessageInternal::GetUnscopedName", {
		const char* unscoped = tick_message_->GetUnscopedName();
		context.Detail("-> \"%s\" (the registry's key for this entry)",
			unscoped != nullptr ? unscoped : "(null)");
		if (unscoped == nullptr)
		{
			context.Fail("a registered message with no name cannot be looked up");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::GetName", {
		const char* name = binding->GetName();
		context.Detail("-> \"%s\"", name != nullptr ? name : "(null)");
		if (name == nullptr)
		{
			context.Fail("the binding a registered message points at has no name");
		}
		else if (std::strcmp(name, "CNETMsg_Tick") != 0)
		{
			// The binding's name is the protobuf's, the unscoped name is the
			// registry's, and neither is defined to be the other - so Suspect,
			// printed, because the day they diverge is the day a lookup by
			// one name stops finding a message known by the other.
			context.Suspect("the binding's name is not CNETMsg_Tick");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::GetSize", {
		const int size = binding->GetSize();
		context.Detail("-> %d (sizeof(CNETMsg_Tick) as compiled here = %zu)",
			size, sizeof(CNETMsg_Tick));
		if (size <= 0)
		{
			context.Fail("a protobuf C++ object cannot have size %d", size);
		}
		else if (static_cast<std::size_t>(size) != sizeof(CNETMsg_Tick))
		{
			// The engine's protobuf runtime laid this object out; this build
			// compiles the same .proto with the SDK's own protoc. A mismatch
			// means the descriptor or the runtime moved - the thing target #8
			// exists to catch. Suspect rather than Fail because one ABI knob
			// (which CRT owns the internal arena) legitimately differs.
			context.Suspect("the engine's CNETMsg_Tick is %d bytes; ours is %zu",
				size, sizeof(CNETMsg_Tick));
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::GetGroup", {
		const char* group = binding->GetGroup();
		const char* stored = info->m_szGroup.Get();
		context.Detail("-> \"%s\" vs NetMessageInfo_t::m_szGroup \"%s\"",
			group != nullptr ? group : "(null)",
			stored != nullptr ? stored : "(null)");
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
		context.Detail("-> %d", static_cast<int>(buf_type));
		if (buf_type < BUF_DEFAULT || buf_type > BUF_VOICE)
		{
			context.Fail("outside NetChannelBufType_t's four enumerators");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::OkToRedispatch", {
		// The sharpest binding oracle, same as GameEventSuite runs on the
		// event message: one bool read two ways that share no code. A bool
		// and its descriptor copy disagreeing on CNETMsg_Tick is a different
		// message's finding than the event suite's, not a duplicate of it.
		const bool from_binding = binding->OkToRedispatch();
		const bool from_descriptor = info->m_bOkayToRedispatch;
		context.Detail("-> %s vs NetMessageInfo_t::m_bOkayToRedispatch %s",
			from_binding ? "true" : "false", from_descriptor ? "true" : "false");
		if (from_binding != from_descriptor)
		{
			context.Suspect("the binding and the descriptor disagree on redispatch");
		}
	});

	CS2ST_CALL(context, "IProtobufBinding::descriptor (generated header)", {
		// The third, protobuf-side reading: what the SDK's .proto says, as
		// this build compiled it. Read through the generated code's own
		// Reflection surface so the row is computed from the same header the
		// round trip uses, not from a number copied out of the .proto by hand.
		const google::protobuf::Descriptor* descriptor = CNETMsg_Tick::descriptor();
		context.Detail("-> \"%s\", %d fields, field 1 is \"%s\"",
			descriptor->name().c_str(), descriptor->field_count(),
			descriptor->field(0)->name().c_str());
		if (std::strcmp(descriptor->field(0)->name().c_str(), "tick") != 0)
		{
			context.Fail("field 1 of CNETMsg_Tick is not tick; the .proto moved");
		}
	});

	// ---- the group tables ---------------------------------------------------

	CS2ST_CALL(context, "INetworkMessages::GetNetworkGroupCount", {
		const int count = messages_->GetNetworkGroupCount();
		context.Detail("-> %d", count);
		if (count <= 0)
		{
			context.Fail("the registry has groups; a count of %d says the slot is "
			             "probably not GetNetworkGroupCount", count);
		}
	});

	CS2ST_CALL(context, "INetworkMessages::GetNetworkGroupName", {
		// Group 0 is the first registered group, not "no group": ids are
		// handed out as groups are created, and the default group every build
		// has is registered first. Id 0 therefore has a right answer on every
		// build, which makes it the safe row.
		const char* name = messages_->GetNetworkGroupName(0);
		context.Detail("-> \"%s\" for group id 0", name != nullptr ? name : "(null)");
		if (name == nullptr)
		{
			context.Fail("group id 0 has no name");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::GetNetworkGroupName (unused group)", {
		const char* name = messages_->GetNetworkGroupName(255);
		context.Detail("-> \"%s\" for group id 255", name != nullptr ? name : "(null)");
		if (name != nullptr && name[0] != '\0')
		{
			// Suspect: the SDK does not say whether an unknown group id
			// returns null or the empty string; a non-empty name is the
			// surprise on either reading.
			context.Suspect("group id 255 answered with a name");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::GetNetworkGroupColor", {
		const Color color = messages_->GetNetworkGroupColor(0);
		context.Detail("-> r=%d g=%d b=%d a=%d for group id 0",
			color.r(), color.g(), color.b(), color.a());
	});

	CS2ST_CALL(context, "INetworkMessages::FindNetworkGroup", {
		// bCreateIfNotFound defaults to false, so this stays read-only, and
		// the name fed in is the one the previous call just handed out - so
		// this is a lookup whose answer is already known.
		const char* group_zero_name = messages_->GetNetworkGroupName(0);
		if (group_zero_name == nullptr)
		{
			context.Detail("-> no group 0 name to feed back");
			return;
		}
		const NetworkGroupId found = messages_->FindNetworkGroup(group_zero_name, false);
		context.Detail("-> id %d for \"%s\"", static_cast<int>(found), group_zero_name);
		if (found != 0)
		{
			// That the first id is 0 is an assumption; the finding is that
			// name -> id -> name has to close, so a mismatch is Suspect.
			context.Suspect("group 0's name did not round-trip to id 0");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::FindNetworkGroup (nonexistent)", {
		const NetworkGroupId found = messages_->FindNetworkGroup(
			"Cs2SdkTestNoSuchGroup", false);
		context.Detail("-> id %d for a name nothing registered",
			static_cast<int>(found));
		if (found != 0)
		{
			context.Suspect("a group name nothing registered resolved to id %d",
				static_cast<int>(found));
		}
	});
}

// -----------------------------------------------------------------------------
//  ExerciseAllocation - the engine's allocator, and the offset oracle
//
//  SDK note: CNetMessagePB here is a *stub* over the engine's real class
//  ("AMNOTE: This is a stub class over real CNetMessagePB!", netmessage.h).
//  Every s2sdk-cs2 plugin nonetheless writes through ToPB<T>() - the stub's
//  layout is the ecosystem's working ABI assumption. This section measures
//  that assumption before anything writes through it: the engine's own
//  AsProto() says where it thinks the protobuf sub-object starts, our
//  ToPB<T>() says where this build's compiler puts it, and the round trip
//  only writes if the two agree.
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::ExerciseAllocation(TestContext& context)
{
	context.Note("[allocation] the engine's allocator for CNetMessage: allocate, copy "
	             "two ways, read the inline probes - and measure the stub-class offset "
	             "before anything writes through it");

	if (tick_message_ == nullptr)
	{
		context.UnavailableCall("INetworkMessages::AllocateNetMessageAbstract",
			"no CNETMsg_Tick entry to allocate through");
		return;
	}

	CNetMessage* allocated = nullptr;
	CNetMessage* copy = nullptr;

	CS2ST_CALL(context, "INetworkMessageInternal::AllocateMessage", {
		allocated = tick_message_->AllocateMessage();
		context.Detail("-> %p", static_cast<void*>(allocated));
		if (allocated == nullptr)
		{
			context.Fail("the engine's own allocator returned null for a registered "
			             "message type");
		}
	});

	if (allocated == nullptr)
	{
		return;
	}

	// ---- the offset oracle, before any write --------------------------------

	CS2ST_CALL(context, "CNetMessage::AsProto", {
		void* engine_proto = allocated->AsProto();
		// Derived-to-base through this build's stub layout: the compiler
		// applies the same offset a write through ToPB would.
		CNETMsg_Tick* local_proto = allocated->ToPB<CNETMsg_Tick>();
		context.Detail("-> engine %p, this build %p", engine_proto,
			static_cast<void*>(local_proto));
		if (!LooksLikeObjectPointer(engine_proto))
		{
			context.Fail("AsProto returned something that is not an object pointer");
			proto_offsets_agree_ = false;
		}
		else
		{
			proto_offsets_agree_ = (engine_proto == static_cast<void*>(local_proto));
			if (!proto_offsets_agree_)
			{
				// Not merely Suspect-and-carry-on: the round trip below refuses
				// to write through the stub until this passes, because a write
				// at the wrong offset lands in some other engine object.
				context.Suspect("the engine reaches the protobuf at a different "
				                "address than this build computes; the round trip "
				                "will not write through the stub");
			}
		}
	});

	CS2ST_CALL(context, "CNetMessage::AsProto2", {
		// The second accessor, unidentified in the SDK. Printed only; on the
		// real class it may return the same sub-object, a different one, or
		// an interior pointer, and none of those readings is promised.
		void* as_proto2 = allocated->AsProto2();
		context.Detail("-> %p", as_proto2);
	});

	CS2ST_CALL(context, "CNetMessage::GetNetMessage", {
		// The registry entry an allocated message carries, against the entry
		// it was allocated through: an oracle the engine cannot get wrong
		// twice, because both sides of it come out of the engine.
		INetworkMessageInternal* entry = allocated->GetNetMessage();
		context.Detail("-> %p (allocated through %p)",
			static_cast<void*>(entry), static_cast<void*>(tick_message_));
		if (entry == nullptr)
		{
			context.Fail("an engine-allocated message carries no registry entry");
		}
		else if (entry != tick_message_)
		{
			context.Fail("the message's own entry differs from the one it was "
			             "allocated through");
		}
	});

	CS2ST_CALL(context, "CNetMessage::GetMessageId", {
		// Slot the SDK named in the Oct 2026 rework (it used to sit inside the
		// old CopyConstruct(other) signature). Two routes to the same id: the
		// message's own vtable and the descriptor the registry hands out.
		const NetworkMessageId id = allocated->GetMessageId();
		context.Detail("-> %u (descriptor says %u)",
			static_cast<unsigned>(id),
			static_cast<unsigned>(tick_message_->GetNetMessageInfo()->m_MessageId));
		if (tick_message_->GetNetMessageInfo() != nullptr
			&& id != tick_message_->GetNetMessageInfo()->m_MessageId)
		{
			context.Fail("the message and its registry descriptor report different ids");
		}
	});

	CS2ST_CALL(context, "CNetMessage::GetName", {
		// Same rework, same shape of oracle: the message's name against the
		// binding's own GetName, reached down two different vtables.
		const char* name = allocated->GetName();
		context.Detail("-> \"%s\"", name != nullptr ? name : "(null)");
		if (name == nullptr || std::strcmp(name, "CNETMsg_Tick") != 0)
		{
			context.Fail("a CNETMsg_Tick allocation reports a different name");
		}
	});

	CS2ST_CALL(context, "CNetMessage::GetMargin", {
		// A plain load of an inline float member, not a virtual call - the
		// same class of read as CCommand::ArgC() in the clients suite. The
		// value is not judged: CNetMessage's constructor is deleted and the
		// SDK calls its own declaration a stub, so a fresh allocation holds
		// whatever the allocator left there.
		context.Detail("-> %.3f", static_cast<double>(allocated->GetMargin()));
	});

	CS2ST_CALL(context, "INetworkMessages::AllocateAndCopyConstructNetMessageAbstract", {
		copy = messages_->AllocateAndCopyConstructNetMessageAbstract(
			tick_message_, allocated);
		context.Detail("-> %p (copy of %p)", static_cast<void*>(copy),
			static_cast<void*>(allocated));
		if (copy == nullptr)
		{
			context.Fail("copy-constructing an engine-allocated message returned null");
		}
	});

	CS2ST_CALL(context, "CNetMessage::CopyConstruct", {
		// The message's own copy path, on the same object - the vtable slot
		// the registry helper presumably ends up inside. The SDK reworked this
		// slot from CopyConstruct(const CNetMessage*) to a nullary copy: the
		// source is the object it is called on.
		CNetMessage* via_message = allocated->CopyConstruct();
		context.Detail("-> %p", static_cast<void*>(via_message));
		if (via_message != nullptr)
		{
			messages_->DeallocateNetMessageAbstract(tick_message_, via_message);
		}
	});

	if (copy != nullptr)
	{
		CS2ST_CALL(context, "INetworkMessages::DeallocateNetMessageAbstract (copy)", {
			messages_->DeallocateNetMessageAbstract(tick_message_, copy);
			context.Detail("-> freed %p", static_cast<void*>(copy));
		});
		copy = nullptr;
	}

	CS2ST_CALL(context, "INetworkMessages::DeallocateNetMessageAbstract", {
		messages_->DeallocateNetMessageAbstract(tick_message_, allocated);
		context.Detail("-> freed %p", static_cast<void*>(allocated));
	});
	allocated = nullptr;
}

// -----------------------------------------------------------------------------
//  ExerciseRoundTrip - plant a value, serialise, read back
//
//  The deepest oracle in the suite. The chain is: allocate an engine-owned
//  CNETMsg_Tick, set tick through the generated protobuf accessor, serialise
//  through INetworkMessages, and read the value back through two unserialize
//  entry points. A protobuf descriptor change breaks this before it breaks
//  anything a plugin would notice by other means.
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::ExerciseRoundTrip(TestContext& context)
{
	if (tick_message_ == nullptr)
	{
		context.UnavailableCall("INetworkMessages::SerializeMessageInternal",
			"no CNETMsg_Tick entry to serialise through");
		return;
	}

#if CS2ST_NETMSG_BITBUF

	context.Note("[round trip] CNETMsg_Tick::set_tick(0x%08x) out through "
	             "SerializeMessageInternal and back through two unserialize entry "
	             "points", kRoundTripTickValue);

	CNetMessage* message = nullptr;
	CNetMessage* typed_target = nullptr;
	CNetMessage* from_stream = nullptr;
	CNetMessage* forwarded_target = nullptr;

	// ---- allocate and fill --------------------------------------------------

	CS2ST_CALL(context, "INetworkMessageInternal::AllocateMessage (round trip)", {
		message = tick_message_->AllocateMessage();
		context.Detail("-> %p", static_cast<void*>(message));
	});

	if (message == nullptr)
	{
		context.UnavailableCall("INetworkMessages::SerializeMessageInternal",
			"allocation failed, so there is no message to serialise");
		return;
	}

	if (!proto_offsets_agree_)
	{
		// Measured one section up. The write is refused rather than risked:
		// a stub-layout mismatch means set_tick lands outside the protobuf,
		// in whatever engine object follows it.
		context.SkipCall("CNetMessagePB<CNETMsg_Tick>::set_tick",
			"AsProto and this build's ToPB disagreed on the protobuf address, "
			"so a write through the stub cannot be aimed");
		context.SkipCall("INetworkMessages::SerializeMessageInternal",
			"no filled message: the set_tick above was refused");
		message_free_section(context, message, typed_target, from_stream,
			forwarded_target);
		return;
	}

	CS2ST_CALL(context, "CNetMessagePB<CNETMsg_Tick>::set_tick", {
		message->ToPB<CNETMsg_Tick>()->set_tick(kRoundTripTickValue);
		context.Detail("-> tick set to 0x%08x through the generated accessor",
			kRoundTripTickValue);
	});

	// ---- serialise ----------------------------------------------------------
	//
	// bf_write/bf_read come from tier1/bitbuf.cpp via CS2SDKTEST_TIER1_BITBUF;
	// the constructors are exported by no prebuilt library. The message being
	// serialised is engine-allocated and engine-freed, so no allocation this
	// module makes crosses the boundary - the one protobuf ABI knob that
	// differs (which CRT owns the arena) never gets turned.

	alignas(8) unsigned char buffer[512] = {};
	int written = 0;
	bool wrote_ok = false;

	{
		bf_write writer(buffer, static_cast<int>(sizeof(buffer)));
		CS2ST_CALL(context, "INetworkMessages::SerializeMessageInternal", {
			wrote_ok = messages_->SerializeMessageInternal(writer, message);
			written = writer.GetNumBytesWritten();
			context.Detail("-> %s, %d bytes", wrote_ok ? "true" : "false", written);
		});
	}

	if (!wrote_ok || written <= 0)
	{
		context.Fail("serialising a freshly allocated, freshly filled CNETMsg_Tick "
		             "produced nothing");
		context.SkipCall("INetworkMessages::UnserializeMessageInternal",
			"the serializer wrote nothing to read back");
		context.SkipCall("INetworkMessages::UnserializeFromStream",
			"the serializer wrote nothing to read back");
		message_free_section(context, message, typed_target, from_stream,
			forwarded_target);
		return;
	}

	// ---- read back, typed ----------------------------------------------------
	//
	// UnserializeMessageInternal reads into a caller-provided message, so the
	// type is known by construction: the target is a Tick, the source was a
	// Tick. This is the strictest form of the round trip.

	{
		bf_read reader(buffer, written);
		CS2ST_CALL(context, "INetworkMessages::UnserializeMessageInternal", {
			typed_target = tick_message_->AllocateMessage();
			if (typed_target == nullptr)
			{
				context.Detail("-> no target message to read into");
				return;
			}
			const bool ok = messages_->UnserializeMessageInternal(reader, typed_target);
			context.Detail("-> %s into %p", ok ? "true" : "false",
				static_cast<void*>(typed_target));
			if (!ok)
			{
				// Suspect: whether the internal serialize/unserialize pair is
				// a matched pair is something the SDK asserts nowhere.
				context.Suspect("the internal serialize/unserialize pair did not "
				                "close on a healthy message");
			}
		});
	}

	if (typed_target != nullptr)
	{
		CS2ST_CALL(context, "CNetMessagePB<CNETMsg_Tick>::tick (typed read)", {
			const std::uint32_t value = typed_target->ToPB<CNETMsg_Tick>()->tick();
			context.Detail("-> 0x%08x (planted 0x%08x)", value, kRoundTripTickValue);
			if (value != kRoundTripTickValue)
			{
				// The .proto declares field 1 = tick, uint32. A successful
				// unserialize that yields a different value is a descriptor
				// that moved - the one thing this row exists to catch.
				context.Fail("the tick value did not survive the round trip");
			}
		});
	}

	// ---- read back, type-detecting -------------------------------------------

	{
		CUtlString err_reason;
		bf_read reader(buffer, written);
		CS2ST_CALL(context, "INetworkMessages::UnserializeFromStream", {
			from_stream = messages_->UnserializeFromStream(reader, err_reason);
			context.Detail("-> %p, err \"%s\"", static_cast<void*>(from_stream),
				err_reason.Get());
		});
	}

	if (from_stream == nullptr)
	{
		// Suspect, not Fail: whether this entry point consumes exactly what
		// SerializeMessageInternal writes (id-prefixed or bare payload) is
		// not stated anywhere in the SDK, so a null here may be a format
		// mismatch rather than damage.
		context.Suspect("the type-detecting unserialize did not accept what the "
		                "serializer wrote");
	}
	else
	{
		CS2ST_CALL(context, "CNetMessagePB<CNETMsg_Tick>::tick (stream read)", {
			// ToPB is an unchecked static_cast ("Doesn't do any validity
			// checks itself!", says the SDK), so the entry is verified before
			// the payload is touched: the message the stream produced has to
			// belong to the same registry entry the planted one did.
			INetworkMessageInternal* entry = from_stream->GetNetMessage();
			context.Detail("-> entry %p (planted through %p)",
				static_cast<void*>(entry), static_cast<void*>(tick_message_));
			if (entry != tick_message_)
			{
				context.Suspect("the stream produced a message of a different type; "
				                "its payload is not read");
				return;
			}
			const std::uint32_t value = from_stream->ToPB<CNETMsg_Tick>()->tick();
			context.Detail("-> tick 0x%08x (planted 0x%08x)", value, kRoundTripTickValue);
			if (value != kRoundTripTickValue)
			{
				context.Fail("the tick value did not survive the stream round trip");
			}
		});
	}

	// ---- the entry's forwarding pair ------------------------------------------

	{
		bf_read reader(buffer, written);
		CS2ST_CALL(context, "INetworkMessageInternal::Unserialize", {
			// The SDK's own comment: this forwards to
			// INetworkMessages::UnserializeMessageInternal. Called on the
			// entry so the forwarding vtable slot itself is exercised.
			forwarded_target = tick_message_->AllocateMessage();
			if (forwarded_target == nullptr)
			{
				context.Detail("-> no target message to read into");
				return;
			}
			const bool ok = tick_message_->Unserialize(reader, forwarded_target);
			context.Detail("-> %s into %p", ok ? "true" : "false",
				static_cast<void*>(forwarded_target));
		});
	}

	if (forwarded_target != nullptr)
	{
		CS2ST_CALL(context, "CNetMessagePB<CNETMsg_Tick>::tick (forwarded read)", {
			const std::uint32_t value = forwarded_target->ToPB<CNETMsg_Tick>()->tick();
			context.Detail("-> 0x%08x (planted 0x%08x)", value, kRoundTripTickValue);
			if (value != kRoundTripTickValue)
			{
				context.Fail("the tick value did not survive the forwarded round trip");
			}
		});
	}

	// ---- the two remaining writers, and the binding's renderer ----------------

	{
		unsigned char sink[512] = {};
		bf_write writer(sink, static_cast<int>(sizeof(sink)));
		CS2ST_CALL(context, "INetworkMessages::Serialize (public)", {
			const bool ok = messages_->Serialize(writer, message);
			context.Detail("-> %s, %d bytes (against %d from the internal serializer)",
				ok ? "true" : "false", writer.GetNumBytesWritten(), written);
		});
	}

	{
		unsigned char sink[512] = {};
		bf_write writer(sink, static_cast<int>(sizeof(sink)));
		CS2ST_CALL(context, "INetworkMessages::SerializeAbstract", {
			const bool ok = messages_->SerializeAbstract(writer, tick_message_, message);
			context.Detail("-> %s, %d bytes", ok ? "true" : "false",
				writer.GetNumBytesWritten());
		});
	}

	{
		unsigned char sink[512] = {};
		bf_write writer(sink, static_cast<int>(sizeof(sink)));
		CS2ST_CALL(context, "INetworkMessageInternal::Serialize", {
			const bool ok = tick_message_->Serialize(writer, message);
			context.Detail("-> %s, %d bytes", ok ? "true" : "false",
				writer.GetNumBytesWritten());
		});
	}

	{
		CUtlString rendered;
		CS2ST_CALL(context, "IProtobufBinding::ToString", {
			// The fourth reading of the payload: the engine's own renderer,
			// handed the protobuf address AsProto reported (not this build's
			// computed offset - the same caution as set_tick, in reverse).
			// CUtlString's heap lives in tier0 on both platforms, so the
			// engine writing into a stack CUtlString of ours frees through
			// the same allocator it allocated with.
			NetMessageInfo_t* info = tick_message_->GetNetMessageInfo();
			const char* text = info->m_pBinding->ToString(
				message->AsProto(), rendered);
			const char* shown = text != nullptr ? text : rendered.Get();
			context.Detail("-> %.96s", shown != nullptr ? shown : "(null)");
			if (shown != nullptr && std::strstr(shown, kRoundTripTickDecimal) == nullptr)
			{
				// Suspect: the engine may render protobuf text format with
				// hex or field-number prefixes, so absence of the decimal is
				// not proof - but presence is the oracle, and its absence
				// after three successful readers above is worth a row.
				context.Suspect("the engine's rendering does not contain the "
				                "planted tick value");
			}
		});
	}

	message_free_section(context, message, typed_target, from_stream,
		forwarded_target);

#else  // !CS2ST_NETMSG_BITBUF

	context.Note("[round trip] skipped whole: this build has no bitbuf");
	context.SkipCall("INetworkMessages::SerializeMessageInternal", "%s", kBitbufSkipReason);
	context.SkipCall("INetworkMessages::UnserializeMessageInternal", "%s", kBitbufSkipReason);
	context.SkipCall("INetworkMessages::UnserializeFromStream", "%s", kBitbufSkipReason);
	context.SkipCall("INetworkMessages::Serialize (public)", "%s", kBitbufSkipReason);
	context.SkipCall("INetworkMessages::SerializeAbstract", "%s", kBitbufSkipReason);
	context.SkipCall("INetworkMessageInternal::Serialize", "%s", kBitbufSkipReason);
	context.SkipCall("INetworkMessageInternal::Unserialize", "%s", kBitbufSkipReason);
	context.SkipCall("CNetMessagePB<CNETMsg_Tick>::set_tick / tick",
		"no message is allocated without the round trip to put it through");
	context.SkipCall("IProtobufBinding::ToString",
		"the filled message only exists inside the round trip");

#endif  // CS2ST_NETMSG_BITBUF
}

// -----------------------------------------------------------------------------
//  ExerciseMiscReadonly - the remaining read-only surface
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::ExerciseMiscReadonly(TestContext& context)
{
	context.Note("[misc] the read-only remainder: mode flags, callback order, "
	             "serialization context, priority ordering");

	CS2ST_CALL(context, "INetworkMessages::GetIsForServer", {
		const bool is_for_server = messages_->GetIsForServer();
		// Reported, not judged: a dedicated server's registry is server-side,
		// but the SDK does not promise the flag reads true there.
		context.Detail("-> %s", is_for_server ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkMessages::IsAdditionalMessageRegistrationAllowed", {
		const bool allowed = messages_->IsAdditionalMessageRegistrationAllowed();
		context.Detail("-> %s", allowed ? "true" : "false");
	});

	CS2ST_CALL(context, "INetworkMessages::GetFieldChangeCallbackOrderCount", {
		const int count = messages_->GetFieldChangeCallbackOrderCount();
		context.Detail("-> %d", count);
		if (count < 0)
		{
			context.Fail("a count cannot be negative");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::GetFieldChangeCallbackPriorities", {
		// The SDK contract: nCount entries are written into nPriorities. The
		// count came from the previous call, so the write is exactly as large
		// as the engine says. Capped so a nonsense count writes at most this
		// many ints, never an arbitrary amount.
		const int reported = messages_->GetFieldChangeCallbackOrderCount();
		if (reported <= 0)
		{
			context.Detail("-> no priorities to fetch");
			return;
		}
		const int count = reported > 32 ? 32 : reported;
		int priorities[32] = {};
		messages_->GetFieldChangeCallbackPriorities(count, priorities);
		context.Detail("-> wrote %d entr%s (reported %d)%s",
			count, count == 1 ? "y" : "ies", reported,
			reported > 32 ? " (capped at 32)" : "");
	});

	CS2ST_CALL(context, "INetworkMessages::GetNetworkSerializationContextData", {
		// A context name nothing set: zero is the only answer that keeps this
		// read-only, so a handle back means the slot is no longer the lookup.
		// The SDK typed this uint16 (it used to be void*) in the Oct 2026
		// rework; unk101 below is what turns the handle back into the pointer.
		const uint16 data = messages_->GetNetworkSerializationContextData(
			"Cs2SdkTestNoSuchContext");
		context.Detail("-> %u for a context name nothing set",
			static_cast<unsigned>(data));
		if (data != 0)
		{
			context.Suspect("an unset serialization context answered with a handle");
		}
	});

	CS2ST_CALL(context, "INetworkMessages::ComputeOrderForPriority", {
		// A pure function of its argument if anything on this interface is:
		// the ordering of callback priorities. Value reported, not judged -
		// the mapping is engine policy.
		const int order = messages_->ComputeOrderForPriority(0);
		context.Detail("-> %d for priority 0", order);
	});
}

// -----------------------------------------------------------------------------
//  InspectLoggingChannel - CLoggingSystem::LoggingChannel_t layout
//
//  GetLoggingChannel used to hand back the CLoggingSystem::LoggingChannel_t
//  pointer itself; the Oct 2026 SDK rework types it as the channel *id*, and
//  tier0's exported LoggingSystem_GetChannel turns the id back into the
//  struct. Two steps, two modules - and the struct itself is one the engine
//  fills at startup, so every field holds a value.
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::InspectLoggingChannel(TestContext& context)
{
	context.Note("[LoggingChannel_t] the registry's own logging channel - a struct the "
	             "engine fills at startup, so every field holds a value");

	CLoggingSystem::LoggingChannel_t* channel = nullptr;
	LoggingChannelID_t channel_id = INVALID_LOGGING_CHANNEL_ID;

	CS2ST_CALL(context, "INetworkMessages::GetLoggingChannel", {
		channel_id = messages_->GetLoggingChannel();
		context.Detail("-> channel id %d", static_cast<int>(channel_id));
		if (channel_id == INVALID_LOGGING_CHANNEL_ID)
		{
			context.Fail("a live registry has a logging channel");
		}
	});

	if (channel_id == INVALID_LOGGING_CHANNEL_ID)
	{
		context.SkipCall("LoggingChannel_t",
			"GetLoggingChannel did not return a channel id");
		return;
	}

	// Bounds before anything indexes with it: the SDK's own CLoggingSystem::
	// GetChannel is now an inline m_RegisteredChannels[id], so an id past the
	// array would walk off the struct. MAX_LOGGING_CHANNEL_COUNT is the SDK's
	// own bound.
	if (channel_id < 0 || channel_id >= MAX_LOGGING_CHANNEL_COUNT)
	{
		context.Fail("channel id %d is outside the SDK's channel array",
			static_cast<int>(channel_id));
		context.SkipCall("LoggingChannel_t", "no in-range channel to inspect");
		return;
	}

	CS2ST_CALL(context, "LoggingSystem_GetChannel (tier0)", {
		channel = const_cast<CLoggingSystem::LoggingChannel_t*>(
			LoggingSystem_GetChannel(channel_id));
		context.Detail("-> %p", static_cast<void*>(channel));
		if (!LooksLikeObjectPointer(channel))
		{
			context.Fail("tier0 resolved the registry's own channel id to nothing");
			channel = nullptr;
		}
	});

	if (channel == nullptr)
	{
		context.SkipCall("LoggingChannel_t",
			"LoggingSystem_GetChannel did not return an instance");
		return;
	}

	StructInspector inspect(context, "CLoggingSystem::LoggingChannel_t",
		channel, sizeof(CLoggingSystem::LoggingChannel_t));

	inspect.Int32  ("m_ID",               offsetof(CLoggingSystem::LoggingChannel_t, m_ID));
	// The one field here with a known-correct value: the id GetLoggingChannel
	// handed back is by definition this channel's own index.
	if (channel->m_ID != channel_id)
	{
		CS2ST_CALL(context, "LoggingChannel_t::m_ID vs GetLoggingChannel", {
			context.Detail("-> struct says %d, interface said %d",
				channel->m_ID, static_cast<int>(channel_id));
			context.Fail("the channel struct does not carry the id that resolved it");
		});
	}
	// Two flag words, each a bitmask the SDK enumerates; raw because the legal
	// set is engine policy, not type law.
	inspect.UInt32 ("m_DefaultFlags",     offsetof(CLoggingSystem::LoggingChannel_t, m_DefaultFlags));
	inspect.UInt32 ("m_Flags",            offsetof(CLoggingSystem::LoggingChannel_t, m_Flags));
	// LV_OFF(0)..LV_MAX(4): a five-value enum, so the Enum check applies.
	inspect.Enum   ("m_DefaultVerbosity", offsetof(CLoggingSystem::LoggingChannel_t, m_DefaultVerbosity), 0, 4);
	inspect.Enum   ("m_Verbosity",        offsetof(CLoggingSystem::LoggingChannel_t, m_Verbosity), 0, 4);
	inspect.Int32  ("m_nIndent",          offsetof(CLoggingSystem::LoggingChannel_t, m_nIndent));
	inspect.Raw    ("m_SpewColor",        offsetof(CLoggingSystem::LoggingChannel_t, m_SpewColor), sizeof(Color));
	// char[32]: inline storage, so String() - which reads a pointer - cannot
	// apply. Raw bytes; the hex dump shows the name directly.
	inspect.Raw    ("m_Name",             offsetof(CLoggingSystem::LoggingChannel_t, m_Name), MAX_LOGGING_IDENTIFIER_LENGTH);
	inspect.Pointer("m_pFirstTag",        offsetof(CLoggingSystem::LoggingChannel_t, m_pFirstTag));
	inspect.UInt32 ("m_nModifiedFields",  offsetof(CLoggingSystem::LoggingChannel_t, m_nModifiedFields));

	inspect.Summarize();
	inspect.HexDump();

	CS2ST_CALL(context, "LoggingChannel_t::m_Name (read)", {
		// The channel's name straight out of the inline array - no vtable, no
		// protobuf, just bytes at an offset the SDK declares. %.32s caps at
		// the array's own length, so an unterminated name cannot walk.
		const char* name = reinterpret_cast<const char*>(
			reinterpret_cast<const unsigned char*>(channel) +
			offsetof(CLoggingSystem::LoggingChannel_t, m_Name));
		context.Detail("-> \"%.32s\"", name);
	});
}

// -----------------------------------------------------------------------------
//  NoteSkippedWrites - the mutating half of the interface
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::NoteSkippedWrites(TestContext& context)
{
	context.Note("[writes] every method that would alter the registry, a message, or "
	             "engine-owned tables - one row each, so a silent gap cannot read as "
	             "coverage");

	context.SkipCall("INetworkMessages::RegisterNetworkCategory",
		"adds a category to the engine's routing tables; runs at registry build time, "
		"which has already happened by plugin load");
	context.SkipCall("INetworkMessages::AssociateNetMessageWithChannelCategoryAbstract",
		"binds a message into a category the engine routes by");
	context.SkipCall("INetworkMessages::FindOrCreateNetMessage (create)",
		"with bCreateIfNotFound=true it inserts a registry entry the engine never "
		"unregisters; the lookup form is covered above");
	context.SkipCall("INetworkMessages::RegisterNetworkFieldSerializer",
		"twelve function pointers into engine-owned serializer tables");
	context.SkipCall("INetworkMessages::RegisterNetworkArrayFieldSerializer",
		"the same, for array fields");
	context.SkipCall("INetworkMessages::AssociateNetMessageGroupIdWithChannelCategory",
		"cross-links the group and category tables");
	context.SkipCall("INetworkMessages::SetNetworkSerializationContextData",
		"stores a pointer the engine reads while serialising; the lookup half is "
		"covered above");
	context.SkipCall("INetworkMessages::RegisterNetworkFieldChangeCallbackInternal",
		"a CUtlAbstractDelegate into the engine's change-callback list, which would "
		"keep pointing at this module after unload");
	context.SkipCall("INetworkMessages::AllowAdditionalMessageRegistration",
		"flips the registration policy flag for every plugin on the server");
	context.SkipCall("INetworkMessages::RegisterFieldChangeCallbackPriority",
		"grows the engine's priority table");
	context.SkipCall("INetworkMessages::SetIsForServer",
		"flips the registry between server and client serialisation; the getter is "
		"covered above");
	context.SkipCall("INetworkMessages::RegisterSchemaTypeOverride",
		"rewrites schema-type resolution for named fields");
	context.SkipCall("INetworkMessages::unk101",
		"resolves a serialization-context handle back to the pointer "
		"SetNetworkSerializationContextData stored; the handle half is covered "
		"above, but calling this on the zero handle nothing set is not a probe "
		"this suite can interpret");
	context.SkipCall("INetworkMessages::unk102", "unidentified in the SDK");
	context.SkipCall("INetworkMessages::unk201", "unidentified in the SDK");
	context.SkipCall("INetworkMessages::unk202", "unidentified in the SDK");
	context.SkipCall("INetworkMessages::~INetworkMessages",
		"would destroy the registry every message on the server depends on");
	context.SkipCall("INetworkMessageInternal::SetMessageId",
		"renumbers a message the engine has already handed to clients");
	context.SkipCall("INetworkMessageInternal::AddCategoryMask",
		"changes how the engine routes every message of this type");
	context.SkipCall("INetworkMessageInternal::SwitchMode",
		"flips the registry entry between server and client serialisation");
	context.SkipCall("INetworkMessageInternal::~INetworkMessageInternal",
		"would destroy a registry entry the engine still owns");
	context.SkipCall("CNetMessage::~CNetMessage",
		"would destroy a message only DeallocateNetMessageAbstract may free");
	context.SkipCall("IProtobufBinding::AllocateMessage",
		"allocates an engine-owned CNetMessage with no matching free on this "
		"interface; INetworkMessageInternal::AllocateMessage is the covered pair");
	context.SkipCall("IProtobufBinding::unk001", "unidentified in the SDK");
}

// -----------------------------------------------------------------------------
//  Freeing - every allocation returned to the engine that lent it
//
//  A private member rather than a lambda so the early exits in the round trip
//  share one free path: a leaked engine allocation outlives the sweep, and a
//  double free kills the server, so both failures need "impossible" to be the
//  easy property to check.
// -----------------------------------------------------------------------------

void NetworkMessagesSuite::message_free_section(TestContext& context,
                                                CNetMessage* message,
                                                CNetMessage* typed_target,
                                                CNetMessage* from_stream,
                                                CNetMessage* forwarded_target)
{
	if (typed_target != nullptr)
	{
		CS2ST_CALL(context, "INetworkMessages::DeallocateNetMessageAbstract (typed)", {
			messages_->DeallocateNetMessageAbstract(tick_message_, typed_target);
			context.Detail("-> freed %p", static_cast<void*>(typed_target));
		});
	}
	if (from_stream != nullptr)
	{
		CS2ST_CALL(context, "INetworkMessages::DeallocateNetMessageAbstract (stream)", {
			messages_->DeallocateNetMessageAbstract(tick_message_, from_stream);
			context.Detail("-> freed %p", static_cast<void*>(from_stream));
		});
	}
	if (forwarded_target != nullptr)
	{
		CS2ST_CALL(context,
			"INetworkMessages::DeallocateNetMessageAbstract (forwarded)", {
			messages_->DeallocateNetMessageAbstract(tick_message_, forwarded_target);
			context.Detail("-> freed %p", static_cast<void*>(forwarded_target));
		});
	}
	if (message != nullptr)
	{
		CS2ST_CALL(context, "INetworkMessages::DeallocateNetMessageAbstract (filled)", {
			messages_->DeallocateNetMessageAbstract(tick_message_, message);
			context.Detail("-> freed %p", static_cast<void*>(message));
		});
	}
}

} // namespace cs2st
