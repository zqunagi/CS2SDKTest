#include "suites/NetChannelSuite.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <interfaces/interfaces.h>
#include <networksystem/inetchannel.h>
#include <networksystem/inetworksystem.h>
#include <tier0/netadr.h>
#include <tier0/ns_address.h>

#include "core/Log.h"

// The channel is built here, so there is no player behind it and none of its
// strings is anyone's address. Strings are still reported by length and
// addresses by validity: the same rows would run unchanged on a real client's
// channel, and the result files are published.

namespace cs2st
{
namespace
{

// The channel's name as the engine will log it.
constexpr const char* kChannelName = "cs2sdktest";

// The SDK gives no values for NetworkCategoryId, so the (send, receive) pairs
// are tried in order until the engine hands a channel back. Each try is its own
// guarded row, so which pair the engine accepted is in the log. Four is all of
// 0 and 1 in both directions; a wider search would be guessing.
struct CategoryPair
{
	NetworkCategoryId send;
	NetworkCategoryId receive;
};
constexpr CategoryPair kCategoryCandidates[] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};

// Capacity handed to GetFrameJitterHistory; the call may write at most this
// many entries, which is also the bound checked on its return value.
constexpr int kJitterEntries = 16;
// A string longer than this is not a name or an address.
constexpr std::size_t kMaxPlausibleStringLength = 256;

const char* FlowName(int flow)
{
	switch (flow)
	{
		case FLOW_OUTGOING: return "FLOW_OUTGOING";
		case FLOW_INCOMING: return "FLOW_INCOMING";
		default:            return "<out of range>";
	}
}

// Length of a C string the engine returned, bounded: a pointer that is not a
// string must not turn the probe into an unbounded walk.
std::size_t BoundedLength(const char* text)
{
	std::size_t length = 0;
	while (length < kMaxPlausibleStringLength + 1 && text[length] != '\0')
	{
		++length;
	}
	return length;
}

// Row label with the flow in it. Built outside CS2ST_CALL: the macro takes two
// arguments, so a format call with commas in the target would split.
template <std::size_t N>
void FlowLabel(char (&label)[N], const char* method, int flow)
{
	std::snprintf(label, N, "INetChannelInfo::%s [%s]", method, FlowName(flow));
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool NetChannelSuite::Acquire(TestContext& context)
{
	network_ = static_cast<INetworkSystem*>(
		context.QueryInterface("networksystem", NETWORKSYSTEM_INTERFACE_VERSION));

	if (network_ == nullptr)
	{
		const Module& network_module = context.GetModule("networksystem");
		if (!network_module.IsValid())
		{
			context.UnavailableCall("INetworkSystem", "the networksystem module is not loaded");
		}
		else
		{
			context.UnavailableCall("INetworkSystem",
				"networksystem is loaded but does not answer '%s'",
				NETWORKSYSTEM_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("channels come from INetworkSystem at %p (%s)",
		static_cast<void*>(network_), NETWORKSYSTEM_INTERFACE_VERSION);
	return true;
}

void NetChannelSuite::ExerciseCalls(TestContext& context)
{
	if (!CreateChannel(context))
	{
		// Reported once rather than as 88 identical lines. The audit script
		// still sees every method below: they are all called once a channel
		// exists.
		context.UnavailableCall("INetChannelInfo",
			"the engine did not return a channel, so there is nothing to ask");
		context.UnavailableCall("INetChannel",
			"the engine did not return a channel, so there is nothing to ask");
		return;
	}

	ExerciseInfo(context);
	ExerciseChannel(context);
	ExerciseWrites(context);
	NoteSideEffectingCalls(context);
	DestroyChannel(context);
}

// -----------------------------------------------------------------------------
//  Building and removing the channel
// -----------------------------------------------------------------------------

/**
 * Asks the engine for a playback channel. Playback is the one kind that needs
 * no socket traffic and no Steam connection (it exists for demo playback), so
 * nothing has to be connected for it to exist - which is the point: the
 * automated runs have no client.
 *
 * Assumptions, none of them stated by the SDK:
 *  - the address argument is not read for a playback channel, so it is zeroed
 *    storage rather than an address we invented;
 *  - the socket argument is NS_SERVER, the id the enum names for the game
 *    server, rather than an index we made up;
 *  - the category ids are one of the pairs in kCategoryCandidates.
 * Everything runs on the thread the sweep runs on and the channel is removed
 * before the sweep ends, so no tick can see it.
 */
bool NetChannelSuite::CreateChannel(TestContext& context)
{
	context.Note("[INetworkSystem] building a playback channel (no client needed)");

	channel_ = nullptr;
	info_ = nullptr;

	for (const CategoryPair& pair : kCategoryCandidates)
	{
		char label[96];
		std::snprintf(label, sizeof(label),
			"INetworkSystem::CreateNetChannel [send %u, receive %u, playback]",
			static_cast<unsigned>(pair.send), static_cast<unsigned>(pair.receive));

		const CallOutcome outcome = context.RunCall(label, [&]() {
			// Zeroed raw storage, not "ns_address address;": zero is the
			// neutral state and there is no constructor whose work we want.
			alignas(ns_address) unsigned char address[sizeof(ns_address)] = {};
			INetChannel* created = network_->CreateNetChannel(NS_SERVER,
				reinterpret_cast<const ns_address*>(address), k_HSteamNetConnection_Invalid,
				kChannelName, pair.send, pair.receive, true);
			context.Detail("-> %p", static_cast<void*>(created));
			if (created != nullptr)
			{
				channel_ = created;
				info_ = created;
			}
		});

		if (channel_ != nullptr)
		{
			return true;
		}
		if (outcome == CallOutcome::kCrashed)
		{
			// A fault part-way through creation may have left half a channel
			// in the engine's list; trying further pairs would stack more.
			context.Note("the engine faulted creating a channel; not trying other categories");
			return false;
		}
	}

	return false;
}

void NetChannelSuite::DestroyChannel(TestContext& context)
{
	if (channel_ == nullptr)
	{
		return;
	}

	// The bool is documented as making a Steam connection close with an app
	// exception end reason, and is passed to the notify callbacks; there is no
	// connection here, so false.
	CS2ST_CALL(context, "INetworkSystem::RemoveNetChannel", {
		network_->RemoveNetChannel(channel_, false);
		context.Detail("-> removed %p", static_cast<void*>(channel_));
	});

	channel_ = nullptr;
	info_ = nullptr;
}

// -----------------------------------------------------------------------------
//  INetChannelInfo
// -----------------------------------------------------------------------------

void NetChannelSuite::ExerciseInfo(TestContext& context)
{
	context.Note("[INetChannelInfo] no destructor and no IAppSystem here: slot 0 is GetName");

	CS2ST_CALL(context, "INetChannelInfo::GetName", {
		const char* name = info_->GetName();
		if (name == nullptr)
		{
			context.Detail("-> (null)");
			return;
		}
		const std::size_t length = BoundedLength(name);
		if (length > kMaxPlausibleStringLength)
		{
			context.Detail("-> %zu characters", length);
			context.Fail("no terminator in %zu bytes; slot 0 is probably not GetName",
				kMaxPlausibleStringLength);
			return;
		}
		// The name is the one we passed to CreateNetChannel, so this is a
		// second path to the same string: a wrong slot or a shifted argument
		// list would hand back something else.
		const bool same = std::strcmp(name, kChannelName) == 0;
		context.Detail("-> %zu characters, %s the name given to CreateNetChannel",
			length, same ? "equals" : "differs from");
		if (!same)
		{
			context.Suspect("the channel does not report the name it was created with");
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::GetAddress", {
		const char* address = info_->GetAddress();
		if (address == nullptr)
		{
			context.Detail("-> (null)");
			return;
		}
		const std::size_t length = BoundedLength(address);
		context.Detail("-> %zu characters", length);
		if (length > kMaxPlausibleStringLength)
		{
			context.Fail("no terminator in %zu bytes", kMaxPlausibleStringLength);
		}
	});

	// The address is a netadr_t, the type NetworkServerSuite already checks
	// field by field against its tier0 accessors. A playback channel has no
	// peer, so "not valid" is the expected answer and is only recorded.
	CS2ST_CALL(context, "INetChannelInfo::GetRemoteAddress", {
		const netadr_t& remote = info_->GetRemoteAddress();
		const bool valid = remote.IsValid();
		context.Detail("-> %p, IsValid %s", static_cast<const void*>(&remote),
			valid ? "true" : "false");
	});

	float now = 0.0f;
	CS2ST_CALL(context, "INetChannelInfo::GetTime", {
		now = info_->GetTime();
		context.Detail("-> %f", static_cast<double>(now));
		if (!std::isfinite(now) || now < 0.0f)
		{
			context.Fail("a net time is a finite non-negative number");
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::GetTimeConnected", {
		const float connected = info_->GetTimeConnected();
		context.Detail("-> %f", static_cast<double>(connected));
		if (!std::isfinite(connected) || connected < 0.0f)
		{
			context.Fail("a connection time is a finite non-negative number");
		}
		else if (connected > now + 1.0f)
		{
			context.Suspect("connected for %f s, longer than the net time %f s",
				static_cast<double>(connected), static_cast<double>(now));
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::GetDataRate", {
		const int rate = info_->GetDataRate();
		context.Detail("-> %d bytes/s", rate);
		if (rate < 0)
		{
			context.Fail("a data rate cannot be negative");
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::IsLocalHost", {
		context.Detail("-> %s", info_->IsLocalHost() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetChannelInfo::IsLoopback", {
		context.Detail("-> %s", info_->IsLoopback() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetChannelInfo::IsTimingOut", {
		context.Detail("-> %s", info_->IsTimingOut() ? "true" : "false");
	});

	// bPlayback was true in CreateNetChannel: the same fact through a getter.
	CS2ST_CALL(context, "INetChannelInfo::IsPlayback", {
		const bool playback = info_->IsPlayback();
		context.Detail("-> %s (CreateNetChannel was given true)", playback ? "true" : "false");
		if (!playback)
		{
			context.Suspect("a channel created as playback does not report IsPlayback");
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::GetAvgLatency", {
		const float latency = info_->GetAvgLatency();
		context.Detail("-> %f s", static_cast<double>(latency));
		if (!std::isfinite(latency) || latency < 0.0f)
		{
			context.Fail("a latency is a finite non-negative number of seconds");
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::GetEngineLatency", {
		const float latency = info_->GetEngineLatency();
		context.Detail("-> %f s", static_cast<double>(latency));
		if (!std::isfinite(latency) || latency < 0.0f)
		{
			context.Fail("a latency is a finite non-negative number of seconds");
		}
	});

	ExerciseInfoFlow(context, FLOW_OUTGOING);
	ExerciseInfoFlow(context, FLOW_INCOMING);

	CS2ST_CALL(context, "INetChannelInfo::GetTimeSinceLastReceived", {
		const float since = info_->GetTimeSinceLastReceived();
		context.Detail("-> %f s", static_cast<double>(since));
		if (!std::isfinite(since) || since < 0.0f)
		{
			context.Fail("a time since the last packet is finite and non-negative");
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::GetRemoteFramerate", {
		float frame_time = -1.0f;
		float deviation = -1.0f;
		float unfiltered = -1.0f;
		info_->GetRemoteFramerate(&frame_time, &deviation, &unfiltered);
		context.Detail("-> frame %f, deviation %f, unfiltered %f",
			static_cast<double>(frame_time), static_cast<double>(deviation),
			static_cast<double>(unfiltered));
		if (!std::isfinite(frame_time) || !std::isfinite(deviation) || !std::isfinite(unfiltered))
		{
			context.Fail("a frame time is finite");
		}
	});

	float timeout = 0.0f;
	CS2ST_CALL(context, "INetChannelInfo::GetTimeoutSeconds", {
		timeout = info_->GetTimeoutSeconds();
		context.Detail("-> %f s", static_cast<double>(timeout));
		if (!std::isfinite(timeout) || timeout < 0.0f)
		{
			context.Fail("a timeout is a finite non-negative number of seconds");
		}
		else if (timeout == 0.0f)
		{
			context.Suspect("a zero timeout; the default for a new channel is not known");
		}
	});

	// Two views of one deadline: the time left cannot exceed the timeout.
	CS2ST_CALL(context, "INetChannelInfo::GetTimeUntilTimeout", {
		const float left = info_->GetTimeUntilTimeout();
		context.Detail("-> %f s (GetTimeoutSeconds %f)", static_cast<double>(left),
			static_cast<double>(timeout));
		if (!std::isfinite(left))
		{
			context.Fail("a time until timeout is finite");
		}
		else if (timeout > 0.0f && left > timeout + 1.0f)
		{
			context.Suspect("%f s left of a %f s timeout", static_cast<double>(left),
				static_cast<double>(timeout));
		}
	});

	CS2ST_CALL(context, "INetChannelInfo::GetLateFrameJitterThreshold", {
		const float threshold = info_->GetLateFrameJitterThreshold();
		context.Detail("-> %f ms", static_cast<double>(threshold));
		if (!std::isfinite(threshold))
		{
			context.Fail("a jitter threshold is finite");
		}
	});

	// The channel index is not documented; 0 is the first one, so the probe
	// reads the pointer and does not follow it (the type is opaque here).
	CS2ST_CALL(context, "INetChannelInfo::GetLatencyStats [channel 0]", {
		context.Detail("-> %p", static_cast<const void*>(info_->GetLatencyStats(0)));
	});
}

void NetChannelSuite::ExerciseInfoFlow(TestContext& context, int flow)
{
	char label[96];

	FlowLabel(label, "GetAvgLoss", flow);
	CS2ST_CALL(context, label, {
		const float loss = info_->GetAvgLoss(flow);
		context.Detail("-> %f", static_cast<double>(loss));
		if (!std::isfinite(loss) || loss < 0.0f || loss > 1.0f)
		{
			context.Fail("the header promises a loss in [0..1]");
		}
	});

	FlowLabel(label, "GetAvgChoke", flow);
	CS2ST_CALL(context, label, {
		const float choke = info_->GetAvgChoke(flow);
		context.Detail("-> %f", static_cast<double>(choke));
		if (!std::isfinite(choke) || choke < 0.0f)
		{
			context.Fail("a choke is a finite non-negative number");
		}
	});

	// Documented: milliseconds, or -1 when there are no frames.
	FlowLabel(label, "GetPeakJitter", flow);
	CS2ST_CALL(context, label, {
		const float jitter = info_->GetPeakJitter(flow);
		context.Detail("-> %f ms", static_cast<double>(jitter));
		if (!std::isfinite(jitter) || jitter < -1.0f)
		{
			context.Fail("the header says a jitter is >= 0, or -1 when there are no frames");
		}
	});

	FlowLabel(label, "GetAvgData", flow);
	CS2ST_CALL(context, label, {
		const float data = info_->GetAvgData(flow);
		context.Detail("-> %f bytes/s", static_cast<double>(data));
		if (!std::isfinite(data) || data < 0.0f)
		{
			context.Fail("a data rate is a finite non-negative number");
		}
	});

	FlowLabel(label, "GetAvgPackets", flow);
	CS2ST_CALL(context, label, {
		const float packets = info_->GetAvgPackets(flow);
		context.Detail("-> %f/s", static_cast<double>(packets));
		if (!std::isfinite(packets) || packets < 0.0f)
		{
			context.Fail("a packet rate is a finite non-negative number");
		}
	});

	FlowLabel(label, "GetTotalData", flow);
	CS2ST_CALL(context, label, {
		context.Detail("-> %llu bytes",
			static_cast<unsigned long long>(info_->GetTotalData(flow)));
	});

	FlowLabel(label, "GetTotalPackets", flow);
	CS2ST_CALL(context, label, {
		const int packets = info_->GetTotalPackets(flow);
		context.Detail("-> %d", packets);
		if (packets < 0)
		{
			context.Fail("a packet count cannot be negative");
		}
	});

	int sequence = 0;
	FlowLabel(label, "GetSequenceNr", flow);
	CS2ST_CALL(context, label, {
		sequence = info_->GetSequenceNr(flow);
		context.Detail("-> %d", sequence);
	});

	// The header gives the status values: 0 delivered, 1 dropped, 2 out of
	// order, 3 unknown. Asked about the flow's own latest sequence number.
	FlowLabel(label, "GetFrameStatus", flow);
	CS2ST_CALL(context, label, {
		float jitter = -2.0f;
		const int status = info_->GetFrameStatus(flow, sequence, &jitter);
		context.Detail("-> %d (seq %d), jitter %f", status, sequence,
			static_cast<double>(jitter));
		if (status < 0 || status > 3)
		{
			context.Fail("the header lists the frame statuses 0..3");
		}
	});

	// Written count must fit the buffer we gave; entries are in 0.1 ms units
	// or -1 / -2 / -3 for dropped / out of order / unknown.
	FlowLabel(label, "GetFrameJitterHistory", flow);
	CS2ST_CALL(context, label, {
		std::int16_t history[kJitterEntries] = {};
		const int count = info_->GetFrameJitterHistory(flow, history, kJitterEntries);
		context.Detail("-> %d entr%s", count, count == 1 ? "y" : "ies");
		if (count < 0 || count > kJitterEntries)
		{
			context.Fail("the header says it returns the count written, at most %d",
				kJitterEntries);
			return;
		}
		for (int i = 0; i < count; ++i)
		{
			if (history[i] < -3)
			{
				context.Suspect("entry %d is %d; the header lists -1 / -2 / -3 as markers",
					i, static_cast<int>(history[i]));
				break;
			}
		}
	});
}

// -----------------------------------------------------------------------------
//  INetChannel
// -----------------------------------------------------------------------------

void NetChannelSuite::ExerciseChannel(TestContext& context)
{
	// CreateNetChannel returns an INetChannel*, so there is no downcast here.
	// The destructor sits after the INetChannelInfo slots and may take two;
	// GetSteamNetConnection is the probe (the first slot after Reset / Clear /
	// Shutdown that is safe to call).
	INetChannel* channel = channel_;
	context.Note("[INetChannel] channel at %p", static_cast<void*>(channel));

	CS2ST_CALL(context, "INetChannel::GetSteamNetConnection", {
		const HSteamNetConnection connection = channel->GetSteamNetConnection();
		context.Detail("-> %u%s", static_cast<unsigned>(connection),
			connection == k_HSteamNetConnection_Invalid ? " (invalid; none was given)" : "");
	});

	CS2ST_CALL(context, "INetChannel::CanPacket", {
		context.Detail("-> %s", channel->CanPacket() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetChannel::IsOverflowed", {
		context.Detail("-> %s", channel->IsOverflowed() ? "true" : "false");
	});

	CS2ST_CALL(context, "INetChannel::HasPendingReliableData", {
		context.Detail("-> %s", channel->HasPendingReliableData() ? "true" : "false");
	});

	// IsTimedOut and IsTimingOut are different questions (past the deadline vs
	// approaching it), so a mismatch is a record and not a verdict - except
	// the one implication that has to hold.
	CS2ST_CALL(context, "INetChannel::IsTimedOut", {
		const bool timed_out = channel->IsTimedOut();
		const bool timing_out = channel->IsTimingOut();
		context.Detail("-> %s (IsTimingOut %s)", timed_out ? "true" : "false",
			timing_out ? "true" : "false");
		if (timed_out && !timing_out)
		{
			context.Suspect("timed out but not timing out");
		}
	});

	CS2ST_CALL(context, "INetChannel::IsRemoteDisconnected", {
		ENetworkDisconnectionReason reason = static_cast<ENetworkDisconnectionReason>(0);
		const bool disconnected = channel->IsRemoteDisconnected(reason);
		context.Detail("-> %s, reason %d", disconnected ? "true" : "false",
			static_cast<int>(reason));
		if (disconnected && !ENetworkDisconnectionReason_IsValid(static_cast<int>(reason)))
		{
			context.Fail("reason %d is not an ENetworkDisconnectionReason",
				static_cast<int>(reason));
		}
	});

	CS2ST_CALL(context, "INetChannel::GetNetMessageDispatcher", {
		context.Detail("-> %p", static_cast<void*>(channel->GetNetMessageDispatcher()));
	});

	CS2ST_CALL(context, "INetChannel::GetNumBitsWritten [BUF_UNRELIABLE]", {
		const int bits = channel->GetNumBitsWritten(BUF_UNRELIABLE);
		context.Detail("-> %d", bits);
		if (bits < 0)
		{
			context.Fail("a bit count cannot be negative");
		}
	});

	CS2ST_CALL(context, "INetChannel::GetNumBitsWritten [BUF_RELIABLE]", {
		const int bits = channel->GetNumBitsWritten(BUF_RELIABLE);
		context.Detail("-> %d", bits);
		if (bits < 0)
		{
			context.Fail("a bit count cannot be negative");
		}
	});

	CS2ST_CALL(context, "INetChannel::IsNull", {
		const bool is_null = channel->IsNull();
		context.Detail("-> %s", is_null ? "true" : "false");
	});

	CS2ST_CALL(context, "INetChannel::HasQueuedNetMessages [id 0]", {
		context.Detail("-> %s", channel->HasQueuedNetMessages(0) ? "true" : "false");
	});

	CS2ST_CALL(context, "INetChannel::GetPendingDisconnect", {
		const int reason = static_cast<int>(channel->GetPendingDisconnect());
		context.Detail("-> %d", reason);
		if (!ENetworkDisconnectionReason_IsValid(reason))
		{
			context.Fail("reason %d is not an ENetworkDisconnectionReason", reason);
		}
	});

	CS2ST_CALL(context, "INetChannel::IsSuppressingTransmit", {
		context.Detail("-> %s", channel->IsSuppressingTransmit() ? "true" : "false");
	});

	// The queue is returned as a pointer and not walked: it holds messages the
	// engine is about to dispatch.
	CS2ST_CALL(context, "INetChannel::GetReceivedMessageQueue", {
		context.Detail("-> %p", static_cast<void*>(channel->GetReceivedMessageQueue()));
	});
}

// -----------------------------------------------------------------------------
//  Setters, each read back through a different slot
// -----------------------------------------------------------------------------

/**
 * Everything here acts on the channel this sweep created, which nothing else
 * has seen and which is removed at the end, so no later suite can be affected.
 * The strongest rows are the ones that write through one slot and read the
 * value back through another: the engine has to have stored the argument, and
 * both slots have to be the ones the SDK says they are.
 *
 * Only what has to be true is judged, and as Suspect: whether a setter clamps
 * or ignores a value is the engine's business and not in the SDK.
 */
void NetChannelSuite::ExerciseWrites(TestContext& context)
{
	INetChannel* channel = channel_;
	context.Note("[INetChannel] setters on the channel we created, read back through "
	             "another slot");

	CS2ST_CALL(context, "INetChannel::SetTimeout [7.5 s, exact]", {
		channel->SetTimeout(7.5f, true);
		const float timeout = info_->GetTimeoutSeconds();
		const float left = info_->GetTimeUntilTimeout();
		context.Detail("-> GetTimeoutSeconds %f, GetTimeUntilTimeout %f",
			static_cast<double>(timeout), static_cast<double>(left));
		if (std::fabs(timeout - 7.5f) > 0.01f)
		{
			context.Suspect("asked for 7.5 s exactly and read back %f",
				static_cast<double>(timeout));
		}
		if (left > timeout + 0.5f)
		{
			context.Suspect("%f s left of a %f s timeout", static_cast<double>(left),
				static_cast<double>(timeout));
		}
	});

	CS2ST_CALL(context, "INetChannel::SetLateFrameJitterThreshold [12.5 ms]", {
		channel->SetLateFrameJitterThreshold(12.5f);
		const float threshold = info_->GetLateFrameJitterThreshold();
		context.Detail("-> GetLateFrameJitterThreshold %f", static_cast<double>(threshold));
		if (std::fabs(threshold - 12.5f) > 0.01f)
		{
			context.Suspect("set 12.5 and read back %f", static_cast<double>(threshold));
		}
	});

	CS2ST_CALL(context, "INetChannel::SuppressTransmit [true, then false]", {
		channel->SuppressTransmit(true);
		const bool on = channel->IsSuppressingTransmit();
		channel->SuppressTransmit(false);
		const bool off = channel->IsSuppressingTransmit();
		context.Detail("-> IsSuppressingTransmit %s after true, %s after false",
			on ? "true" : "false", off ? "true" : "false");
		if (!on || off)
		{
			context.Suspect("the flag did not follow SuppressTransmit");
		}
	});

	// The previous value is put back before the channel is removed, so the
	// removal path never sees a pending disconnect that this row invented.
	CS2ST_CALL(context, "INetChannel::SetPendingDisconnect [DISCONNECT_BY_USER, then restored]", {
		const ENetworkDisconnectionReason before = channel->GetPendingDisconnect();
		channel->SetPendingDisconnect(NETWORK_DISCONNECT_DISCONNECT_BY_USER);
		const ENetworkDisconnectionReason after = channel->GetPendingDisconnect();
		channel->SetPendingDisconnect(before);
		context.Detail("-> %d, then %d", static_cast<int>(after), static_cast<int>(before));
		if (after != NETWORK_DISCONNECT_DISCONNECT_BY_USER)
		{
			context.Suspect("set %d and read back %d",
				static_cast<int>(NETWORK_DISCONNECT_DISCONNECT_BY_USER), static_cast<int>(after));
		}
	});

	// Set to the value it already has: not a change, but it proves the slot is
	// a setter that accepts what the getter hands out.
	CS2ST_CALL(context, "INetChannel::SetNetMessageDispatcher [current value]", {
		INetMessageDispatcher* current = channel->GetNetMessageDispatcher();
		channel->SetNetMessageDispatcher(current);
		INetMessageDispatcher* after = channel->GetNetMessageDispatcher();
		context.Detail("-> %p, then %p", static_cast<void*>(current), static_cast<void*>(after));
		if (after != current)
		{
			context.Suspect("the dispatcher changed when set to itself");
		}
	});

	CS2ST_CALL(context, "INetChannel::SetMinDataRate / SetMaxDataRate [current rate]", {
		const int rate = info_->GetDataRate();
		channel->SetMinDataRate(rate);
		channel->SetMaxDataRate(rate);
		const int after = info_->GetDataRate();
		context.Detail("-> GetDataRate %d, then %d", rate, after);
		if (after != rate)
		{
			context.Suspect("the data rate changed when both limits were set to it");
		}
	});

	CS2ST_CALL(context, "INetChannel::UpdateLastReceivedTime", {
		channel->UpdateLastReceivedTime();
		const float since = info_->GetTimeSinceLastReceived();
		context.Detail("-> GetTimeSinceLastReceived %f s", static_cast<double>(since));
		if (since > 1.0f)
		{
			context.Suspect("a packet was just recorded and %f s have passed since",
				static_cast<double>(since));
		}
	});

	CS2ST_CALL(context, "INetChannel::SetCheckpoint / ClearCheckpoint", {
		channel->SetCheckpoint();
		channel->ClearCheckpoint();
		context.Detail("-> returned (void, void)");
	});

	CS2ST_CALL(context, "INetChannel::UpdateMessageStats [GENERIC, 8 bits]", {
		channel->UpdateMessageStats(INetChannelInfo::GENERIC, 8, false);
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannel::NotifyOutboundTick [tick 0]", {
		channel->NotifyOutboundTick(0);
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannel::SetDemoRecorder [nullptr]", {
		channel->SetDemoRecorder(nullptr);
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannel::SetInstantReplayIntercept [nullptr]", {
		channel->SetInstantReplayIntercept(nullptr);
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannelInfo::SetInterpolationAmount [0.1, 64]", {
		info_->SetInterpolationAmount(0.1f, 64.0f);
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannelInfo::SetNumPredictionErrors [0]", {
		info_->SetNumPredictionErrors(0);
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannelInfo::SetShowNetMessages [false]", {
		info_->SetShowNetMessages(false);
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannelInfo::ResetLatencyStats [channel 0]", {
		info_->ResetLatencyStats(0);
		context.Detail("-> returned (void)");
	});

	// Last, because they throw away state the rows above wrote. The channel is
	// removed straight after, so nothing reads that state again.
	CS2ST_CALL(context, "INetChannel::Reset", {
		channel->Reset();
		context.Detail("-> returned (void)");
	});

	CS2ST_CALL(context, "INetChannel::Clear", {
		channel->Clear();
		context.Detail("-> returned (void)");
	});
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void NetChannelSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[INetChannelInfo / INetChannel] slots not called");

	context.SkipCall("INetChannelInfo::SetLatencyStats",
		"takes an SNetChannelLatencyStats, which is only forward-declared here; "
		"there is no value of it we can build or copy");
	context.SkipCall("INetChannelInfo::GetNetworkFlowQuality",
		"fills a protobuf message through the engine's own protobuf runtime, "
		"which may not be the one this plugin links; a mismatch would crash in "
		"the engine, not in the SDK");

	context.SkipCall("INetChannel::~INetChannel",
		"destroys the channel; the destructor may take two vtable slots");
	context.SkipCall("INetChannel::Shutdown",
		"runs the shutdown notifications the game registered and may free the "
		"channel, which RemoveNetChannel then frees again");
	context.SkipCall("INetChannel::SendNetMessage",
		"sends through a Steam connection that does not exist on this channel");
	context.SkipCall("INetChannel::SendData",
		"sends through a Steam connection that does not exist on this channel");
	context.SkipCall("INetChannel::Transmit",
		"sends through a Steam connection that does not exist on this channel");
	context.SkipCall("INetChannel::TransmitUpToCheckpoint",
		"sends through a Steam connection that does not exist on this channel");
	context.SkipCall("INetChannel::SendMessageRaw",
		"sends through a Steam connection that does not exist on this channel");
	context.SkipCall("INetChannel::StoreEncodingStatsInFrame",
		"writes into the current incoming frame, and a channel that never received "
		"a packet has none; the index it would use is not known");
	context.SkipCall("INetChannel::SetActiveChannel",
		"reroutes the channel's messages to another channel; the only other "
		"channels are the game's");
	context.SkipCall("INetChannel::AttachSplitPlayer", "splitscreen: no second channel to attach");
	context.SkipCall("INetChannel::DetachSplitPlayer", "splitscreen: nothing attached");
	context.SkipCall("INetChannel::SetRemoteFramerate",
		"needs a CNetMessagePB<CNETMsg_Tick> the engine accepts; ours would be "
		"built by this plugin's protobuf runtime");
	context.SkipCall("INetChannel::StartRegisteringMessageHandlers",
		"opens a registration window that builds handler tables from the "
		"categories, which are guessed values here");
	context.SkipCall("INetChannel::FinishRegisteringMessageHandlers",
		"closes that window and reports missing handlers into the server console");
	context.SkipCall("INetChannel::RegisterNetMessageHandlerAbstract",
		"needs a delegate the engine can call; a bad one crashes on the next message");
	context.SkipCall("INetChannel::UnregisterNetMessageHandlerAbstract",
		"nothing was registered, and the delegate would have to match one");
	context.SkipCall("INetChannel::ProcessDemoPacket",
		"reads a NetPacket_t that only the engine can build");
	context.SkipCall("INetChannel::ProcessMessages",
		"dispatches queued messages through a dispatcher this channel does not "
		"have, so a fault here would be ours");
	context.SkipCall("INetChannel::InstallMessageFilter",
		"needs a filter object the engine can call");
	context.SkipCall("INetChannel::UninstallMessageFilter",
		"nothing was installed");
	context.SkipCall("INetChannel::PostReceivedNetMessage",
		"needs a CNetMessage the engine built; it would be dispatched as received");
	context.SkipCall("INetChannel::PostReceivedNetMessageToFront",
		"needs a CNetMessage the engine built; it would be dispatched as received");
	context.SkipCall("INetChannel::InsertReplayMessage",
		"needs an InstantReplayMessage_t the engine built");
	context.SkipCall("INetChannel::WriteFrameQualityToTick",
		"writes into a CNetMessagePB<CNETMsg_Tick> built by a different protobuf runtime");
	context.SkipCall("INetChannel::SetQueuedMessageLimitConVar",
		"the channel keeps the pointer and reads it later; the only convars are the engine's");
}

} // namespace cs2st
