#include "testkit/TestRunner.h"

#include "core/Log.h"
#include "hook/HookRecorder.h"
#include "testkit/CallGuard.h"

// Test module headers go here as they land, e.g.:
//   #include "modules/EngineServerModule.h"

namespace cs2st
{

TestRunner TestRunner::CreateDefault()
{
	TestRunner runner;

	// Register test modules here. One line each:
	//   runner.Add(std::unique_ptr<ITestModule>(new EngineServerModule()));
	//
	// Nothing else in the pipeline needs to know they exist.

	return runner;
}

void TestRunner::Add(std::unique_ptr<ITestModule> module)
{
	if (module != nullptr)
	{
		modules_.push_back(std::move(module));
	}
}

ITestModule* TestRunner::Find(const std::string& id)
{
	for (const auto& module : modules_)
	{
		if (id == module->Id())
		{
			return module.get();
		}
	}
	return nullptr;
}

const ITestModule* TestRunner::Find(const std::string& id) const
{
	return const_cast<TestRunner*>(this)->Find(id);
}

RunTally TestRunner::RunCalls(TestContext& context, const std::string& only)
{
	// One scope around the whole sweep. On Linux this is what installs the
	// fault handlers, and it takes the engine's own out of the way for as long
	// as it is alive - so it wraps the sweep, not each call.
	FaultGuardScope guard;
	if (!FaultGuardScope::IsActive())
	{
		CS2ST_LOG_WARN("fault guard could not be armed; a bad call will crash the server\n");
	}

	for (const auto& module : modules_)
	{
		if (!only.empty() && only != module->Id())
		{
			continue;
		}

		context.BeginSection(module->Id(), module->DisplayName());

		// Acquire failing is a normal outcome, not an error: a client-only
		// interface is legitimately absent on a dedicated server. Say so once
		// and move on rather than failing every call underneath it.
		if (!module->Acquire(context))
		{
			context.Note("skipped: could not acquire the interfaces this module needs");
			context.EndSection();
			continue;
		}

		module->ExerciseCalls(context);
		context.EndSection();
	}

	return context.tally();
}

int TestRunner::InstallHooks(TestContext& context, const std::string& only)
{
	int installed = 0;

	for (const auto& module : modules_)
	{
		if (!only.empty() && only != module->Id())
		{
			continue;
		}
		if (module->HooksInstalled())
		{
			continue;
		}
		if (!module->Acquire(context))
		{
			CS2ST_LOG_WARN("%s: cannot install hooks, interfaces unavailable\n", module->Id());
			continue;
		}

		const int count = module->InstallHooks(context);
		if (count > 0)
		{
			CS2ST_LOG_INFO("%s: %d hook(s) installed\n", module->Id(), count);
			installed += count;
		}
	}

	return installed;
}

int TestRunner::RemoveHooks(TestContext& context, const std::string& only)
{
	int touched = 0;

	for (const auto& module : modules_)
	{
		if (!only.empty() && only != module->Id())
		{
			continue;
		}
		if (!module->HooksInstalled())
		{
			continue;
		}

		module->RemoveHooks(context);
		++touched;
	}

	return touched;
}

} // namespace cs2st
