/**
 * Exercises INetworkMessages - the registry every net message lives in.
 *
 * "NetworkMessagesVersion001" @ networksystem. This is the interface
 * GameEventSuite borrowed as a tool; here it is the subject, and that change
 * of role is visible in what gets asked: there the question was "what is the
 * descriptor behind one event", here it is "does the registry itself still
 * agree with the protobufs the SDK ships".
 *
 * The first thing this suite reports is that INetworkMessages is not an
 * IAppSystem. Every other Version001 interface this project touches starts
 * with the eleven appsystem slots; this one's vtable begins at
 * RegisterNetworkCategory. So there is no ExerciseAppSystem section and no
 * GetTier liveness check - which is worth a line in the log, because a future
 * update that adds appsystem inheritance here renumbers every slot below it.
 *
 * The substance is a set of paths that must agree. A message is reachable
 * four ways - by name (FindNetworkMessage), by partial name
 * (FindNetworkMessagePartial), by id (FindNetworkMessageById), and by
 * id-plus-descriptor (FindOrCreateNetMessage with bCreateIfNotFound false,
 * which turns the creating method into a fourth read-only lookup). All four
 * hand back the same INetworkMessageInternal*, and the descriptor it carries
 * answers the same id, group and binding however it is read. The protobuf
 * side adds the deepest check this suite can make: an allocated CNETMsg_Tick
 * with a known field value is serialised through SerializeMessageInternal and
 * read back through UnserializeFromStream, and both the value and the message
 * type have to survive. A protobuf descriptor change - the thing
 * docs/TEST_TARGETS.md #8 exists for - breaks that round trip before it
 * breaks anything a plugin would notice.
 *
 * The round trip needs bf_write/bf_read, whose constructors are not exported
 * by any prebuilt library (tier0 exports exactly two bitbuf symbols,
 * netadr_t::Serialize and netadr_t::Unserialize). CS2SDKTEST_TIER1_BITBUF
 * compiles tier1/bitbuf.cpp in - this suite is why that option now defaults
 * to ON - and a build with it off reports every serialization method as SKIP
 * with that reason rather than pretending.
 *
 * No hooks. The mutating methods (RegisterNetworkCategory and friends) run
 * while the engine builds the registry, which on a server this plugin loads
 * late into has already happened; the lookup methods run per message sent,
 * which is a hook firing thousands of times a minute to re-report rows the
 * call sweep already prints. Every method is accounted for either way - a
 * read-only call, a no-op-write pair, or a SkipCall naming what it would
 * mutate.
 */

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
