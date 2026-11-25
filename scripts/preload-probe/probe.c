/*
 * LD_PRELOAD feasibility probe. Not part of the plugin build.
 *
 * The question, in parts:
 *   stage 1  does LD_PRELOAD get us into the cs2 process at all?
 *   stage 2  once the game is up, can we find the engine modules and their
 *            interfaces from a thread that is not the main thread?
 *   stage 3  which libc call sites does the engine hit *on the main thread*,
 *            often enough to drive a callback from? This one needs no vtable
 *            slot at all, so it is the safe way to get back onto the main
 *            thread and the one worth preferring if it holds up.
 *   stage 4  (opt-in) can IVEngineServer2::ServerCommand be called from a
 *            worker thread? Needs PROBE_SERVERCOMMAND_SLOT. Prints where the
 *            slot's function pointer lives before calling it, so a wrong slot
 *            is visible in the log rather than only in the crash.
 *
 * Nothing here mutates the game. Every event is appended to $PROBE_OUT
 * (default /tmp/preload-probe.log) with the pid and tid, flushed immediately,
 * so a crash still leaves everything before it.
 *
 * Build:
 *     gcc -O1 -shared -fPIC -o probe.so probe.c -ldl -lpthread
 *
 * Run: see run-probe.sh next to this file.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef void* (*CreateInterfaceFn)(const char* name, int* return_code);
typedef void (*ServerCommandFn)(void* self, const char* command);

static FILE* g_out;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec g_t0;

static long Tid(void)
{
	return (long)syscall(SYS_gettid);
}

static double Elapsed(void)
{
	/* Raw syscall on purpose: clock_gettime itself is interposed below. */
	struct timespec now;
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - g_t0.tv_sec) + (double)(now.tv_nsec - g_t0.tv_nsec) / 1e9;
}

