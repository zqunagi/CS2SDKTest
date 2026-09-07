/**
 * The registry of test suites and the driver that runs them.
 *
 * Adding a suite means one #include and one Add() line in CreateDefault(),
 * and nothing else in the pipeline changes.
 */

#ifndef CS2SDKTEST_TESTKIT_TESTRUNNER_H_
#define CS2SDKTEST_TESTKIT_TESTRUNNER_H_

#include <memory>
#include <string>
#include <vector>

#include "testkit/ITestSuite.h"

namespace cs2st
{

class TestRunner
{
public:
	TestRunner() = default;

	/** Builds the runner with every test suite this build ships. */
	static TestRunner CreateDefault();

	void Add(std::unique_ptr<ITestSuite> suite);

	ITestSuite* Find(const std::string& id);
	const ITestSuite* Find(const std::string& id) const;

	std::size_t size() const { return suites_.size(); }
	const std::vector<std::unique_ptr<ITestSuite>>& suites() const { return suites_; }

	/**
	 * Runs the call sweep.
	 *
	 * `only` restricts it to one suite id; empty runs all of them. A suite
	 * that fails to acquire its interface is reported and skipped, never fatal:
	 * a partial sweep is what you want when half the engine just changed.
	 *
	 * Returns the combined tally.
	 */
	RunTally RunCalls(TestContext& context, const std::string& only = std::string());

	/** Installs hooks for one suite, or all of them. Returns how many. */
	int InstallHooks(TestContext& context, const std::string& only = std::string());

	/** Removes hooks for one suite, or all of them. Returns how many suites. */
	int RemoveHooks(TestContext& context, const std::string& only = std::string());

private:
	std::vector<std::unique_ptr<ITestSuite>> suites_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_TESTRUNNER_H_ */
