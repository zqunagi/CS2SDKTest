#include "suites/Tier0PlatformSuite.h"

#include <cstdint>

#include <tier0/platform.h>
#include <tier0/reliabletimer.h>

namespace cs2st
{
namespace
{

/** Reads taken in a row to look for a clock that steps backwards. */
constexpr int kMonotonicReads = 64;

/**
 * Ceiling on every spin loop in this file. Plat_USTime costs tens of
 * nanoseconds, so a 2 ms wait needs tens of thousands of reads; this is far
 * above that, and exists so a clock that stopped advancing ends a loop with a
 * finding instead of hanging the server thread.
 */
constexpr int kMaxSpins = 5000000;

/** How long the tick-rate cross-check watches both clocks, in microseconds. */
constexpr std::uint64_t kWatchMicroSecs = 2000;

/** A deadline short enough to spin to and long enough to measure. */
constexpr std::uint64_t kShortLimitMicroSecs = 1000;

constexpr std::uint64_t kOneSecond = 1000000;
constexpr std::uint64_t kOneHour = 3600ull * 1000000ull;

constexpr unsigned long long ull(std::uint64_t value)
{
	return static_cast<unsigned long long>(value);
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool Tier0PlatformSuite::Acquire(TestContext& context)
{
	// Free functions linked straight from tier0's import library: nothing to
	// query. A tier0 that lacked the exports would fail at load, not here, so the
	// only thing worth recording is that the module is the one answering.
	const Module& module = context.GetModule("tier0");
	if (!module.IsValid())
	{
		context.UnavailableCall("tier0", "tier0 is not loaded");
		return false;
	}

	context.Note("tier0 loaded; Plat_RelativeTicks and Plat_TicksAfterMicroSecs are "
	             "free exports, so there is no vtable to renumber - a missing export "
	             "fails the plugin load itself");
	return true;
}

// -----------------------------------------------------------------------------
//  ExerciseCalls
// -----------------------------------------------------------------------------

void Tier0PlatformSuite::ExerciseCalls(TestContext& context)
{
	ExerciseTicks(context);
	ExerciseDeadlines(context);
	ExerciseLimitTimer(context);
}

// -----------------------------------------------------------------------------
//  Plat_RelativeTicks
//
//  The one fact the SDK states about it is in the declaration's comment: "Clock
//  ticks, never decreasing". That is the only thing judged as Fail here. The tick
//  unit is not stated anywhere, so it is measured below instead of assumed.
// -----------------------------------------------------------------------------

void Tier0PlatformSuite::ExerciseTicks(TestContext& context)
{
	context.Note("[Plat_RelativeTicks] the clock itself: it has to never step backwards");

	CS2ST_CALL(context, "Plat_RelativeTicks", {
		const std::uint64_t first = Plat_RelativeTicks();
		const std::uint64_t second = Plat_RelativeTicks();
		context.Detail("-> %llu, then %llu", ull(first), ull(second));
		if (second < first)
		{
			context.Fail("two consecutive reads went backwards, the SDK says the "
			             "ticks never decrease");
		}
	});

	CS2ST_CALL(context, "Plat_RelativeTicks (monotonic over a run of reads)", {
		std::uint64_t previous = Plat_RelativeTicks();
		const std::uint64_t start = previous;
		int backwards = 0;
		int stalled = 0;
		for (int i = 1; i < kMonotonicReads; ++i)
		{
			const std::uint64_t now = Plat_RelativeTicks();
			if (now < previous)
			{
				++backwards;
			}
			else if (now == previous)
			{
				++stalled;
			}
			previous = now;
		}
		context.Detail("-> %d reads, %d backwards, %d equal to the one before, "
		               "advanced %llu in total", kMonotonicReads, backwards, stalled,
			ull(previous >= start ? previous - start : 0));
		if (backwards > 0)
		{
			context.Fail("%d of %d reads were lower than the one before; the SDK "
			             "says the ticks never decrease", backwards, kMonotonicReads);
		}
		if (stalled == kMonotonicReads - 1)
		{
			// Equal reads are legal on a coarse clock, but a clock that did not
			// move across 64 back-to-back reads and a full loop of work is the
			// shape of a counter that is not counting.
			context.Suspect("the tick clock did not advance across %d reads",
				kMonotonicReads);
		}
	});
}

// -----------------------------------------------------------------------------
//  Plat_TicksAfterMicroSecs
//
//  "Plat_RelativeTicks() nMicroSecs from now" - so against its own clock it is
//  checkable three ways: zero microseconds is now, the answer grows with the
//  argument, and the growth per second is the tick rate, which an independent
//  microsecond clock (Plat_USTime) can then confirm.
// -----------------------------------------------------------------------------

void Tier0PlatformSuite::ExerciseDeadlines(TestContext& context)
{
	context.Note("[Plat_TicksAfterMicroSecs] a deadline in ticks, checked against the "
	             "clock that defines it and against Plat_USTime");

	CS2ST_CALL(context, "Plat_TicksAfterMicroSecs [0 us is now]", {
		const std::uint64_t before = Plat_RelativeTicks();
		const std::uint64_t deadline = Plat_TicksAfterMicroSecs(0);
		const std::uint64_t after = Plat_RelativeTicks();
		context.Detail("-> %llu, between reads of %llu and %llu", ull(deadline),
			ull(before), ull(after));
		if (deadline < before || deadline > after)
		{
			// Suspect: "from now" with zero microseconds is now, but the SDK does
			// not say the two functions read the clock the same way, so a few
			// ticks of rounding would not be damage.
			context.Suspect("a deadline of zero microseconds is not between the "
			                "reads taken either side of it");
		}
	});

	// Both deadlines are taken back to back; the second is a second further out,
	// so the difference is the tick rate with a read's worth of jitter in it.
	std::uint64_t ticks_per_second = 0;

	CS2ST_CALL(context, "Plat_TicksAfterMicroSecs [grows with the argument]", {
		const std::uint64_t now_deadline = Plat_TicksAfterMicroSecs(0);
		const std::uint64_t one_second = Plat_TicksAfterMicroSecs(kOneSecond);
		const std::uint64_t two_seconds = Plat_TicksAfterMicroSecs(2 * kOneSecond);
		context.Detail("-> 0 us: %llu, 1 s: %llu, 2 s: %llu", ull(now_deadline),
			ull(one_second), ull(two_seconds));
		if (one_second <= now_deadline || two_seconds <= one_second)
		{
			context.Suspect("a later deadline did not come out as a larger tick count");
			return;
		}
		ticks_per_second = one_second - now_deadline;
		context.Detail("   one second is %llu ticks", ull(ticks_per_second));

		// Linearity: the second second adds as many ticks as the first, give or
		// take the jitter between the three reads.
		const std::uint64_t first_step = one_second - now_deadline;
		const std::uint64_t second_step = two_seconds - one_second;
		const double ratio = static_cast<double>(second_step) / static_cast<double>(first_step);
		context.Detail("   second step / first step = %.4f", ratio);
		if (ratio < 0.9 || ratio > 1.1)
		{
			context.Suspect("the tick count is not linear in microseconds: the "
			                "second second added %.4f of the first", ratio);
		}
	});

	CS2ST_CALL(context, "Plat_RelativeTicks vs Plat_USTime", {
		if (ticks_per_second == 0)
		{
			context.SkipCall("Plat_RelativeTicks vs Plat_USTime",
				"the tick rate could not be derived from Plat_TicksAfterMicroSecs");
			return;
		}

		// Two independent clocks watching the same interval: the microseconds
		// Plat_USTime counts, scaled by the tick rate the deadline function
		// implies, have to match the ticks Plat_RelativeTicks counted.
		const std::uint64_t ticks_start = Plat_RelativeTicks();
		const std::uint64_t micro_start = Plat_USTime();
		std::uint64_t micro_now = micro_start;
		int spins = 0;
		while (micro_now - micro_start < kWatchMicroSecs && spins < kMaxSpins)
		{
			micro_now = Plat_USTime();
			++spins;
		}
		const std::uint64_t ticks_end = Plat_RelativeTicks();

		if (micro_now - micro_start < kWatchMicroSecs)
		{
			context.Detail("-> Plat_USTime advanced only %llu us in %d reads",
				ull(micro_now - micro_start), spins);
			context.Suspect("Plat_USTime did not advance %llu us within %d reads",
				ull(kWatchMicroSecs), kMaxSpins);
			return;
		}

		const std::uint64_t elapsed_ticks = ticks_end >= ticks_start ? ticks_end - ticks_start : 0;
		const std::uint64_t elapsed_micro = micro_now - micro_start;
		const double expected = static_cast<double>(elapsed_micro) *
			static_cast<double>(ticks_per_second) / static_cast<double>(kOneSecond);
		const double ratio = static_cast<double>(elapsed_ticks) / expected;
		context.Detail("-> %llu ticks over %llu us; %.0f expected at %llu ticks/s "
		               "(ratio %.4f)", ull(elapsed_ticks), ull(elapsed_micro), expected,
			ull(ticks_per_second), ratio);
		if (ratio < 0.5 || ratio > 2.0)
		{
			// Wide on purpose: the two reads either side are not atomic and a
			// preempted thread stretches one of them. A factor of two is not
			// scheduling noise - it is a clock counting in another unit.
			context.Suspect("the tick clock and Plat_USTime disagree about how much "
			                "time passed by a factor of %.2f", ratio);
		}
	});
}

// -----------------------------------------------------------------------------
//  CLimitTimer
//
//  An inline class in the SDK header: SetLimit stores Plat_TicksAfterMicroSecs
//  and BLimitReached compares it with Plat_RelativeTicks. Its two tier0 calls are
//  covered above; what is left is whether the header composes them the way a
//  plugin relies on, with the real clock behind it.
// -----------------------------------------------------------------------------

void Tier0PlatformSuite::ExerciseLimitTimer(TestContext& context)
{
	context.Note("[CLimitTimer] the inline deadline helper over the two exports");

	CS2ST_CALL(context, "CLimitTimer::BLimitReached [a deadline an hour away]", {
		const CLimitTimer timer(kOneHour);
		const bool reached = timer.BLimitReached();
		context.Detail("-> %s", reached ? "true" : "false");
		if (reached)
		{
			context.Suspect("a deadline an hour away is already reached");
		}
	});

	CS2ST_CALL(context, "CLimitTimer::BLimitReached [a zero-length period]", {
		const CLimitTimer timer(0);
		bool reached = timer.BLimitReached();
		int polls = 1;
		// The first poll can land a tick before the deadline the constructor
		// computed; the clock has to catch up within a handful of reads.
		while (!reached && polls < kMonotonicReads)
		{
			reached = timer.BLimitReached();
			++polls;
		}
		context.Detail("-> %s after %d poll(s)", reached ? "true" : "false", polls);
		if (!reached)
		{
			context.Suspect("a zero-length period was not reached in %d polls",
				kMonotonicReads);
		}
	});

	CS2ST_CALL(context, "CLimitTimer::SetLimit [re-arm]", {
		CLimitTimer timer(0);
		int polls = 0;
		while (!timer.BLimitReached() && polls < kMonotonicReads)
		{
			++polls;
		}
		const bool reached_first = timer.BLimitReached();
		timer.SetLimit(kOneHour);
		const bool reached_after = timer.BLimitReached();
		context.Detail("-> reached %s, then %s after SetLimit(one hour)",
			reached_first ? "true" : "false", reached_after ? "true" : "false");
		if (reached_first && reached_after)
		{
			context.Suspect("SetLimit did not move the deadline: still reached after "
			                "re-arming an hour out");
		}
	});

	CS2ST_CALL(context, "CLimitTimer [a real short deadline]", {
		const CLimitTimer timer(kShortLimitMicroSecs);
		const std::uint64_t micro_start = Plat_USTime();
		int spins = 0;
		while (!timer.BLimitReached() && spins < kMaxSpins)
		{
			++spins;
		}
		const std::uint64_t waited = Plat_USTime() - micro_start;
		const bool reached = timer.BLimitReached();
		context.Detail("-> %s after %llu us of Plat_USTime, %d poll(s) (asked for %llu us)",
			reached ? "reached" : "not reached", ull(waited), spins, ull(kShortLimitMicroSecs));
		if (!reached)
		{
			context.Suspect("a %llu us deadline was not reached within %d polls",
				ull(kShortLimitMicroSecs), kMaxSpins);
		}
		else if (waited < kShortLimitMicroSecs * 9 / 10)
		{
			// Early is the finding. Late is only load, and printed above.
			context.Suspect("the deadline fired after %llu us, before the %llu us "
			                "it was set for", ull(waited), ull(kShortLimitMicroSecs));
		}
	});
}

} // namespace cs2st
