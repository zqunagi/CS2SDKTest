#ifndef CS2SDKTEST_SUITES_STRINGTABLESUITE_H_
#define CS2SDKTEST_SUITES_STRINGTABLESUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks.
class INetworkStringTableContainer;
class INetworkStringTable;

namespace cs2st
{

class StringTableSuite : public ITestSuite
{
public:
	const char* Id() const override { return "stringtable"; }
	const char* DisplayName() const override { return "INetworkStringTable"; }
	const char* Description() const override
	{
		return "The network string tables: container lookups, per-table "
		       "strings and user data, name / id / index round trips";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseContainer(TestContext& context);
	void ExerciseTable(TestContext& context, INetworkStringTable* table, int index);
	void InspectUserData(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	INetworkStringTableContainer* container_ = nullptr;
	// First user data the engine returned, read back by InspectUserData.
	const void* user_data_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_STRINGTABLESUITE_H_ */
