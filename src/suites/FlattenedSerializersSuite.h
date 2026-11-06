#ifndef CS2SDKTEST_SUITES_FLATTENEDSERIALIZERSSUITE_H_
#define CS2SDKTEST_SUITES_FLATTENEDSERIALIZERSSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks.
class IFlattenedSerializers;
class ISerializedEntities;
class CSerializedEntity;
class INetworkServerService;

namespace cs2st
{

class FlattenedSerializersSuite : public ITestSuite
{
public:
	const char* Id() const override { return "flattenedserializers"; }
	const char* DisplayName() const override { return "IFlattenedSerializers"; }
	const char* Description() const override
	{
		return "IFlattenedSerializers driven by the serializer of an engine "
		       "instance baseline: names, validation, deltas, dumps";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseSerializer(TestContext& context, const CSerializedEntity* entity);
	void ExerciseEntityWork(TestContext& context, const CSerializedEntity* entity);
	void NoteSkippedCalls(TestContext& context);

	IFlattenedSerializers* serializers_ = nullptr;
	ISerializedEntities* entities_ = nullptr;
	INetworkServerService* service_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_FLATTENEDSERIALIZERSSUITE_H_ */
