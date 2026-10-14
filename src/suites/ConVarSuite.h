/**
 * Exercises the console variable and command system.
 *
 * "VEngineCvar007" resolves to ICvar, which lives in tier0 rather than in
 * engine2 - the only interface this plugin tests that does. It derives from
 * IAppSystem, so the first eleven slots are the usual base class and the
 * thirty-five after them are the cvar system proper.
 *
 * Two things make this suite different from the others.
 *
 * First, most of convar.h is not engine code. ConVarRefAbstract, ConVarData's
 * getters, CCommand, CVValue_t and the CVarTypeTraits table are all header or
 * tier1 code compiled *into this plugin*, so checking them against themselves
 * would prove nothing. What actually crosses into the game is a shorter list,
 * and it is what this suite aims at:
 *
 *   - the ICvar vtable itself,
 *   - ConVarData and ConCommandData, which tier0's CCvar allocates and we read
 *     back through the layout the SDK declares,
 *   - ConVarRef and ConCommandRef, 8-byte handles returned *by value* from
 *     engine functions, so their size and field order is a register-level ABI
 *     contract rather than a memory layout,
 *   - CCommand and ConVarCreation_t, which travel the other way: we build them
 *     and the game reads them.
 *
 * Second, this is the one interface where ground truth is available. Every
 * other suite has to infer what a correct answer looks like; here the suite
 * registers its own convars and its own console commands at load time, with
 * known names, types, defaults, bounds, flags and callbacks. The engine then
 * has to agree with values we chose ourselves - which turns a large part of
 * this file into two-path oracles instead of recorded return values:
 *
 *   FindConVar by name          vs the ConVarRef our own registration produced
 *   ICvar::GetConVarData        vs ConVarRefAbstract::GetConVarData
 *   CCommand::DefaultBreakSet   vs ICvar::GetCharacterSet
 *   ConVarData::GetMaxSplit...  vs ICvar::GetMaxSplitScreenSlots
 *   a dispatched command        vs our own callback invocation counter
 *   FindConVar(name, false)     vs FindConVar(name, true) on an FCVAR_DEFENSIVE
 *                                  cvar registered for exactly that purpose
 *
 * Writes are confined to convars this plugin owns. Nothing here sets an engine
 * cvar: the value path has to be exercised to mean anything, and doing it on
 * our own registrations tests the same code without changing how the server
 * plays. The methods that would reach every cvar in the process -
 * StripDevelopmentFlags, ResetConVarsToDefaultValuesByFlag,
 * ResetConVarsToSnapshot, SetConVarsFromGameInfo, SetMaxSplitScreenSlots - are
 * listed and skipped, one line each.
 */

#ifndef CS2SDKTEST_SUITES_CONVARSUITE_H_
#define CS2SDKTEST_SUITES_CONVARSUITE_H_

#include <string>

// Unlike the suites that forward-declare their interface, this one needs the
// SDK in the header: IConVarListener is a base class below, and the hook
// handlers have to spell out ConCommandRef, ConVarRef, ConVarCreation_t and
// CCommandContext by name. icvar.h is where that whole set is declared.
#include <tier0/icvar.h>
#include <khook.hpp>

#include "testkit/ITestSuite.h"

namespace cs2st
{

class ConVarSuite : public ITestSuite, public IConVarListener
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

	int InstallHooks(TestContext& context) override;
	void RemoveHooks(TestContext& context) override;
	bool HooksInstalled() const override { return hooks_installed_; }

	// ---- IConVarListener ---------------------------------------------------
	//
	// Observation only. The engine walks its listener list for real work, so a
	// handler that does anything beyond counting is intervening, not watching.

