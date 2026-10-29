#ifndef CS2SDKTEST_SUITES_SERVERCONFIGSUITE_H_
#define CS2SDKTEST_SUITES_SERVERCONFIGSUITE_H_

#include "testkit/ITestSuite.h"

class ISource2ServerConfig;

namespace cs2st
{

class ServerConfigSuite : public ITestSuite
{
public:
	const char* Id() const override { return "serverconfig"; }
	const char* DisplayName() const override { return "ISource2ServerConfig"; }
	const char* Description() const override
	{
		return "The startup-questions interface the engine asks the game DLL: "
		       "player limits, dedicated-server permission, the tick interval";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseConfigQueries(TestContext& context);
	void ExerciseTailSlots(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ISource2ServerConfig* config_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SERVERCONFIGSUITE_H_ */
