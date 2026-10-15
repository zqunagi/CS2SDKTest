/**
 * Byte-pattern scanning over a module the game has already mapped.
 *
 * This is the piece every CS2 plugin ships and almost none tests: resolving a
 * function from its bytes ("signature") rather than an export. The pattern
 * syntax is the ecosystem's - hex byte pairs separated by spaces, where "?" or
 * "??" matches anything - so a pattern copied out of another project's
 * gamedata means the same thing here.
 *
 * The scan bounds come from Module (PE SizeOfImage / the loaded program
 * headers), which includes headers and padding, not just executable code. A
 * signature matching twice in those bounds is reported as ambiguous rather
 * than picking the first: two hits is how a too-short pattern announces
 * itself, and the first hit is not more likely to be the function than the
 * second.
 *
 * Scanning is read-only and never faults on its own account - the memory is
 * mapped for the whole scan range - but callers still run resolution inside
 * CS2ST_CALL, because the *consequences* of a resolved address (calling it)
 * absolutely can.
 */

#ifndef CS2SDKTEST_CORE_SIGNATURESCAN_H_
#define CS2SDKTEST_CORE_SIGNATURESCAN_H_

#include <cstddef>
#include <string>
#include <vector>

#include "core/Module.h"

namespace cs2st
{

/** A pattern parsed once: bytes with per-position wildcard flags. */
struct BytePattern
{
	std::vector<unsigned char> bytes;
	std::vector<bool> wildcard;

	bool empty() const { return bytes.empty(); }
	std::size_t size() const { return bytes.size(); }
};

struct PatternScanResult
{
	/** Number of matches inside the scanned range (capped; see kMaxMatches). */
	std::size_t matches = 0;

	/** The first match, or nullptr when there was none. */
	const void* first = nullptr;

	/** A second match, so an ambiguous pattern can show where it landed. */
	const void* second = nullptr;
};

/** Parses "48 89 5C 24 ? ..." into bytes+wildcards. Error text when null. */
const char* ParseBytePattern(const std::string& text, BytePattern& out);

/**
 * Scans `module` for `pattern`. Never returns a guess: matches > 1 leaves
 * first/second set so the caller can report the ambiguity.
 */
PatternScanResult ScanModule(const Module& module, const BytePattern& pattern);

} // namespace cs2st

#endif /* CS2SDKTEST_CORE_SIGNATURESCAN_H_ */
