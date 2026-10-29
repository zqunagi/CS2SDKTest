#ifndef CS2SDKTEST_SUITES_GAMEENTITIESSUITE_H_
#define CS2SDKTEST_SUITES_GAMEENTITIESSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks.
class ISource2GameEntities;

namespace cs2st
{

class GameEntitiesSuite : public ITestSuite
{
public:
	const char* Id() const override { return "gameentities"; }
	const char* DisplayName() const override { return "ISource2GameEntities"; }
	const char* Description() const override
	{
		return "The game DLL's entity-transmission interface: IAppSystem slots, "
		       "the read-only baseline-name query and the argument-free "
		       "transmit / pack callbacks";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseEntityQueries(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ISource2GameEntities* entities_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_GAMEENTITIESSUITE_H_ */
