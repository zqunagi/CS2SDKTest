/**
 * The few process-level operations the entry point needs that differ between
 * Linux and Windows. Kept out of Entry.cpp for the same reason Platform.h exists:
 * Entry.cpp includes SDK headers, and <Windows.h> does not mix with those.
 */

#ifndef CS2SDKTEST_CORE_PROCESS_H_
#define CS2SDKTEST_CORE_PROCESS_H_

#include <cstddef>
#include <vector>

namespace cs2st
{
namespace proc
{

/**
 * Is this process the executable called `stem`? "cs2" matches .../cs2 on Linux
 * and ...\cs2.exe on Windows (case-insensitively there).
 */
bool IsExecutable(const char* stem);

/**
 * Stores `value` into the pointer-sized slot at `slot`, which may sit in a
 * read-only page (a vtable in .rdata / .data.rel.ro). The page's protection is
 * put back afterwards. Returns false, leaving the slot alone, if the page could
 * not be made writable.
 */
bool PatchPointerSlot(void** slot, void* value);

/**
 * Starts of every function that takes the address of the C string `text`
 * (a `lea reg, [rip+disp32]` in executable code), for the image at
 * [base, base + size). Found from the string outwards: locate the string in the
 * image's readable data, find the instructions that point at it, and ask the
 * image's unwind tables (.pdata on Windows, .eh_frame_hdr on Linux) which
 * function each one belongs to. Nothing has to guess where a function ends.
 *
 * `known_starts` are addresses the caller knows to be function starts (the slots
 * of a vtable, say). Windows does not need them: .pdata covers every function that
 * has a frame or a call. A Linux build may carry unwind records for only a small
 * share of its functions (libserver.so has 8328 for 25 MB of code, and GameFrame
 * has none), so there they are merged with the unwind table: a reference belongs
 * to the nearest known or recorded start at or below it.
 *
 * Returns false when the image cannot be described (not found, or no usable
 * unwind table); `out` is untouched then. Returns true with an empty `out` when
 * it can answer and nothing matched.
 */
bool FunctionsReferencingString(const void* base, std::size_t size, const char* text,
	const std::vector<const void*>& known_starts, std::vector<const void*>& out);

/**
 * Ends the process now, without running DLL detach routines or static
 * destructors. On Windows exit()/_exit() both go through ExitProcess, which
 * unloads every module in whatever state the engine is in; engine DLLs fault
 * doing that mid-run. TerminateProcess does not.
 */
[[noreturn]] void Terminate(int code);

} // namespace proc
} // namespace cs2st

#endif /* CS2SDKTEST_CORE_PROCESS_H_ */
