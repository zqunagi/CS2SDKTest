/**
 * Exercises the game event machinery: the engine's event bus and the legacy
 * event manager.
 *
 * Two interfaces, one subsystem, and they are not equally reachable - which is
 * itself the first thing this suite reports.
 *
 *   IGameEventSystem     "GameEventSystemServerV001" @ engine2. An IAppSystem
 *                        like every other, resolved with one CreateInterface
 *                        call, and the route every net message a plugin sends
 *                        actually travels.
 *
 *   IGameEventManager2   The Source 1 interface plugins still write against
 *                        (FireEvent, CreateEvent, LookupEventId). s2sdk's cs2
 *                        branch declares the class but defines *no* interface
 *                        version constant for it - the s1 sample uses
 *                        INTERFACEVERSION_GAMEEVENTSMANAGER2, while the s2
 *                        sample declares a FireEvent hook and then never
 *                        acquires anything to hang it on. So this suite asks
 *                        every loaded module for every version string the name
 *                        has ever had, and reports what came back. Today that
 *                        is "nothing", and "nothing" is the finding: the CS2
 *                        ecosystem reaches this object through a gamedata
 *                        vtable offset on ISource2Server instead, which is a
 *                        dependency this project deliberately does not have.
 *
 * The shape of the IGameEventSystem half is unusual. Of its ten methods, nine
 * either send an event or mutate the engine's registry - there is exactly one
 * read-only call on the whole interface (GetEventSource). So the calls are thin
 * and the *hooks* are the substance: nine methods hooked pre/post, watching the
 * engine push events through an interface whose argument list has grown twice.
 *
 * Two things make the hook output more than a tick count:
 *
 *   - Each hook that carries an INetworkMessageInternal* remembers the distinct
 *     pointers it has seen (no allocation, fixed array, capped). A later call
 *     sweep resolves those to names *under the fault guard*, which is where
 *     walking an engine object belongs - not in the engine's own frame.
 *   - NetChannelBufType_t is an int8 enum with four legal values, and
 *     nClientCount is documented as -1 or a real count. Both are checked in the
 *     handler, so a shifted argument list shows up as a flagged sample rather
 *     than as a hook that keeps counting happily.
 *
 * INetworkMessages ("NetworkMessagesVersion001" @ networksystem) is used here
 * as a *tool*, the way Source2GameClientsSuite uses IVEngineServer2 for
 * maxClients: it is the only read-only way to get hold of an
 * INetworkMessageInternal for a named message, and that is what unlocks
 * NetMessageInfo_t - the descriptor struct behind every game event - plus three
 * oracles between that struct and the IProtobufBinding it points at. Its own
 * coverage is a separate target and is not attempted here.
 */

#ifndef CS2SDKTEST_SUITES_GAMEEVENTSUITE_H_
#define CS2SDKTEST_SUITES_GAMEEVENTSUITE_H_

#include <cstddef>
#include <string>

// The hook handlers below have to spell out IGameEventSystem's exact parameter
// types - CSplitScreenSlot, INetworkMessageInternal*, CNetMessage*,
// NetChannelBufType_t, CBaseHandle, IRecipientFilter*, CUtlSlot* and
// CUtlAbstractDelegate - and a paraphrase would compile and then hand every
// handler garbage. This header is where the SDK assembles that set.
#include <engine2/igameeventsystem.h>
#include <khook.hpp>

#include "testkit/ITestSuite.h"

class IGameEventManager2;

