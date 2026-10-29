#ifndef CS2SDKTEST_SUITES_NETCHANNELSUITE_H_
#define CS2SDKTEST_SUITES_NETCHANNELSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks.
class INetChannelInfo;
class INetChannel;
class INetworkSystem;

namespace cs2st
{

class NetChannelSuite : public ITestSuite
{
public:
	const char* Id() const override { return "netchannel"; }
	const char* DisplayName() const override { return "INetChannelInfo / INetChannel"; }
	const char* Description() const override
	{
		return "A net channel the engine builds for the test through "
		       "INetworkSystem::CreateNetChannel (no client needed): the "
		       "read-only statistics and state queries, with the mutating "
		       "slots listed as skipped";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	bool CreateChannel(TestContext& context);
	void DestroyChannel(TestContext& context);
	void ExerciseInfo(TestContext& context);
	void ExerciseInfoFlow(TestContext& context, int flow);
	void ExerciseChannel(TestContext& context);
	void ExerciseWrites(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	INetworkSystem* network_ = nullptr;
	// The channel this sweep created, and the same object seen as its base.
	// Created and removed inside one ExerciseCalls: nothing keeps it between
	// sweeps, so a stale pointer cannot be called.
	INetChannel* channel_ = nullptr;
	INetChannelInfo* info_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_NETCHANNELSUITE_H_ */
