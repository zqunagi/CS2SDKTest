#ifndef CS2SDKTEST_SUITES_RESOURCESYSTEMSUITE_H_
#define CS2SDKTEST_SUITES_RESOURCESYSTEMSUITE_H_

#include "testkit/ITestSuite.h"

class IResourceSystem;
class IGameResourceService;
struct ResourceBindingBase_t;

namespace cs2st
{

class ResourceSystemSuite : public ITestSuite
{
public:
	const char* Id() const override { return "resourcesystem"; }
	const char* DisplayName() const override { return "IResourceSystem / IGameResourceService"; }
	const char* Description() const override
	{
		return "The resource system (types, handles, names, status, lock, listeners) and the "
		       "game resource service (empty manifest round trip, CGameResourceService layout)";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	// IResourceSystem
	void ExerciseTypes(TestContext& context);
	void ExerciseEnumeration(TestContext& context);
	void ExerciseHandles(TestContext& context);
	void ExerciseState(TestContext& context);
	void ExerciseResourceSkips(TestContext& context);
	void InspectBinding(TestContext& context);

	// IGameResourceService
	void ExerciseServiceBase(TestContext& context);
	void ExerciseManifestRoundTrip(TestContext& context);
	void ExerciseServiceSkips(TestContext& context);
	void InspectService(TestContext& context);

	IResourceSystem* resource_system_ = nullptr;
	IGameResourceService* game_service_ = nullptr;

	// A few named resources picked out of GetAllResources, for the per-handle oracles.
	static constexpr int kMaxSamples = 4;
	const ResourceBindingBase_t* samples_[kMaxSamples] = {};
	int sample_count_ = 0;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_RESOURCESYSTEMSUITE_H_ */
