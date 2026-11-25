/**
 * The few process-level operations the entry point needs that differ between
 * Linux and Windows. Kept out of Entry.cpp for the same reason Platform.h exists:
 * Entry.cpp includes SDK headers, and <Windows.h> does not mix with those.
 */

#ifndef CS2SDKTEST_CORE_PROCESS_H_
#define CS2SDKTEST_CORE_PROCESS_H_

#include <cstddef>

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
 * Size in bytes of the function that starts exactly at `fn`, taken from the
 * image's own unwind tables (.pdata on Windows x64), or 0 when it is not known.
 * Linux returns 0: its binaries carry no per-function size without symbols.
 */
std::size_t FunctionSize(const void* fn);

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
