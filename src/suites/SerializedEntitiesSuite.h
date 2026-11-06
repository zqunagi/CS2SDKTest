#ifndef CS2SDKTEST_SUITES_SERIALIZEDENTITIESSUITE_H_
#define CS2SDKTEST_SUITES_SERIALIZEDENTITIESSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks.
class ISerializedEntities;
class CSerializedEntity;
class CFieldPathPacker;
class INetworkServerService;

namespace cs2st
{
class SerializedEntitiesSuite : public ITestSuite
{
public:
	const char* Id() const override { return "serializedentities"; }
	const char* DisplayName() const override { return "ISerializedEntities"; }
	const char* Description() const override
	{
		return "ISerializedEntities, CFieldPathPacker and CSerializedEntity, "
		       "driven from the engine's instance baselines";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExercisePacker(TestContext& context);
	void ExerciseCounts(TestContext& context);
	void InspectEntity(TestContext& context, const CSerializedEntity* entity);
	void ExerciseOnEntity(TestContext& context, const CSerializedEntity* entity);
	void NoteSkippedCalls(TestContext& context);

	ISerializedEntities* entities_ = nullptr;
	INetworkServerService* service_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SERIALIZEDENTITIESSUITE_H_ */
