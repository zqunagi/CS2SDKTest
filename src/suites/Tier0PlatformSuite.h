#ifndef CS2SDKTEST_SUITES_TIER0PLATFORMSUITE_H_
#define CS2SDKTEST_SUITES_TIER0PLATFORMSUITE_H_

#include "testkit/ITestSuite.h"

namespace cs2st
{

/**
 * tier0's monotonic tick clock: Plat_RelativeTicks and Plat_TicksAfterMicroSecs,
 * and CLimitTimer, the inline wrapper that turns the pair into a deadline.
 *
 * These are free functions exported by tier0, so there is no interface to
 * acquire; Acquire only confirms the module is loaded. The SDK states one
 * property outright - the ticks never decrease - and every other row compares
 * one clock against another (the tick clock against Plat_USTime, a deadline
 * against the clock that set it) rather than against a number written down here.
 */
class Tier0PlatformSuite : public ITestSuite
{
public:
	const char* Id() const override { return "tier0platform"; }
	const char* DisplayName() const override { return "tier0 platform clock (Plat_RelativeTicks / CLimitTimer)"; }
	const char* Description() const override
	{
		return "The monotonic tick clock, the deadline helper built on it, and "
		       "whether both agree with Plat_USTime";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseTicks(TestContext& context);
	void ExerciseDeadlines(TestContext& context);
	void ExerciseLimitTimer(TestContext& context);
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_TIER0PLATFORMSUITE_H_ */
