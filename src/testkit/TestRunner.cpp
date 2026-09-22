#include "testkit/TestRunner.h"

#include "core/Log.h"
#include "hook/HookRecorder.h"
#include "suites/ConVarSuite.h"
#include "suites/GameEntitySystemSuite.h"
#include "suites/GameEventSuite.h"
#include "suites/MetamodListenerSuite.h"
#include "suites/NetworkMessagesSuite.h"
#include "suites/NetworkServerSuite.h"
#include "suites/SchemaSystemSuite.h"
#include "suites/Source2EngineSuite.h"
#include "suites/Source2GameClientsSuite.h"
#include "suites/Source2ServerSuite.h"
#include "testkit/CallGuard.h"

namespace cs2st
{

TestRunner TestRunner::CreateDefault()
{
	TestRunner runner;

	// One line per suite. Nothing else in the pipeline needs to know they
	// exist; registration order is the order they appear in a log.
	runner.Add(std::unique_ptr<ITestSuite>(new Source2EngineSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new Source2ServerSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new Source2GameClientsSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new ConVarSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new SchemaSystemSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new GameEntitySystemSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new GameEventSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new NetworkServerSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new NetworkMessagesSuite()));
	runner.Add(std::unique_ptr<ITestSuite>(new MetamodListenerSuite()));

	return runner;
}

void TestRunner::Add(std::unique_ptr<ITestSuite> suite)
{
	if (suite != nullptr)
	{
		suites_.push_back(std::move(suite));
	}
}

ITestSuite* TestRunner::Find(const std::string& id)
{
	for (const auto& suite : suites_)
	{
		if (id == suite->Id())
		{
			return suite.get();
		}
	}
	return nullptr;
}

const ITestSuite* TestRunner::Find(const std::string& id) const
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

	for (const auto& suite : suites_)
	{
		if (!only.empty() && only != suite->Id())
		{
			continue;
		}

		context.BeginSection(suite->Id(), suite->DisplayName());

		// Acquire failing is a normal outcome, not an error: a client-only
		// interface is legitimately absent on a dedicated server. Say so once
		// and move on rather than failing every call underneath it.
		if (!suite->Acquire(context))
		{
			context.Note("skipped: could not acquire the interfaces this suite needs");
			context.EndSection();
			continue;
		}

		suite->ExerciseCalls(context);
		context.EndSection();
	}

	return context.tally();
}

int TestRunner::InstallHooks(TestContext& context, const std::string& only)
{
	int installed = 0;

	for (const auto& suite : suites_)
	{
		if (!only.empty() && only != suite->Id())
		{
			continue;
		}
		if (suite->HooksInstalled())
		{
			continue;
		}
		if (!suite->Acquire(context))
		{
			CS2ST_LOG_WARN("%s: cannot install hooks, interfaces unavailable\n", suite->Id());
			continue;
		}

		const int count = suite->InstallHooks(context);
		if (count > 0)
		{
			CS2ST_LOG_INFO("%s: %d hook(s) installed\n", suite->Id(), count);
			installed += count;
		}
	}

	return installed;
}

int TestRunner::RemoveHooks(TestContext& context, const std::string& only)
{
	int touched = 0;

	for (const auto& suite : suites_)
	{
		if (!only.empty() && only != suite->Id())
		{
			continue;
		}
		if (!suite->HooksInstalled())
		{
			continue;
		}

		suite->RemoveHooks(context);
		++touched;
	}

	return touched;
}

} // namespace cs2st
