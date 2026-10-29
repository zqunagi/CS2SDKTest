#ifndef CS2SDKTEST_SUITES_HOSTSTATEMGRSUITE_H_
#define CS2SDKTEST_SUITES_HOSTSTATEMGRSUITE_H_

#include "testkit/ITestSuite.h"

class IHostStateMgr;

namespace cs2st
{

class HostStateMgrSuite : public ITestSuite
{
public:
	const char* Id() const override { return "hoststatemgr"; }
	const char* DisplayName() const override { return "IHostStateMgr"; }
	const char* Description() const override
	{
		return "The host state machine: IAppSystem slots and the CHostStateMgr / "
		       "CHostStateRequest layout (every Request method changes state)";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseRequests(TestContext& context);
	void InspectManager(TestContext& context);

	IHostStateMgr* manager_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_HOSTSTATEMGRSUITE_H_ */
