/**
 * Exercises ISource2ServerConfig - the small server-DLL interface the engine
 * asks "may I?" and "what is your ...?" questions of at startup.
 *
 * "Source2ServerConfig001" @ server.
 *
 * Small, cheap, and load-bearing in a way its size does not suggest:
 * GetTickInterval() is the tick interval the whole engine's clock arithmetic
 * hangs on (NetworkServerSuite's GetTime formula cross-check consumes it as a
 * tool), and AllowDedicatedServers() is what the Metamod loader consults when
 * it intercepts this very interface's Connect() - the single most plugin-
 * adjacent method on the surface.
 *
 * Two oracles are specific to this suite:
 *
 *   - GetTickInterval() has a type-level contract strong enough to judge:
 *     it is a simulation interval, so finite and strictly positive, and on
 *     the subsecond side (a tick rate below 1 Hz is not a thing the engine
 *     can run). A convar exists for the tick rate ("sv_tickrate" or the
 *     legacy "tickrate"), but its name is policy rather than declaration, so
 *     the cross-check is recorded rather than judged - listed as Suspect when
 *     the two disagree, never Fail.
 *   - GetPlayerLimits() writes four out-parameters the SDK constrains only
 *     as "player limits". The type-level shape - every slot written, and
 *     written into [1, ABSOLUTE_PLAYER_LIMIT] where the value is a count -
 *     is checked; which exact numbers a gamemode picks is not.
 *
 * Everything else on the interface is either read-only-and-recorded (bools
 * the SDK attaches no contract to - inventing one would be exactly the
 * "semantic assertion" mistake this project has retracted twice, see
 * docs/STATUS.md) or a one-line SkipCall with the reason.
 */

#ifndef CS2SDKTEST_SUITES_SERVERCONFIGSUITE_H_
#define CS2SDKTEST_SUITES_SERVERCONFIGSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks, so nothing
// outside the .cpp needs the interface's complete type.
class ISource2ServerConfig;

namespace cs2st
{

class ServerConfigSuite : public ITestSuite
{
public:
	const char* Id() const override { return "serverconfig"; }
	const char* DisplayName() const override { return "ISource2ServerConfig"; }
	const char* Description() const override
	{
		return "The startup-questions interface the engine asks the game DLL: "
		       "tick interval, player limits, dedicated-server permission";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseConfigQueries(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ISource2ServerConfig* config_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SERVERCONFIGSUITE_H_ */
