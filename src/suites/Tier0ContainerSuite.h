#ifndef CS2SDKTEST_SUITES_TIER0CONTAINERSUITE_H_
#define CS2SDKTEST_SUITES_TIER0CONTAINERSUITE_H_

#include "testkit/ITestSuite.h"

class ISchemaSystem;

namespace cs2st
{

class Tier0ContainerSuite : public ITestSuite
{
public:
	const char* Id() const override { return "tier0containers"; }
	const char* DisplayName() const override { return "tier0 containers (CUtlVector / CUtlString / CUtlBuffer)"; }
	const char* Description() const override
	{
		return "Container layouts checked against objects the engine built or owns";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseUtlString(TestContext& context);
	void ExerciseUtlBuffer(TestContext& context);
	void ExerciseBufferString(TestContext& context);
	void ExerciseUtlVector(TestContext& context);
	void NoteUncoveredContainers(TestContext& context);

	// Null when schemasystem is not loaded; only the engine-owned vector row
	// needs it, the rest of the suite runs regardless.
	ISchemaSystem* schema_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_TIER0CONTAINERSUITE_H_ */