	void OnConVarCreated(ConVarRefAbstract* new_cvar) override;
	void OnConCommandCreated(ConCommand* new_command) override;

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
	void InspectCvarObjectLayout(TestContext& context);
	void InspectCharacterSetLayout(TestContext& context);
	void InspectConVarRefLayout(TestContext& context);
	void InspectConCommandDataLayout(TestContext& context);
	void InspectConVarDataLayout(TestContext& context);
	void NoteTypeTraitsTable(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	// ---- KHook handlers ----------------------------------------------------
	//
	// Signatures copied from icvar.h rather than paraphrased. KHook resolves
	// the vtable slot from the SDK member pointer, so a drifted signature is a
	// compile error here rather than a silent garbage read - which is the drift
	// detection this suite exists for, moved to build time.

public:
	ConVarSuite();

private:
	KHook::Return<void> Pre_DispatchConCommand(ICvar*, ConCommandRef cmd, const CCommandContext& ctx,
	                                           const CCommand& args);
	KHook::Return<void> Post_DispatchConCommand(ICvar*, ConCommandRef cmd, const CCommandContext& ctx,
	                                            const CCommand& args);

	KHook::Return<void> Pre_RegisterConVar(ICvar*, const ConVarCreation_t& setup, uint64 flags,
	                                       ConVarRef* cvar_ref, ConVarData** cvar_data);
	KHook::Return<void> Post_RegisterConVar(ICvar*, const ConVarCreation_t& setup, uint64 flags,
	                                        ConVarRef* cvar_ref, ConVarData** cvar_data);

	KHook::Return<ConCommandRef> Pre_RegisterConCommand(ICvar*, const ConCommandCreation_t& setup, uint64 flags);
	KHook::Return<ConCommandRef> Post_RegisterConCommand(ICvar*, const ConCommandCreation_t& setup, uint64 flags);

	KHook::Return<void> Pre_CallGlobalChangeCallbacks(ICvar*, ConVarRefAbstract* ref, CSplitScreenSlot slot,
	                                                  const char* new_value, const char* old_value,
	                                                  ConVarUserInfoSet_t* userinfo_data);
	KHook::Return<void> Post_CallGlobalChangeCallbacks(ICvar*, ConVarRefAbstract* ref, CSplitScreenSlot slot,
	                                                   const char* new_value, const char* old_value,
	                                                   ConVarUserInfoSet_t* userinfo_data);

	KHook::Return<void> Pre_UnregisterConVarCallbacks(ICvar*, ConVarRef cvar);
	KHook::Return<void> Post_UnregisterConVarCallbacks(ICvar*, ConVarRef cvar);

	KHook::Return<void> Pre_UnregisterConCommandCallbacks(ICvar*, ConCommandRef cmd);
	KHook::Return<void> Post_UnregisterConCommandCallbacks(ICvar*, ConCommandRef cmd);

	// ---- the hooks themselves ------------------------------------------------
	//
	// One KHook::Virtual per virtual, bound in the constructor; template
	// parameters are the interface, the return type, then the SDK's argument
	// list. RegisterConCommand returns ConCommandRef by value, which is why its
	// handlers return KHook::Return<ConCommandRef>.
	KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext&, const CCommand&> hook_DispatchConCommand_;
	KHook::Virtual<ICvar, void, const ConVarCreation_t&, uint64, ConVarRef*, ConVarData**> hook_RegisterConVar_;
	KHook::Virtual<ICvar, ConCommandRef, const ConCommandCreation_t&, uint64> hook_RegisterConCommand_;
	KHook::Virtual<ICvar, void, ConVarRefAbstract*, CSplitScreenSlot, const char*, const char*, ConVarUserInfoSet_t*> hook_CallGlobalChangeCallbacks_;
	KHook::Virtual<ICvar, void, ConVarRef> hook_UnregisterConVarCallbacks_;
	KHook::Virtual<ICvar, void, ConCommandRef> hook_UnregisterConCommandCallbacks_;

	ICvar* cvar_ = nullptr;

	/** Which module factory answered "VEngineCvar007". */
	std::string cvar_module_;

	bool hooks_installed_ = false;

	/**
	 * The persistent creation listener has been handed to the engine.
	 * RegisterCreationListeners has a proper inverse, so unlike Metamod's
	 * AddListener this does go back to false in RemoveHooks.
	 */
	bool listener_registered_ = false;

	/** The persistent global change callback is installed. */
	bool global_callback_installed_ = false;

	/**
	 * ICvar::RegisterConVar and RegisterConCommand have already been called
	 * directly during this load.
	 *
	 * CCvar allocates names and value storage out of buffers it never frees,
	 * and the SDK says an unregistered cvar stays in the lists so old refs keep
	 * working - so a direct registration is permanent. Once per plugin load is
	 * what any plugin costs; once per `meta_test run` would be a slow leak.
	 */
	bool dynamic_registered_ = false;

	/** What the direct registration produced; read back by later sections. */
	ConVarRef dynamic_cvar_;
	ConVarData* dynamic_cvar_data_ = nullptr;
	ConCommandRef dynamic_command_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_CONVARSUITE_H_ */