namespace cs2st
{

class GameEventSuite : public ITestSuite
{
public:
	const char* Id() const override { return "event"; }
	const char* DisplayName() const override { return "IGameEventSystem / IGameEventManager2"; }
	const char* Description() const override
	{
		return "The engine's event bus, and the legacy manager CS2 stopped exporting";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

	int InstallHooks(TestContext& context) override;
	void RemoveHooks(TestContext& context) override;
	bool HooksInstalled() const override { return hooks_installed_; }

	// ---- hook handlers -----------------------------------------------------
	//
	// KHook::Virtual binds these by member pointer in the constructor, so they
	// can stay private. Every one answers Action::Ignore - this suite watches
	// the event bus, it does not steer it.
	//
	// Signatures are copied from public/engine/igameeventsystem.h verbatim,
	// `unsigned long` included. KHook resolves the vtable slot and calling
	// convention from the SDK member pointer, so a "tidier" uint64 here would
	// not fail to compile - it would silently misread every argument after it,
	// which is the exact drift this suite exists to catch.

	KHook::Return<void> Pre_RegisterGameEvent(IGameEventSystem*, INetworkMessageInternal* pEvent);
	KHook::Return<void> Post_RegisterGameEvent(IGameEventSystem*, INetworkMessageInternal* pEvent);

	KHook::Return<void> Pre_RegisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot* nSlot,
	                                                         const CUtlAbstractDelegate& delegate,
	                                                         INetworkMessageInternal* pEvent, int nPriority);
	KHook::Return<void> Post_RegisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot* nSlot,
	                                                          const CUtlAbstractDelegate& delegate,
	                                                          INetworkMessageInternal* pEvent, int nPriority);

	KHook::Return<void> Pre_UnregisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot* nSlot,
	                                                           const CUtlAbstractDelegate& delegate,
	                                                           INetworkMessageInternal* pEvent);
	KHook::Return<void> Post_UnregisterGameEventHandlerAbstract(IGameEventSystem*, CUtlSlot* nSlot,
	                                                            const CUtlAbstractDelegate& delegate,
	                                                            INetworkMessageInternal* pEvent);

	KHook::Return<void> Pre_PostEventAbstract_Local(IGameEventSystem*, CSplitScreenSlot nSlot, INetworkMessageInternal* pEvent,
	                                                const CNetMessage* pData, unsigned long nSize);
	KHook::Return<void> Post_PostEventAbstract_Local(IGameEventSystem*, CSplitScreenSlot nSlot, INetworkMessageInternal* pEvent,
	                                                 const CNetMessage* pData, unsigned long nSize);

	// The two PostEventAbstract overloads are distinct vtable slots, so they get
	// distinct handlers and distinct KHook::Virtual members. KHook::Virtual
	// takes the member pointer itself, which is overloaded - the constructor
	// disambiguates it with a static_cast per overload, and from there each
	// member tracks its own slot.
	KHook::Return<void> Pre_PostEventAbstract(IGameEventSystem*, CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount,
	                                          const uint64* clients, INetworkMessageInternal* pEvent,
	                                          const CNetMessage* pData, unsigned long nSize,
	                                          NetChannelBufType_t bufType);
	KHook::Return<void> Post_PostEventAbstract(IGameEventSystem*, CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount,
	                                           const uint64* clients, INetworkMessageInternal* pEvent,
	                                           const CNetMessage* pData, unsigned long nSize,
	                                           NetChannelBufType_t bufType);

	KHook::Return<void> Pre_PostEventAbstractFiltered(IGameEventSystem*, CSplitScreenSlot nSlot, bool bLocalOnly,
	                                                  IRecipientFilter* pFilter,
	                                                  INetworkMessageInternal* pEvent,
	                                                  const CNetMessage* pData, unsigned long nSize);
	KHook::Return<void> Post_PostEventAbstractFiltered(IGameEventSystem*, CSplitScreenSlot nSlot, bool bLocalOnly,
	                                                   IRecipientFilter* pFilter,
	                                                   INetworkMessageInternal* pEvent,
	                                                   const CNetMessage* pData, unsigned long nSize);

	KHook::Return<void> Pre_PostEntityEventAbstract(IGameEventSystem*, const CBaseHandle& hndl, INetworkMessageInternal* pEvent,
	                                                const CNetMessage* pData, unsigned long nSize,
	                                                NetChannelBufType_t bufType, bool bAllClients);
	KHook::Return<void> Post_PostEntityEventAbstract(IGameEventSystem*, const CBaseHandle& hndl, INetworkMessageInternal* pEvent,
	                                                 const CNetMessage* pData, unsigned long nSize,
	                                                 NetChannelBufType_t bufType, bool bAllClients);

	KHook::Return<void> Pre_ProcessQueuedEvents(IGameEventSystem*);
	KHook::Return<void> Post_ProcessQueuedEvents(IGameEventSystem*);

	KHook::Return<void> Pre_PurgeQueuedEvents(IGameEventSystem*);
	KHook::Return<void> Post_PurgeQueuedEvents(IGameEventSystem*);

	// ---- the hooks themselves ------------------------------------------------
	//
	// One KHook::Virtual per vtable slot, bound in the constructor below;
	// template parameters are the interface, the return type, then the SDK's
	// argument list. PostEventAbstract appears twice because it *is* two slots.
	KHook::Virtual<IGameEventSystem, void, INetworkMessageInternal*> hook_RegisterGameEvent_;
	KHook::Virtual<IGameEventSystem, void, CUtlSlot*, const CUtlAbstractDelegate&, INetworkMessageInternal*, int> hook_RegisterGameEventHandlerAbstract_;
	KHook::Virtual<IGameEventSystem, void, CUtlSlot*, const CUtlAbstractDelegate&, INetworkMessageInternal*> hook_UnregisterGameEventHandlerAbstract_;
	KHook::Virtual<IGameEventSystem, void, CSplitScreenSlot, INetworkMessageInternal*, const CNetMessage*, unsigned long> hook_PostEventAbstract_Local_;
	KHook::Virtual<IGameEventSystem, void, CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*,
	               const CNetMessage*, unsigned long, NetChannelBufType_t> hook_PostEventAbstract_;
	KHook::Virtual<IGameEventSystem, void, CSplitScreenSlot, bool, IRecipientFilter*, INetworkMessageInternal*,
	               const CNetMessage*, unsigned long> hook_PostEventAbstractFiltered_;
	KHook::Virtual<IGameEventSystem, void, const CBaseHandle&, INetworkMessageInternal*, const CNetMessage*, unsigned long,
	               NetChannelBufType_t, bool> hook_PostEntityEventAbstract_;
	KHook::Virtual<IGameEventSystem, void> hook_ProcessQueuedEvents_;
	KHook::Virtual<IGameEventSystem, void> hook_PurgeQueuedEvents_;

	GameEventSuite();

