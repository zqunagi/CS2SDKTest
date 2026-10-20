/**
 * Machine-readable results, written while the run is still going.
 *
 * Two files land in the output directory:
 *
 *   results.jsonl  append-only, one JSON object per line, flushed per line.
 *                  A `begin` line is written *before* each call and a `result`
 *                  line after it, so a `begin` with no matching `result` at the
 *                  end of the file names the call that took the process down.
 *   results.json   written once, when the sweep finishes. "completed": true is
 *                  the only thing that says the run ran to the end - a missing
 *                  file or a missing flag means the run is not to be trusted.
 *
 * The sink is inactive until Open() is called, so a TestContext used outside a
 * sweep (tests, hook installs) records nothing.
 */

#ifndef CS2SDKTEST_TESTKIT_RESULTSINK_H_
#define CS2SDKTEST_TESTKIT_RESULTSINK_H_

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "testkit/CallGuard.h"
#include "testkit/TestContext.h"

namespace cs2st
{

class ResultSink
{
public:
	static ResultSink& Get();

	/**
	 * Overrides the output directory. Empty restores the default, the plugin's
	 * log directory. Takes effect at the next Open().
	 */
	void SetDirectory(const std::string& directory) { directory_override_ = directory; }

	/** Free-form label for what this run covers, e.g. "all" or a suite id. */
	void SetScope(const std::string& scope) { scope_ = scope; }

	/** Starts a fresh run: truncates results.jsonl and writes the header line. */
	bool Open();

	bool active() const { return file_ != nullptr; }

	/** A call is about to run. */
	void Begin(const std::string& suite, const std::string& call);

	/** A call finished (or was recorded without running). */
	void Result(const std::string& suite, const std::string& call,
		CallOutcome outcome, const std::string& detail);

	/** Writes results.json with "completed": true and closes the jsonl. */
	void Finish(const RunTally& tally);

	/** Closes without a summary; used when the run is abandoned. */
	void Abort(const std::string& reason);

	const std::string& json_path() const { return json_path_; }

private:
	struct Entry
	{
		std::string suite;
		std::string call;
		std::string outcome;
		std::string detail;
		long long ms = 0;
	};

	void WriteLine(const std::string& line);
	void Close();
	std::string MetaJson() const;

	std::string directory_override_;
	std::string scope_ = "all";
	std::string jsonl_path_;
	std::string json_path_;
	std::string started_utc_;

	std::FILE* file_ = nullptr;
	std::vector<Entry> entries_;

	std::chrono::steady_clock::time_point run_start_;
	std::chrono::steady_clock::time_point call_start_;
	bool call_open_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_RESULTSINK_H_ */
