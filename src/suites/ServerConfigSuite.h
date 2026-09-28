/**
 * Exercises ISource2ServerConfig - the small server-DLL interface the engine
 * asks "may I?" and "what is your ...?" questions of at startup.
 *
 * "Source2ServerConfig001" @ server.
 *
 * Small, cheap, and load-bearing in a way its size does not suggest:
 * AllowDedicatedServers() is what the Metamod loader consults when it
 * intercepts this very interface's Connect() - the single most plugin-
 * adjacent method on the surface. GetTickInterval() is the other pillar:
 * a finite positive subsecond float is a type-level contract, and the
 * interval is cross-checked against the tick-rate convar below.
 *
 * Two oracles are specific to this suite:
 *
 *   - the tick-rate cross-check: two routes to one interval (the getter and
 *     the convar the command line and the engine both know). Which name a
 *     deployment answers to ("sv_tickrate" or the legacy "tickrate") is
 *     policy, not declaration, so a mismatch is Suspect and never Fail -
 *     the same reasoning as GetHostName vs the hostname convar in
 *     NetworkServerSuite.
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
		       "player limits, dedicated-server permission, the tick interval";
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
