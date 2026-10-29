#ifndef CS2SDKTEST_SUITES_KEYVALUES3SUITE_H_
#define CS2SDKTEST_SUITES_KEYVALUES3SUITE_H_

#include "testkit/ITestSuite.h"

class ISource2Server;

namespace cs2st
{

class KeyValues3Suite : public ITestSuite
{
public:
	const char* Id() const override { return "keyvalues3"; }
	const char* DisplayName() const override { return "KeyValues3"; }
	const char* Description() const override
	{
		return "KeyValues3 read back from a document the engine wrote "
		       "(needs CS2SDKTEST_TIER1_KEYVALUES3)";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseEngineFilled(TestContext& context);
	void ExerciseLocal(TestContext& context);

	ISource2Server* server_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_KEYVALUES3SUITE_H_ */
