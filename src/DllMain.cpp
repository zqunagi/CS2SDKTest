/**
 * Windows entry point.
 *
 * cst_run.exe creates cs2.exe suspended and has Microsoft Detours add this DLL
 * to the process's import table (DetourCreateProcessWithDllEx). The loader then
 * maps it, and runs DllMain, before cs2.exe's own entry point: the equivalent of
 * LD_PRELOAD.
 *
 * DllMain runs under the loader lock, so it does what the Linux constructor does
 * and no more: read the environment, open the log, start one thread. Anything
 * that loads a module or waits on another thread has to stay out of this path;
 * the worker thread does that, after the lock is released.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Entry.h"

BOOL APIENTRY DllMain(HINSTANCE module, DWORD reason, LPVOID /*reserved*/)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		// No per-thread work to do, so the loader need not tell us about threads.
		::DisableThreadLibraryCalls(module);
		cs2st::Start();
	}
	return TRUE;
}

// Detours adds this DLL to the target's import table by ordinal 1, so the
// export has to exist or the loader refuses to start the process. It is also
// the entry point Detours' helper process calls when the target has the other
// bitness; both are x64 here, so nothing ever calls it. Exported by number in
// exports.def, not by name.
extern "C" void CALLBACK CstDetoursOrdinal1(HWND, HINSTANCE, LPSTR, int)
{
}
