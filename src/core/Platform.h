/**
 * Platform detection, kept deliberately independent of hl2sdk's own
 * public/tier0/platform.h.
 *
 * This header is included by code that also touches OS APIs directly (module
 * enumeration, file I/O). Mixing the SDK's platform macros with <Windows.h> in
 * the same translation unit is a reliable way to produce unreadable errors, so
 * the two are kept apart: SDK headers stay on the SDK side, OS headers stay
 * inside the .cpp files that need them, and this header is the neutral ground.
 */

#ifndef CS2SDKTEST_CORE_PLATFORM_H_
#define CS2SDKTEST_CORE_PLATFORM_H_

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) || defined(_WIN64)
	#define CS2ST_WINDOWS 1
	#define CS2ST_POSIX   0
	#define CS2ST_PLATFORM_NAME "windows"
	#define CS2ST_LIB_PREFIX    ""
	#define CS2ST_LIB_SUFFIX    ".dll"
	#define CS2ST_PATH_SEP      '\\'
	#define CS2ST_PATH_SEP_STR  "\\"
#elif defined(__linux__)
	#define CS2ST_WINDOWS 0
	#define CS2ST_POSIX   1
	#define CS2ST_PLATFORM_NAME "linux"
	#define CS2ST_LIB_PREFIX    "lib"
	#define CS2ST_LIB_SUFFIX    ".so"
	#define CS2ST_PATH_SEP      '/'
	#define CS2ST_PATH_SEP_STR  "/"
#else
	#error "CS2SDKTest supports Windows and Linux only (Source 2 ships nothing else)."
#endif

#if !defined(__x86_64__) && !defined(_M_X64)
	#error "Source 2 is x86_64 only."
#endif

/* printf-style argument checking, so a format/argument mismatch in a log call
 * is a compile error rather than a garbage line in a diagnostic report. */
#if defined(__GNUC__) || defined(__clang__)
	#define CS2ST_PRINTF_FMT(fmt_index, first_arg) \
		__attribute__((format(printf, fmt_index, first_arg)))
#else
	#define CS2ST_PRINTF_FMT(fmt_index, first_arg)
#endif

namespace cs2st
{

/** Opaque handle to a shared library already mapped into this process. */
using ModuleHandle = void*;

/** Signature of the exported CreateInterface every Source module provides. */
using CreateInterfaceFnRaw = void* (*)(const char* name, int* return_code);

/** Return codes CreateInterface uses (public/tier0/interface.h, IFACE_OK). */
enum : int
{
	kInterfaceOk     = 0,
	kInterfaceFailed = 1,
};

} // namespace cs2st

#endif /* CS2SDKTEST_CORE_PLATFORM_H_ */
