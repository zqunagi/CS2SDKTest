#ifndef CS2SDKTEST_SUITES_CONVARSUITE_H_
#define CS2SDKTEST_SUITES_CONVARSUITE_H_

#include <string>

#include <tier0/icvar.h>

#include "testkit/ITestSuite.h"

namespace cs2st
{

class ConVarSuite : public ITestSuite
{
public:
	const char* Id() const override { return "convar"; }
	const char* DisplayName() const override { return "ICvar / ConVar / ConCommand"; }
	const char* Description() const override
	{
		return "The cvar and command system, checked against convars this plugin owns";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	// ---- call sweep --------------------------------------------------------

	void ExerciseAppSystem(TestContext& context);
	void ExerciseOwnRegistrations(TestContext& context);
	void ExerciseConVarLookup(TestContext& context);
	void ExerciseConVarRefApi(TestContext& context);
	void ExerciseConVarDataApi(TestContext& context);
	void ExerciseTypedRefs(TestContext& context);
	void ExerciseValueWrites(TestContext& context);
	void ExerciseChangeCallbacks(TestContext& context);
	void ExerciseCreationListeners(TestContext& context);
	void ExerciseDynamicRegistration(TestContext& context);
	void ExerciseConCommandLookup(TestContext& context);
	void ExerciseCallbackInfoShapes(TestContext& context);
	void ExerciseCommandTokenizer(TestContext& context);
	void ExerciseCommandDispatch(TestContext& context);
	void ExerciseCompletion(TestContext& context);
	void ExerciseSnapshots(TestContext& context);
	void ExerciseUserInfoCvars(TestContext& context);
	void ExerciseCharacterSet(TestContext& context);
	void ExerciseScratchCalls(TestContext& context);
	void InspectCvarObjectLayout(TestContext& context);
	void InspectCharacterSetLayout(TestContext& context);
	void InspectConVarRefLayout(TestContext& context);
	void InspectConCommandDataLayout(TestContext& context);
	void InspectConVarDataLayout(TestContext& context);
	void NoteTypeTraitsTable(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ICvar* cvar_ = nullptr;

	/** Which module factory answered "VEngineCvar007". */
	std::string cvar_module_;

	/**
	 * ICvar::RegisterConVar and RegisterConCommand have already been called
	 * directly during this load.
	 *
	 * CCvar allocates names and value storage out of buffers it never frees,
	 * and the SDK says an unregistered cvar stays in the lists so old refs keep
	 * working - so a direct registration is permanent. Once per plugin load is
	 * what any plugin costs; once per `cst run` would be a slow leak.
	 */
	bool dynamic_registered_ = false;

	/** What the direct registration produced; read back by later sections. */
	ConVarRef dynamic_cvar_;
	ConVarData* dynamic_cvar_data_ = nullptr;
	ConCommandRef dynamic_command_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_CONVARSUITE_H_ */
