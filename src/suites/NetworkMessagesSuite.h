#ifndef CS2SDKTEST_SUITES_NETWORKMESSAGESSUITE_H_
#define CS2SDKTEST_SUITES_NETWORKMESSAGESSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers here: this suite installs no hooks, so nothing needs an SDK
// type in its own declaration. The interface pointer is stored as the
// forward-declared class below; every other SDK type lives in the .cpp.
class INetworkMessages;
class INetworkMessageInternal;
class CNetMessage;

namespace cs2st
{

class NetworkMessagesSuite : public ITestSuite
{
public:
	const char* Id() const override { return "netmessages"; }
	const char* DisplayName() const override
	{
		return "INetworkMessages / protobuf bindings";
	}
	const char* Description() const override
	{
		return "The registry every net message lives in, and whether it still "
		       "agrees with the protobufs the SDK ships";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	// ---- sections -----------------------------------------------------------
	//
	// One section per group of facts, ordered so each builds on the last:
	// resolve first (every later section needs the registry entries), then
	// the registry's own lookup surfaces, then the binding each entry points
	// at, then allocation and the serialization round trip.

	void ResolveMessages(TestContext& context);
	void ExerciseRegistryLookups(TestContext& context);
	void ExerciseBindingMatrix(TestContext& context);
	void ExerciseAllocation(TestContext& context);
	void ExerciseRoundTrip(TestContext& context);
	void ExerciseMiscReadonly(TestContext& context);
	void InspectLoggingChannel(TestContext& context);
	void NoteSkippedWrites(TestContext& context);

	/** The round trip's one exit-shared free path; see the .cpp for why. */
	void message_free_section(TestContext& context, CNetMessage* message,
	                          CNetMessage* typed_target, CNetMessage* from_stream,
	                          CNetMessage* forwarded_target);

	INetworkMessages* messages_ = nullptr;

	/**
	 * The registry entry for CNETMsg_Tick, resolved fresh each sweep. The
	 * round trip and the allocation oracles all run through it: it has
	 * scalar-only protobuf fields, so a value can be planted, serialised and
	 * read back without any string allocation crossing the module boundary.
	 *
	 * These are the engine's per-message-type registry entries and outlive
	 * the sweep, but each sweep re-resolves rather than caching across runs -
	 * a pointer held from a previous run says nothing about this one.
	 */
	INetworkMessageInternal* tick_message_ = nullptr;

	/**
	 * Whether AsProto() - the engine's own pointer to the protobuf
	 * sub-object - agrees with this build's ToPB<T>() computed offset on the
	 * allocated message. CNetMessagePB in the SDK is a stub over the engine's
	 * real class, so this is the ABI assumption every write through the stub
	 * depends on; the round trip refuses to write until it holds.
	 */
	bool proto_offsets_agree_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_NETWORKMESSAGESSUITE_H_ */
