#ifndef CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_
#define CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_

#include "testkit/ITestSuite.h"

#include <engine2/inetworkserverservice.h>

class CNetworkGameServerBase;
class CGlobalVars;

namespace cs2st
{

class NetworkServerSuite : public ITestSuite
{
public:
	const char* Id() const override { return "netserver"; }
	const char* DisplayName() const override
	{
		return "INetworkServerService / INetworkGameServer";
	}
	const char* Description() const override
	{
		return "The server object plugins mean by \"the server\", cross-checked "
		       "against the engine interface that reports the same facts";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseEngineService(TestContext& context);
	void ExerciseServerService(TestContext& context);
	void ResolveGameServer(TestContext& context);
	void ExerciseGameServer(TestContext& context);
	void ExerciseGameServerBase(TestContext& context);
	void InspectNetworkAddress(TestContext& context);
	void NoteInheritanceShape(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	INetworkServerService* service_ = nullptr;

	/**
	 * Resolved inside ExerciseCalls, never in Acquire: it takes an engine call,
	 * which belongs where the fault guard is armed. Null whenever no level is
	 * loaded, which is a normal state and not a failure.
	 */
	CNetworkGameServerBase* game_server_ = nullptr;

	/**
	 * The game server pointer the previous sweep saw. Comparing the two is how
	 * "the engine replaces this object on level change" stops being a claim in
	 * a comment and becomes a line in the log.
	 */
	CNetworkGameServerBase* previous_game_server_ = nullptr;
	bool have_previous_game_server_ = false;

	/** From IVEngineServer2, for the cross-checks. Null if the engine moved. */
	CGlobalVars* globals_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_ */
