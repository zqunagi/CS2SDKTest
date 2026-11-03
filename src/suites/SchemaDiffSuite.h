#ifndef CS2SDKTEST_SUITES_SCHEMADIFFSUITE_H_
#define CS2SDKTEST_SUITES_SCHEMADIFFSUITE_H_

#include "testkit/ITestSuite.h"

class CSchemaSystem;

namespace cs2st
{

/**
 * Dumps the running game's schema and compares the part of it that the SDK
 * also declares by hand.
 *
 * Two oracles that never saw each other: the engine's own reflection data
 * (what Source2SchemaDumper reads), and the compiler's view of the SDK headers
 * (sizeof / offsetof / enumerator values). A disagreement means the SDK is
 * stale or wrong for this game build - which is the finding, not a test bug,
 * so a mismatch is reported as is and the suite is never bent to hide it.
 *
 * The full dump goes to schema-dump.json next to results.json. It is not part
 * of the published results; only the per-class verdicts are.
 */
class SchemaDiffSuite : public ITestSuite
{
public:
	const char* Id() const override { return "schemadiff"; }
	const char* DisplayName() const override { return "Schema dump vs SDK declarations"; }
	const char* Description() const override
	{
		return "Reads every declared class and enum from the schema system, writes the "
		       "full dump to schema-dump.json, and diffs the classes and enums the SDK "
		       "also declares (size, field offsets, enumerator values)";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	CSchemaSystem* schema_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SCHEMADIFFSUITE_H_ */