private:
	/**
	 * How many distinct INetworkMessageInternal pointers the hooks remember.
	 *
	 * Capped and fixed-size on purpose: this is filled from inside the engine's
	 * frame, where an allocation per event would be a real cost and a container
	 * that can reallocate would be a real hazard.
	 */
	static constexpr std::size_t kMaxObservedMessages = 24;

	/** Cheap, allocation-free, called from hooks. Duplicates are dropped. */
	void NoteObservedMessage(INetworkMessageInternal* message);

	void ExerciseAppSystem(TestContext& context);
	void ExerciseEventSystem(TestContext& context);
	void ResolveEventMessages(TestContext& context);
	void InspectNetMessageInfo(TestContext& context);
	void ReportObservedMessages(TestContext& context);
	void ReportManagerProbe(TestContext& context);
	void ExerciseGameEventManager(TestContext& context);
	void AccountForGameEvent(TestContext& context);

	IGameEventSystem* event_system_ = nullptr;

	/**
	 * Null on every build seen so far; see the file comment. Kept as a real
	 * member rather than a constant so that the day a CS2 update starts
	 * answering one of the probed version strings, the manager section runs
	 * instead of reporting N/A.
	 */
	IGameEventManager2* manager_ = nullptr;

	/** Which version string answered, and from which module. Null/empty if none. */
	const char* manager_version_ = nullptr;
	std::string manager_module_;

	/**
	 * The registry entry for CMsgSource1LegacyGameEvent, resolved through
	 * INetworkMessages. This is what every IGameEventManager2::FireEvent ends
	 * up travelling inside, and the only handle this suite has on the
	 * descriptor struct behind a game event.
	 *
	 * These objects are the engine's per-message-type registry entries, not
	 * per-event temporaries, so they outlive the call that produced them. They
	 * are still re-vetted before use.
	 */
	INetworkMessageInternal* legacy_event_message_ = nullptr;

	INetworkMessageInternal* observed_messages_[kMaxObservedMessages] = {};
	std::size_t observed_message_count_ = 0;
	std::size_t observed_message_overflow_ = 0;

	bool hooks_installed_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_GAMEEVENTSUITE_H_ */
