#ifndef CS2SDKTEST_SUITES_GAMETYPESSUITE_H_
#define CS2SDKTEST_SUITES_GAMETYPESSUITE_H_

#include "testkit/ITestSuite.h"

class IGameTypes;

namespace cs2st
{

class GameTypesSuite : public ITestSuite
{
public:
	const char* Id() const override { return "gametypes"; }
	const char* DisplayName() const override { return "IGameTypes"; }
	const char* Description() const override
	{
		return "The game type / mode / map group tables: alias and name <-> int "
		       "round trips, the current mode, map group walks, per-map data";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseModeTables(TestContext& context);
	void ExerciseCurrentMode(TestContext& context);
	void ExerciseMapGroups(TestContext& context);
	void ExercisePerMap(TestContext& context, const char* map_name);
	void ExerciseFlags(TestContext& context);
	void NoteSkippedCalls(TestContext& context);

	IGameTypes* game_types_ = nullptr;
	bool initialized_ = false;
	// Filled by ExerciseModeTables from the "competitive" alias.
	int alias_type_ = -1;
	int alias_mode_ = -1;
	bool alias_ok_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_GAMETYPESSUITE_H_ */
