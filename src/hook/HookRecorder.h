/**
 * Where hook callbacks report in.
 *
 * A hook is only useful as evidence if you can answer three questions later:
 * did it fire at all, did its arguments look sane, and did it come back.
 *
 *   - stopped firing        the virtual it was attached to probably moved slots
 *   - fires with nonsense   the signature changed underneath it
 *   - enters but never      the original function faulted, or another plugin
 *     leaves                superseded it
 *
 * That third one is why entry and return are counted separately. A suite hooks
 * both pre and post: pre records the entry, post records the return. Equal
 * counts mean the call went all the way through. Unequal means something
 * swallowed it, and *which* hook is unbalanced says where.
 *
 * A hook is not logged per invocation. GameFrame alone would be tens of
 * thousands of lines a minute, and the interesting part is not the thousandth
 * call. Each hook keeps counts plus the first few argument snapshots, and the
 * whole set is dumped on demand.
 *
 * The exception is tracing: the first few entries and returns of each hook do
 * get written to the session log, one line each. The log is flushed per line,
 * so when a fault happens inside a hooked call the last line in the file is an
 * `enter` with no matching `leave` - which names the culprit even though the
 * process is gone.
 *
 * Declare() before install so a hook that never fires still appears in the
 * dump. "Never fired" is a result, and a silently missing row is not.
 */

#ifndef CS2SDKTEST_HOOK_HOOKRECORDER_H_
#define CS2SDKTEST_HOOK_HOOKRECORDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "core/Platform.h"

namespace cs2st
{

class HookRecorder
{
public:
	/** Argument snapshots kept per hook before it stops recording them. */
	static constexpr std::size_t kMaxSamples = 3;

	/** Entries and returns per hook that get a traced line in the log. */
	static constexpr std::size_t kMaxTraced = 3;

	static HookRecorder& Get();

	/**
	 * Registers a hook id so it shows up in the dump whether or not it fires.
	 * Safe to call more than once for the same id.
	 */
	void Declare(const char* id, const char* owner);

	/** Records entry to a hooked call. Call from the pre-hook. */
	void Enter(const char* id);

	/** Records entry, keeping the formatted arguments as a sample. */
	void EnterWithArgs(const char* id, const char* format, ...) CS2ST_PRINTF_FMT(3, 4);

	/** Records that a hooked call returned. Call from the post-hook. */
	void Leave(const char* id);

	/** Drops every count and sample, keeping declarations. */
	void ResetCounts();

	/** Forgets everything, including declarations. */
	void Clear();

	/** Writes the current state as an aligned table. */
	void Dump() const;

	std::size_t declared() const { return entries_.size(); }
	std::size_t fired() const;

	/** Hooks that were entered more often than they were seen to return. */
	std::size_t unbalanced() const;

private:
	struct Entry
	{
		std::string id;
		std::string owner;          ///< test suite that installed it
		std::uint64_t enters = 0;
		std::uint64_t leaves = 0;
		std::vector<std::string> samples;
		std::size_t traced_enters = 0;
		std::size_t traced_leaves = 0;
	};

	// Takes const char* rather than std::string on purpose: this runs from
	// inside GameFrame, so a std::string temporary here would be a heap
	// allocation every tick, per hook, forever.
	Entry* FindOrCreate(const char* id);

	std::vector<Entry> entries_;
};

} // namespace cs2st

/**
 * Use from inside a hook callback. Keep these cheap: several of them run on
 * every tick, in the server's own frame.
 */
#define CS2ST_HOOK_ENTER(id) \
	::cs2st::HookRecorder::Get().Enter(id)

#define CS2ST_HOOK_ENTER_ARGS(id, ...) \
	::cs2st::HookRecorder::Get().EnterWithArgs(id, __VA_ARGS__)

#define CS2ST_HOOK_LEAVE(id) \
	::cs2st::HookRecorder::Get().Leave(id)

#endif /* CS2SDKTEST_HOOK_HOOKRECORDER_H_ */
