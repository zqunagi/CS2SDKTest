/**
 * Exercises Metamod:Source's own plugin-facing surface: ISmmAPI, and the
 * callbacks that arrive through IMetamodListener.
 *
 * This is the only suite that does not test s2sdk, and it is here for one
 * reason: on Source 2, LevelInit and LevelShutdown do not exist on
 * ISource2Server. They reach a plugin *only* as IMetamodListener::OnLevelInit /
 * OnLevelShutdown, which Metamod synthesises by hooking ILoopMode::LoopInit and
 * LoopShutdown. That path runs through Metamod's provider, not through the game
 * DLL interface, so the only way to cover map lifecycle at all is to cover the
 * listener - and the only way to know the listener still works is to register
 * one and watch.
 *
 * Two things make this different from every other suite:
 *
 *   - It is not SourceHook. There is no vtable patch and no pre/post pair;
 *     Metamod calls us directly because we asked it to via
 *     ISmmAPI::AddListener. The handlers still record an enter *and* a leave,
 *     so the dump's balance column keeps meaning something - though here it
 *     only proves our own handler ran to completion, since we are the callee.
 *
 *   - Registration is one-way. ISmmAPI has AddListener and no RemoveListener;
 *     Metamod only drops a plugin's listeners when the plugin unloads. So
 *     "removing" these hooks can only stop the recording, not the callbacks,
 *     and re-installing must not register a second time or every callback
 *     would be counted twice.
 *
 * The call side covers ISmmAPI itself. Most of it is version and factory
 * plumbing that a plugin depends on before it can do anything else, and it has
 * a property worth exploiting: several of these have exactly one correct
 * answer, which makes them sanity checks rather than mere recordings.
 */

#ifndef CS2SDKTEST_SUITES_METAMODLISTENERSUITE_H_
#define CS2SDKTEST_SUITES_METAMODLISTENERSUITE_H_

#include <ISmmPlugin.h>

#include "testkit/ITestSuite.h"

namespace cs2st
{

class MetamodListenerSuite : public ITestSuite, public IMetamodListener
{
public:
	const char* Id() const override { return "metamod"; }
	const char* DisplayName() const override { return "ISmmAPI / IMetamodListener"; }
	const char* Description() const override
	{
		return "Metamod's own API, and the listener that carries level lifecycle";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

	int InstallHooks(TestContext& context) override;
	void RemoveHooks(TestContext& context) override;
	bool HooksInstalled() const override { return recording_; }

	// ---- IMetamodListener --------------------------------------------------
	//
	// Every one of these must behave exactly like the default it overrides: the
	// listener list is walked for real work (the *Query hooks can supply an
	// interface and short-circuit the search), so a handler that answers
	// differently is no longer observing, it is intervening.

	void OnLevelInit(char const* map_name,
	                 char const* map_entities,
	                 char const* old_level,
	                 char const* landmark_name,
	                 bool load_game,
	                 bool background) override;
	void OnLevelShutdown() override;

	void OnPluginLoad(PluginId id) override;
	void OnPluginUnload(PluginId id) override;
	void OnPluginPause(PluginId id) override;
	void OnPluginUnpause(PluginId id) override;

	void* OnEngineQuery(const char* iface, int* ret) override;
	void* OnPhysicsQuery(const char* iface, int* ret) override;
	void* OnFileSystemQuery(const char* iface, int* ret) override;
	void* OnGameDLLQuery(const char* iface, int* ret) override;
	void* OnMetamodQuery(const char* iface, int* ret) override;

	void OnVSPListening(IServerPluginCallbacks* iface) override;

	void OnUnlinkConCommandBase(PluginId id, ConCommandBase* command) override;
	void OnUnlinkConCommand(PluginId id, ProviderConCommand* command) override;
	void OnUnlinkConVar(PluginId id, ProviderConVar* var) override;

private:
	void ExerciseVersions(TestContext& context);
	void ExerciseFactories(TestContext& context);
	void ExerciseMetaFactory(TestContext& context);
	void ExerciseFormatting(TestContext& context);
	void ExerciseUserMessages(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);
	void NoteListenerExpectations(TestContext& context);

	/** Shared body of the five *Query callbacks: record, then decline. */
	void* DeclineQuery(const char* hook_id, const char* iface, int* ret);

	/** Records one plugin-lifecycle callback. */
	void RecordPluginEvent(const char* hook_id, PluginId id);

	ISmmAPI* smm_ = nullptr;

	/**
	 * AddListener has been called. Never goes back to false while loaded -
	 * Metamod offers no way to withdraw a listener - so it exists purely to
	 * stop a second InstallHooks from registering twice.
	 */
	bool listener_registered_ = false;

	/** Whether the callbacks currently record. This is what install/remove move. */
	bool recording_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_METAMODLISTENERSUITE_H_ */
