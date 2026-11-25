/*
 * Prototype: LD_PRELOAD, no Metamod, hook ISource2Server::GameFrame by patching
 * its vtable slot, with the slot found by evidence instead of by SDK index.
 *
 *   1. wait until libserver.so is mapped, CreateInterface("Source2Server001")
 *   2. vtable = *(void***)iface        (no RTTI/symbol lookup needed)
 *   3. a slot is "GameFrame" when its function contains a RIP-relative lea
 *      whose target is the C string "GameFrame" (the VPROF scope name).
 *      Exactly one slot must match, otherwise nothing is patched.
 *   4. mprotect the page, swap the slot for our function, which calls the
 *      original and, once the delay has passed, logs FIRE (the place where the
 *      real plugin would run the call sweep) exactly once.
 *
 * Nothing is called into the engine except the original GameFrame. If any
 * check fails the log says which one and the process is left alone.
 *
 * Build: gcc -O1 -shared -fPIC -o probe2.so probe2.c -ldl -lpthread
 * Env:   PROBE_OUT (log file), PROBE_DELAY (seconds after the hook, default 20)
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef void* (*CreateInterfaceFn)(const char*, int*);
typedef void (*GameFrameFn)(void* self, bool simulating, bool first_tick, bool last_tick);

static FILE* g_out;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec g_t0;
static GameFrameFn g_original;
static volatile int g_frames;
static volatile int g_fired;
static double g_hooked_at;
static int g_delay = 20;

static long Tid(void) { return (long)syscall(SYS_gettid); }

static double Elapsed(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - g_t0.tv_sec) + (double)(now.tv_nsec - g_t0.tv_nsec) / 1e9;
}

static void Say(const char* fmt, ...)
{
	pthread_mutex_lock(&g_lock);
	if (g_out != NULL)
	{
		fprintf(g_out, "[%8.3f] tid=%ld ", Elapsed(), Tid());
		va_list args;
		va_start(args, fmt);
		vfprintf(g_out, fmt, args);
		va_end(args);
		fputc('\n', g_out);
		fflush(g_out);
	}
	pthread_mutex_unlock(&g_lock);
}

/* ---- the replacement slot, runs on the engine's main thread ---------------- */

static void HookedGameFrame(void* self, bool simulating, bool first_tick, bool last_tick)
{
	const int n = ++g_frames;
	if (n == 1)
	{
		Say("first GameFrame through the hook: main_thread=%s simulating=%d",
			Tid() == (long)getpid() ? "yes" : "no", simulating ? 1 : 0);
	}
	if (!g_fired && simulating && Elapsed() - g_hooked_at >= g_delay)
	{
		g_fired = 1;
		Say("FIRE: %d frames seen, main_thread=%s - the sweep would run here",
			n, Tid() == (long)getpid() ? "yes" : "no");
	}
	g_original(self, simulating, first_tick, last_tick);
}

/* ---- slot identification --------------------------------------------------- */

/* Does the code at fn contain `lea r64, [rip+disp32]` pointing at "GameFrame"? */
static int ReferencesGameFrameString(void* fn, void* module_base)
{
	const unsigned char* code = (const unsigned char*)fn;
	for (int i = 0; i < 0x300; ++i)
	{
		if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0x8D && (code[i + 2] & 0xC7) == 0x05)
		{
			int disp;
			memcpy(&disp, code + i + 3, 4);
			const char* target = (const char*)(code + i + 7) + disp;
			Dl_info info;
			if (dladdr(target, &info) != 0 && info.dli_fbase == module_base &&
			    memcmp(target, "GameFrame", 10) == 0)
			{
				return 1;
			}
		}
	}
	return 0;
}

static void* FindLoaded(const char* file) { return dlopen(file, RTLD_NOW | RTLD_NOLOAD); }

static int TryHook(void)
{
	void* handle = FindLoaded("libserver.so");
	if (handle == NULL)
	{
		return 0; /* not yet */
	}
	CreateInterfaceFn create = (CreateInterfaceFn)dlsym(handle, "CreateInterface");
	if (create == NULL)
	{
		Say("libserver.so has no CreateInterface export; giving up");
		return -1;
	}
	int rc = 0;
	void* iface = create("Source2Server001", &rc);
	if (iface == NULL)
	{
		Say("CreateInterface(Source2Server001) -> NULL (rc=%d); giving up", rc);
		return -1;
	}
	void** vtable = *(void***)iface;
	Dl_info mod;
	if (dladdr(vtable[0], &mod) == 0)
	{
		Say("slot 0 is not inside a known module; giving up");
		return -1;
	}
	Say("iface=%p vtable=%p module=%s", iface, (void*)vtable, mod.dli_fname);

	int match = -1, matches = 0;
	for (int i = 0; i < 100; ++i)
	{
		Dl_info fi;
		if (dladdr(vtable[i], &fi) == 0 || fi.dli_fbase != mod.dli_fbase)
		{
			break; /* ran off the end of this class's vtable */
		}
		if (ReferencesGameFrameString(vtable[i], mod.dli_fbase))
		{
			Say("  slot %d (%p) references \"GameFrame\"", i, vtable[i]);
			match = i;
			++matches;
		}
	}
	if (matches != 1)
	{
		Say("%d slot(s) matched, need exactly 1; NOT patching", matches);
		return -1;
	}

	void** slot = &vtable[match];
	const long page = sysconf(_SC_PAGESIZE);
	void* page_start = (void*)((unsigned long)slot & ~(unsigned long)(page - 1));
	if (mprotect(page_start, (size_t)page, PROT_READ | PROT_WRITE) != 0)
	{
		Say("mprotect failed; NOT patching");
		return -1;
	}
	g_original = (GameFrameFn)*slot;
	g_hooked_at = Elapsed();
	__atomic_store_n(slot, (void*)HookedGameFrame, __ATOMIC_SEQ_CST);
	mprotect(page_start, (size_t)page, PROT_READ);
	Say("patched slot %d: %p -> %p", match, (void*)g_original, (void*)HookedGameFrame);
	return 1;
}

static void* ProbeMain(void* unused)
{
	(void)unused;
	for (int waited = 0; waited < 120; ++waited)
	{
		const int r = TryHook();
		if (r != 0)
		{
			if (r > 0)
			{
				sleep((unsigned)g_delay + 15);
				Say("summary: %d frames through the hook, fired=%d", g_frames, g_fired);
			}
			return NULL;
		}
		sleep(1);
	}
	Say("libserver.so never appeared in 120s");
	return NULL;
}

__attribute__((constructor)) static void Init(void)
{
	clock_gettime(CLOCK_MONOTONIC, &g_t0);
	char exe[512] = "?";
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0)
	{
		exe[n] = '\0';
	}
	const size_t len = strlen(exe);
	if (len < 4 || strcmp(exe + len - 4, "/cs2") != 0)
	{
		return;
	}
	const char* path = getenv("PROBE_OUT");
	g_out = fopen(path != NULL && path[0] != '\0' ? path : "/tmp/preload-probe2.log", "a");
	const char* d = getenv("PROBE_DELAY");
	if (d != NULL && d[0] != '\0')
	{
		g_delay = atoi(d);
	}
	Say("constructor in cs2, delay=%ds", g_delay);
	pthread_t t;
	if (pthread_create(&t, NULL, ProbeMain, NULL) == 0)
	{
		pthread_detach(t);
	}
}
