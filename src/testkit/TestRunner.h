/**
 * The registry of test modules and the driver that runs them.
 *
 * The registry is empty right now - this is the framework, not the coverage.
 * Adding a module means one #include and one Add() line in CreateDefault(),
 * and nothing else in the pipeline changes.
 */

#ifndef CS2SDKTEST_TESTKIT_TESTRUNNER_H_
#define CS2SDKTEST_TESTKIT_TESTRUNNER_H_

#include <memory>
#include <string>
#include <vector>

#include "testkit/ITestModule.h"

namespace cs2st
{

class TestRunner
{
public:
	TestRunner() = default;

	/** Builds the runner with every test module this build ships. */
	static TestRunner CreateDefault();

	void Add(std::unique_ptr<ITestModule> module);

	ITestModule* Find(const std::string& id);
	const ITestModule* Find(const std::string& id) const;

	std::size_t size() const { return modules_.size(); }
	const std::vector<std::unique_ptr<ITestModule>>& modules() const { return modules_; }

	/**
	 * Runs the call sweep.
	 *
	 * `only` restricts it to one module id; empty runs all of them. A module
	 * that fails to acquire its interface is reported and skipped, never fatal:
	 * a partial sweep is what you want when half the engine just changed.
	 *
	 * Returns the combined tally.
	 */
	RunTally RunCalls(TestContext& context, const std::string& only = std::string());

	/** Installs hooks for one module, or all of them. Returns how many. */
	int InstallHooks(TestContext& context, const std::string& only = std::string());

	/** Removes hooks for one module, or all of them. Returns how many modules. */
	int RemoveHooks(TestContext& context, const std::string& only = std::string());

private:
	std::vector<std::unique_ptr<ITestModule>> modules_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_TESTRUNNER_H_ */
