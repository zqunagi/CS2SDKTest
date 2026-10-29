#ifndef CS2SDKTEST_SUITES_LOCALIZESUITE_H_
#define CS2SDKTEST_SUITES_LOCALIZESUITE_H_

#include "testkit/ITestSuite.h"

class ILocalize;

namespace cs2st
{

class LocalizeSuite : public ITestSuite
{
public:
	const char* Id() const override { return "localize"; }
	const char* DisplayName() const override { return "ILocalize"; }
	const char* Description() const override
	{
		return "The localization table: token lookup by name and by index, "
		       "language state, value / date / number formatting";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseLanguage(TestContext& context);
	void ExerciseLookup(TestContext& context);
	void ExerciseTokenSamples(TestContext& context);
	void ExerciseFormatting(TestContext& context);
	void ExerciseFiles(TestContext& context);
	void ExerciseGuardedWrites(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ILocalize* localize_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_LOCALIZESUITE_H_ */
