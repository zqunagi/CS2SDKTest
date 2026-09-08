/**
 * Vetting a hook argument that cannot be trusted.
 *
 * A hook callback is handed whatever the caller put in the register the
 * signature says holds a pointer. When the signature is right that is a
 * pointer. When it is wrong - and a changed signature is a primary thing this
 * plugin exists to catch - it is a float, a bool, or the third field of some
 * struct, and touching it faults inside the very callback that was supposed to
 * report the problem. Worse, a hook runs in the engine's own frame, where
 * CallGuard is not armed: a fault there takes the server down rather than
 * producing a CRASHED line.
 *
 * A null check does not help: a float's bit pattern is almost never null. So
 * the address is vetted for plausibility first, and string callers additionally
 * cap the length with a precision specifier ("%.63s") so a missing terminator
 * cannot walk off the end of a page.
 *
 * Deliberately separate from StructInspector's pointer check. That one is about
 * struct layout, reports into a TestContext, and has no reason to be cheap;
 * these run on the tick path and must stay a handful of compares with no
 * cross-module coupling.
 *
 * Neither can promise a plausible address is actually mapped - there is no
 * portable way to find out short of reading it. What they rule out is the set
 * of values a misread argument is in practice.
 */

#ifndef CS2SDKTEST_HOOK_HOOKARG_H_
#define CS2SDKTEST_HOOK_HOOKARG_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace cs2st
{

/**
 * True if `address` could be a live user-space pointer to an object.
 *
 * The first 64 KiB is the null guard region, and user-space addresses on x86-64
 * stay under 2^47. 8-byte alignment is required as well, because anything
 * holding a pointer or a 64-bit field is aligned that way - which makes this
 * stricter than the string check below, where literals sit packed in .rodata on
 * arbitrary bytes.
 */
inline bool LooksLikeObjectPointer(const void* address)
{
	const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);

	return value >= 0x10000
		&& value < (std::uintptr_t(1) << 47)
		&& (value % 8) == 0;
}

/**
 * Returns something safe to hand to "%s".
 *
 * `scratch` holds the rendering of a rejected pointer, so it must outlive the
 * format call that consumes the result.
 */
template <std::size_t N>
const char* SafeStringArg(const char* text, char (&scratch)[N])
{
	if (text == nullptr)
	{
		return "(null)";
	}

	const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(text);

	// The first 64 KiB is the null guard region, and user-space addresses on
	// x86-64 stay under 2^47. No alignment requirement: string literals sit
	// packed in .rodata on arbitrary bytes.
	if (value < 0x10000 || value >= (std::uintptr_t(1) << 47))
	{
		std::snprintf(scratch, N, "<bad ptr 0x%llx>",
			static_cast<unsigned long long>(value));
		return scratch;
	}

	// A plausible address can still be unmapped, and there is no portable way
	// to find out short of reading it. Callers cap the length with a precision
	// specifier so a missing terminator cannot walk off the page.
	return text;
}

} // namespace cs2st

#endif /* CS2SDKTEST_HOOK_HOOKARG_H_ */
