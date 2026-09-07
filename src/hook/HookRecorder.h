/**
 * Where hook callbacks report in.
 *
 * A hook is only useful as evidence if you can answer two questions later:
 * did it fire at all, and did its arguments look sane. Both matter for
 * different reasons - a hook that stops firing after an update usually means
 * the virtual it was attached to moved slots, while a hook that still fires
 * with nonsense arguments means the signature changed underneath it.
 *
 * So a hook is not logged per invocation. GameFrame alone would be tens of
 * thousands of lines in a minute, and the interesting part is not the
 * thousandth call. Instead each hook keeps a count plus the first few argument
 * snapshots, and the whole set is dumped on demand.
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

	static HookRecorder& Get();

	/**
	 * Registers a hook id so it shows up in the dump whether or not it fires.
	 * Safe to call more than once for the same id.
	 */
	void Declare(const char* id, const char* owner);

	/** Records one invocation with no argument detail. */
	void Fire(const char* id);

	/** Records one invocation, keeping the formatted arguments as a sample. */
	void FireWithArgs(const char* id, const char* format, ...) CS2ST_PRINTF_FMT(3, 4);

	/** Drops every count and sample, keeping declarations. */
	void ResetCounts();

	/** Forgets everything, including declarations. */
	void Clear();

	/** Writes the current state as an aligned table. */
	void Dump() const;

	std::size_t declared() const { return entries_.size(); }
	std::size_t fired() const;

private:
	struct Entry
	{
		std::string id;
		std::string owner;          ///< test module that installed it
		std::uint64_t count = 0;
		std::vector<std::string> samples;
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
#define CS2ST_HOOK_FIRED(id) \
	::cs2st::HookRecorder::Get().Fire(id)

#define CS2ST_HOOK_FIRED_ARGS(id, ...) \
	::cs2st::HookRecorder::Get().FireWithArgs(id, __VA_ARGS__)

#endif /* CS2SDKTEST_HOOK_HOOKRECORDER_H_ */