static void Say(const char* fmt, ...)
{
	pthread_mutex_lock(&g_lock);
	if (g_out != NULL)
	{
		fprintf(g_out, "[%8.3f] pid=%ld tid=%ld ", Elapsed(), (long)getpid(), Tid());
		va_list args;
		va_start(args, fmt);
		vfprintf(g_out, fmt, args);
		va_end(args);
		fputc('\n', g_out);
		fflush(g_out);
	}
	pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------------------
 *  stage 3: is there a main-thread call site we could drive?
 *
 *  Counters only - an atomic increment per call - so the cost is negligible
 *  even for something called tens of thousands of times a second. Reported at
 *  two points so a rate can be worked out.
 * ------------------------------------------------------------------------- */

enum
{
	kClockGettime = 0,
	kGetTimeOfDay,
	kNanosleep,
	kPoll,
	kEpollWait,
	kUsleep,
	kCallSiteCount
};

static const char* const kCallSiteNames[kCallSiteCount] = {
	"clock_gettime", "gettimeofday", "nanosleep", "poll", "epoll_wait", "usleep",
};

/* [site][0] = main thread (tid == pid), [site][1] = any other thread. */
static long g_calls[kCallSiteCount][2];

/*
 * Who called, for the idle-wait style calls only. Written from the main thread
 * alone (so no lock), read by the probe thread at report time; a torn read
 * costs at worst one odd line in a diagnostic. clock_gettime is left out: it is
 * called from everywhere and would only fill the table.
 */
#define kMaxCallers 96
static struct
{
	void* addr;
	int site;
	long count;
} g_callers[kMaxCallers];
static volatile int g_caller_n;

static void RecordCaller(int site, void* caller)
{
	const int n = g_caller_n;
	for (int i = 0; i < n; ++i)
	{
		if (g_callers[i].addr == caller && g_callers[i].site == site)
		{
			g_callers[i].count++;
			return;
		}
	}
	if (n < kMaxCallers)
	{
		g_callers[n].addr = caller;
		g_callers[n].site = site;
		g_callers[n].count = 1;
		g_caller_n = n + 1;
	}
}

static void Count(int site, void* caller)
{
	const int slot = (Tid() == (long)getpid()) ? 0 : 1;
	__atomic_add_fetch(&g_calls[site][slot], 1, __ATOMIC_RELAXED);
	if (slot == 0 && site != kClockGettime && site != kGetTimeOfDay)
	{
		RecordCaller(site, caller);
	}
}

/* dlsym can allocate, and allocation can reach clock_gettime: break the loop. */
static __thread int t_in_resolve;

static void* Resolve(void** cache, const char* name)
{
	if (*cache != NULL)
	{
		return *cache;
	}
	if (t_in_resolve)
	{
		return NULL;
	}
	t_in_resolve = 1;
	*cache = dlsym(RTLD_NEXT, name);
	t_in_resolve = 0;
	return *cache;
}

/* Each wrapper forwards to the real symbol; a NULL real (should not happen) is
 * tolerated by returning a harmless default rather than faulting. */

int clock_gettime(clockid_t clk, struct timespec* ts)
{
	typedef int (*Fn)(clockid_t, struct timespec*);
	static void* cache;
	Count(kClockGettime, __builtin_return_address(0));
	Fn real = (Fn)Resolve(&cache, "clock_gettime");
	return real != NULL ? real(clk, ts) : 0;
}

int gettimeofday(struct timeval* tv, void* tz)
{
	typedef int (*Fn)(struct timeval*, void*);
	static void* cache;
	Count(kGetTimeOfDay, __builtin_return_address(0));
	Fn real = (Fn)Resolve(&cache, "gettimeofday");
	return real != NULL ? real(tv, tz) : 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem)
{
	typedef int (*Fn)(const struct timespec*, struct timespec*);
	static void* cache;
	Count(kNanosleep, __builtin_return_address(0));
	Fn real = (Fn)Resolve(&cache, "nanosleep");
	return real != NULL ? real(req, rem) : 0;
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout)
{
	typedef int (*Fn)(struct pollfd*, nfds_t, int);
	static void* cache;
	Count(kPoll, __builtin_return_address(0));
	Fn real = (Fn)Resolve(&cache, "poll");
	return real != NULL ? real(fds, nfds, timeout) : 0;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)
{
	typedef int (*Fn)(int, struct epoll_event*, int, int);
	static void* cache;
	Count(kEpollWait, __builtin_return_address(0));
	Fn real = (Fn)Resolve(&cache, "epoll_wait");
	return real != NULL ? real(epfd, events, maxevents, timeout) : 0;
}

int usleep(useconds_t usec)
{
	typedef int (*Fn)(useconds_t);
	static void* cache;
	Count(kUsleep, __builtin_return_address(0));
	Fn real = (Fn)Resolve(&cache, "usleep");
	return real != NULL ? real(usec) : 0;
}

static void ReportCallSites(const char* when)
{
	Say("%s: libc call sites, [main thread / other threads]", when);
	for (int site = 0; site < kCallSiteCount; ++site)
	{
		const long main_calls = __atomic_load_n(&g_calls[site][0], __ATOMIC_RELAXED);
		const long other_calls = __atomic_load_n(&g_calls[site][1], __ATOMIC_RELAXED);
		if (main_calls == 0 && other_calls == 0)
		{
			continue;
		}
		Say("  %-14s %10ld / %10ld%s", kCallSiteNames[site], main_calls, other_calls,
			main_calls > 0 ? "   <- usable from the main thread" : "");
	}
}

/*
 * Top main-thread callers of the idle-wait calls, as module + offset. The offset
 * is what to look up in IDA: a caller that is the same few addresses in
 * libengine2.so, at about the tick rate, is the engine's own frame loop.
 */
static void ReportCallers(const char* when)
{
	const int n = g_caller_n;
	int printed[kMaxCallers] = {0};

	Say("%s: main-thread callers of nanosleep/poll/epoll_wait/usleep, most frequent first", when);
	for (int rank = 0; rank < 12; ++rank)
	{
		int best = -1;
		for (int i = 0; i < n; ++i)
		{
			if (!printed[i] && (best < 0 || g_callers[i].count > g_callers[best].count))
			{
				best = i;
			}
		}
		if (best < 0)
		{
			break;
		}
		printed[best] = 1;

		char where[400] = "?";
		Dl_info info;
		if (dladdr(g_callers[best].addr, &info) != 0 && info.dli_fname != NULL)
		{
			const char* base = strrchr(info.dli_fname, '/');
			snprintf(where, sizeof(where), "%s+0x%lx", base != NULL ? base + 1 : info.dli_fname,
				(unsigned long)((char*)g_callers[best].addr - (char*)info.dli_fbase));
		}
		Say("  %-14s %9ld  %s", kCallSiteNames[g_callers[best].site], g_callers[best].count, where);
	}
	if (n == 0)
	{
		Say("  (none recorded)");
	}
}

/* ---------------------------------------------------------------------------
 *  stage 2: module and interface lookup, no loading
 * ------------------------------------------------------------------------- */

static void* FindLoaded(const char* file)
{
	return dlopen(file, RTLD_NOW | RTLD_NOLOAD);
}

static void* QueryIn(const char* module_file, const char* version)
{
	void* handle = FindLoaded(module_file);
	if (handle == NULL)
	{
		Say("  %s: not mapped", module_file);
		return NULL;
	}
	CreateInterfaceFn create = (CreateInterfaceFn)dlsym(handle, "CreateInterface");
	if (create == NULL)
	{
		Say("  %s: mapped, no CreateInterface export", module_file);
		return NULL;
	}
	int rc = 0;
	void* iface = create(version, &rc);
	Say("  %s: CreateInterface(\"%s\") -> %p (rc=%d)", module_file, version, iface, rc);
	return iface;
}

/* ---------------------------------------------------------------------------
 *  stage 4: ServerCommand from a worker thread (opt-in)
 *
 *  The slot is not guessed: it comes from PROBE_SERVERCOMMAND_SLOT, and before
 *  calling, dladdr reports which module the pointer belongs to. A slot that
 *  lands outside libengine2.so is the signature of a stale SDK declaration.
 * ------------------------------------------------------------------------- */

static void TryServerCommand(void* engine)
{
	const char* slot_text = getenv("PROBE_SERVERCOMMAND_SLOT");
	if (engine == NULL || slot_text == NULL || slot_text[0] == '\0')
	{
		Say("  ServerCommand: skipped (%s)",
			engine == NULL ? "no IVEngineServer2" : "PROBE_SERVERCOMMAND_SLOT not set");
		return;
	}

	const int slot = atoi(slot_text);
	void** vtable = *(void***)engine;
	void* fn = vtable[slot];

	char path[512] = "?";
	Dl_info info;
	if (dladdr(fn, &info) != 0 && info.dli_fname != NULL)
	{
		snprintf(path, sizeof(path), "%s", info.dli_fname);
	}
	Say("  ServerCommand: slot %d -> %p in %s", slot, fn, path);

	if (strstr(path, "libengine2") == NULL)
	{
		Say("  ServerCommand: NOT calling - the pointer is not inside libengine2.so, "
		    "so the slot number is wrong");
		return;
	}

	Say("  ServerCommand: calling from this thread");
	((ServerCommandFn)fn)(engine, "echo PROBE_SERVERCOMMAND_OK\n");
	Say("  ServerCommand: returned");
}

/* ---------------------------------------------------------------------------
 *  main
 * ------------------------------------------------------------------------- */

static void* ProbeMain(void* unused)
{
	(void)unused;

	int delay = 30;
	const char* delay_text = getenv("PROBE_DELAY");
	if (delay_text != NULL && delay_text[0] != '\0')
	{
		delay = atoi(delay_text);
	}

	Say("probe thread started, polling for modules for %ds", delay);
	int engine_seen = 0, server_seen = 0;
	for (int waited = 0; waited < delay; ++waited)
	{
		if (!engine_seen && FindLoaded("libengine2.so") != NULL)
		{
			engine_seen = 1;
			Say("libengine2.so became mapped after %ds", waited);
		}
		if (!server_seen && FindLoaded("libserver.so") != NULL)
		{
			server_seen = 1;
			Say("libserver.so became mapped after %ds", waited);
			/* One reading once the game is up, a second one after it has run. */
			ReportCallSites("call sites at map start");
		}
		sleep(1);
	}

	Say("lookup after the delay:");
	void* engine = QueryIn("libengine2.so", "Source2EngineToServer001");
	QueryIn("libserver.so", "Source2Server001");
	TryServerCommand(engine);
	ReportCallSites("call sites at the end of the wait");
	ReportCallers("callers at the end of the wait");

	Say("probe finished; process left running");
	return NULL;
}

__attribute__((constructor)) static void ProbeInit(void)
{
	clock_gettime(CLOCK_MONOTONIC, &g_t0);

	char exe[512] = "?";
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0)
	{
		exe[n] = '\0';
	}

	/* The preload is inherited by whatever cs2 spawns (dash, uname, ...). Those are
	 * not what is being measured, so only the cs2 process opens the log and starts
	 * the probe thread; the others carry the wrappers but stay silent. */
	const size_t len = strlen(exe);
	if (len < 4 || strcmp(exe + len - 4, "/cs2") != 0)
	{
		return;
	}

	const char* path = getenv("PROBE_OUT");
	g_out = fopen(path != NULL && path[0] != '\0' ? path : "/tmp/preload-probe.log", "a");

	/* tid == pid means this is the process's first thread, which is the engine's main thread. */
	Say("constructor ran in exe=%s main_thread=%s", exe,
		Tid() == (long)getpid() ? "yes" : "no");

	pthread_t thread;
	if (pthread_create(&thread, NULL, ProbeMain, NULL) == 0)
	{
		pthread_detach(thread);
	}
	else
	{
		Say("pthread_create failed");
	}
}
