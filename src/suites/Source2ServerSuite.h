#ifndef CS2SDKTEST_SUITES_SOURCE2SERVERSUITE_H_
#define CS2SDKTEST_SUITES_SOURCE2SERVERSUITE_H_

#include "testkit/ITestSuite.h"

class ISource2Server;
class KeyValues;

namespace cs2st
{

class Source2ServerSuite : public ITestSuite
{
public:
	const char* Id() const override { return "server"; }
	const char* DisplayName() const override { return "ISource2Server (IServerGameDLL)"; }
	const char* Description() const override
	{
		return "The game DLL interface the engine drives";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseServerState(TestContext& context);
	void ExerciseEnumerations(TestContext& context);
	void ExerciseLookups(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ISource2Server* server_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SOURCE2SERVERSUITE_H_ */
