#ifndef _WIN32
#define _GNU_SOURCE /* ucontext register names, pthread names, environ */
#endif
#include "text_input.h"
/* The Windows calls of the native PS2 system (hle.h), apart from its code because the PS2
   kernel's names (CreateThread, ExitThread ...) are Windows' too. */
#include "hle.h"
#include "timer.h"
#include "state_trace.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>        /* _get_osfhandle */
#include <commdlg.h>  /* the file-open dialog's struct; comdlg32 itself is loaded when needed */
#else
#include "../host/win32_compat.h"
#include "../host/backtrace_posix.h"  /* <execinfo.h>, or Android's own (API < 33) */
#include <dirent.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "ps2asm.h"
#include "../include/guest.h"

/* The instruction pointer, stack pointer and frame register of a CONTEXT (the 32-bit and the 64-bit
   records name them differently). */
#ifdef _WIN32
#ifdef _WIN64
#define CTX_IP(c) ((uintptr_t)(c)->Rip)
#define CTX_SP(c) ((uintptr_t)(c)->Rsp)
#else
#define CTX_IP(c) ((uintptr_t)(c)->Eip)
#define CTX_SP(c) ((uintptr_t)(c)->Esp)
#endif
#else
/* Linux crash/dump signals hand a ucontext_t: x86_64, or aarch64 (Android, finding 091). */
#if defined(__aarch64__)
#define CTX_IP(c) ((uintptr_t)(c)->uc_mcontext.pc)
#define CTX_SP(c) ((uintptr_t)(c)->uc_mcontext.sp)
#else
#define CTX_IP(c) ((uintptr_t)(c)->uc_mcontext.gregs[REG_RIP])
#define CTX_SP(c) ((uintptr_t)(c)->uc_mcontext.gregs[REG_RSP])
#endif
typedef ucontext_t CONTEXT;
#endif

/* Where the game thread is: its registers, the code addresses on its stack (no frame
   pointers, so likely return addresses, some stale; port/tools/host64_rva.py names them from
   dhd64.map, after subtracting the image base) and the
   recompiled code's registers. */
typedef struct {
    CONTEXT context;
    uintptr_t returns[32];  /* host code addresses (the return addresses on the stack) */
    uint32_t registers[5];  /* the game's registers: guest values */
    unsigned count;
} Snapshot;

/* No CRT calls while the game thread is suspended: it may own a CRT lock. */
static uint32_t published_registers[5];
#ifndef _WIN32
/* Whether [p, p + n) can be read (peek, safe_vprint, snapshot): a fault guard
   around a volatile probe. The previous SIGSEGV/SIGBUS handlers stay in the
   chain for faults outside the probe (or on other threads). */
static _Thread_local sigjmp_buf probe_env;
static _Thread_local volatile int probe_armed;
static struct sigaction probe_old_segv, probe_old_bus;
static void probe_fault(int sig, siginfo_t *info, void *ctx)
{
    struct sigaction *old = sig == SIGSEGV ? &probe_old_segv : &probe_old_bus;
    (void)info;
    (void)ctx;
    if (probe_armed) siglongjmp(probe_env, 1);
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) old->sa_sigaction(sig, info, ctx);
        else {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    } else if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) {
        signal(sig, old->sa_handler);
        raise(sig);
    } else if (old->sa_handler) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}
static int host_readable(const void *p, size_t n)
{
    struct sigaction sa;
    volatile const char *q = p;
    volatile char sink;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = probe_fault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, &probe_old_segv) < 0 || sigaction(SIGBUS, &sa, &probe_old_bus) < 0) return 0;
    probe_armed = 1;
    if (sigsetjmp(probe_env, 1)) {
        probe_armed = 0;
        sigaction(SIGSEGV, &probe_old_segv, NULL);
        sigaction(SIGBUS, &probe_old_bus, NULL);
        return 0;
    }
    for (size_t i = 0; i < n; i++) sink = q[i];
    (void)sink;
    probe_armed = 0;
    sigaction(SIGSEGV, &probe_old_segv, NULL);
    sigaction(SIGBUS, &probe_old_bus, NULL);
    return 1;
}
#endif
static void snapshot(Snapshot *out, const CONTEXT *c)
{
    out->context = *c;
    out->count = 0;
#ifdef _WIN32
    extern char __ImageBase[];
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(__ImageBase + ((IMAGE_DOS_HEADER *)__ImageBase)->e_lfanew);
    uintptr_t lo = (uintptr_t)__ImageBase + nt->OptionalHeader.BaseOfCode;
    uintptr_t hi = lo + nt->OptionalHeader.SizeOfCode;
    const uintptr_t *sp = (const uintptr_t *)CTX_SP(c);
    for (unsigned i = 0; i < 2048 && out->count < 32; i++) {
        uintptr_t value;
        SIZE_T read;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &value, sizeof value, &read) || read != sizeof value) break;
        if (value >= lo && value < hi) out->returns[out->count++] = value;
    }
#else
    uintptr_t lo = 0, hi = 0;
    const uintptr_t *sp = (const uintptr_t *)CTX_SP(c);
    host_code_range(&lo, &hi);
    for (unsigned i = 0; i < 2048 && out->count < 32; i++) {
        uintptr_t value;
        if (!host_readable(sp + i, sizeof value)) break;
        value = *(volatile const uintptr_t *)(sp + i);
        if (value >= lo && value < hi) out->returns[out->count++] = value;
    }
#endif
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    memcpy(out->registers,published_registers,sizeof out->registers);
#else
    out->registers[0] = ps2.gpr[29].w[0]; out->registers[1] = ps2.gpr[31].w[0];
    out->registers[2] = ps2.gpr[4].w[0]; out->registers[3] = ps2.gpr[5].w[0];
    out->registers[4] = ps2.gpr[2].w[0];
#endif
}

static void print_snapshot(const Snapshot *s)
{
    const CONTEXT *c = &s->context;
#ifdef _WIN32
#ifdef _WIN64
    fprintf(stderr, "  rip %016llX rax %016llX rbx %016llX rcx %016llX rdx %016llX rsi %016llX rdi %016llX rsp %016llX\n",
            (unsigned long long)c->Rip, (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
            (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->Rsi,
            (unsigned long long)c->Rdi, (unsigned long long)c->Rsp);
#else
    fprintf(stderr, "  eip %08lX eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX esp %08lX\n",
            c->Eip, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp);
#endif
#elif defined(__aarch64__)
    const unsigned long long *x = (const unsigned long long *)c->uc_mcontext.regs;
    fprintf(stderr, "  pc %016llX sp %016llX lr %016llX x0 %016llX x1 %016llX x2 %016llX x19 %016llX x29 %016llX\n",
            (unsigned long long)c->uc_mcontext.pc, (unsigned long long)c->uc_mcontext.sp, x[30], x[0], x[1], x[2],
            x[19], x[29]);
#else
    const greg_t *r = c->uc_mcontext.gregs;
    fprintf(stderr, "  rip %016llX rax %016llX rbx %016llX rcx %016llX rdx %016llX rsi %016llX rdi %016llX rsp %016llX\n",
            (unsigned long long)r[REG_RIP], (unsigned long long)r[REG_RAX], (unsigned long long)r[REG_RBX],
            (unsigned long long)r[REG_RCX], (unsigned long long)r[REG_RDX], (unsigned long long)r[REG_RSI],
            (unsigned long long)r[REG_RDI], (unsigned long long)r[REG_RSP]);
#endif
    fprintf(stderr, "  code addresses on the stack:");
    for (unsigned i = 0; i < s->count; i++)
        fprintf(stderr, "%s%08llX", i % 8 ? " " : "\n    ", (unsigned long long)s->returns[i]);
    fprintf(stderr, "\n  ps2: sp %08X ra %08X a0 %08X a1 %08X v0 %08X\n",
            s->registers[0], s->registers[1], s->registers[2], s->registers[3], s->registers[4]);
    fflush(stderr);
}

static void report(const CONTEXT *c)
{
    Snapshot s;
    snapshot(&s, c);
    print_snapshot(&s);
}

/* DHD_PEEK=address,bytes (host, hex): memory too, after a dump or a crash report. */
static void peek(void)
{
    const char *peek = getenv("DHD_PEEK");
    unsigned addr, len;
    if (peek && sscanf(peek, "%x,%x", &addr, &len) == 2)
        for (unsigned i = 0; i < len; i += 4) {
#ifdef _WIN32
            if (IsBadReadPtr(GUEST(addr + i), 4)) break;
#else
            if (!host_readable(GUEST(addr + i), 4)) break;
#endif
            fprintf(stderr, "%s%08X", i % 32 ? " " : i ? "\n  " : "  ", *(uint32_t *)GUEST(addr + i));
        }
    fprintf(stderr, "\n");
}

#ifdef _WIN32
static LONG WINAPI crashed(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    fprintf(stderr, "crash: exception %08lX at %08llX", r->ExceptionCode,
            (unsigned long long)(uintptr_t)r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, " (%s %08llX)", r->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)r->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    report(e->ContextRecord);
    peek();
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
/* Fatal signals end the process with the same words the Windows crash report has. */
static void crashed(int sig, siginfo_t *info, void *ctx)
{
    const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : "SIGFPE";
    char thread[17] = "?";
    prctl(PR_GET_NAME, thread);  /* which of the port's threads (host_thread_name) */
    fprintf(stderr, "crash: %s at %016llX in thread %s\n", name,
            (unsigned long long)(uintptr_t)(info ? info->si_addr : NULL), thread);
    fflush(stderr);
    report((const CONTEXT *)ctx);
    {
        void *frames[32];
        int n = backtrace(frames, 32);
        backtrace_symbols_fd(frames, n, STDERR_FILENO);
    }
    peek();
    fflush(stderr);
    _exit(3);
}
#endif

void host_backtrace(void)
{
#ifdef _WIN32
    CONTEXT c = {0};
#ifdef _WIN64
    c.Rip = (DWORD64)(uintptr_t)__builtin_return_address(0);
    c.Rsp = (DWORD64)(uintptr_t)__builtin_frame_address(0);
#else
    c.Eip = (DWORD)(uintptr_t)__builtin_return_address(0);
    c.Esp = (DWORD)(uintptr_t)__builtin_frame_address(0);
#endif
    report(&c);
#else
    void *frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
#endif
}

/* DHD_DUMP=N: after N seconds, where the game thread is (then it goes on). */
#ifdef _WIN32
static HANDLE game_thread;
#else
static pthread_t game_thread;
static int game_thread_set;
#endif

/* DHD_WATCH=address[:rw],... (EE addresses, hex, up to 4): hardware data watchpoints on the
   game thread (all its fibers), on stores, or with :rw on loads too. Each hit (the first 40 per
   watchpoint) prints the word, the code address after the access and the code addresses on the
   stack, for port/tools/host64_rva.py (as RVAs). A diagnostic (finding 041: host addresses in EE memory). */
static uint32_t watch_address[4];
static LONG watch_hits[4];

#ifdef _WIN32
static LONG WINAPI watch_hit(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 15)) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; i++)
        if (c->Dr6 & 1u << i && InterlockedIncrement(&watch_hits[i]) <= 40) {
            extern uint64_t kernel_vsync_count;
            uint32_t value = 0;
            SIZE_T got;  /* a kernel read: a plain load would hit a load watchpoint again */
            ReadProcessMemory(GetCurrentProcess(), GUEST(0x08000000u + watch_address[i]), &value, 4, &got);
            extern char __ImageBase[];
            fprintf(stderr, "watch: EE %08X = %08X field %llu after eip %08llX rva %08llX (map)\n",
                    watch_address[i], value, (unsigned long long)kernel_vsync_count,
                    (unsigned long long)CTX_IP(c),
                    (unsigned long long)((uintptr_t)CTX_IP(c) - (uintptr_t)__ImageBase));
            report(c);
        }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI watch_arm(void *unused)
{
    CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
    const char *spec = getenv("DHD_WATCH");
    if (!spec) spec = state_trace_watch_spec();  /* DHD_STATE_WATCH (state_trace.c) */
    (void)unused;
    if (SuspendThread(game_thread) == (DWORD)-1) return 0;
    GetThreadContext(game_thread, &c);
    for (int i = 0; i < 4 && spec && *spec; i++) {
        char *end;
        watch_address[i] = (uint32_t)strtoul(spec, &end, 16) & ~3u;
        int rw = !strncmp(end, ":rw", 3);
        (&c.Dr0)[i] = (DWORD_PTR)GUEST(0x08000000u + watch_address[i]);  /* the host address of the EE word */
        c.Dr7 |= 1u << (2 * i) | (rw ? 3u : 1u) << (16 + 4 * i) | 3u << (18 + 4 * i);  /* local, 4 bytes */
        fprintf(stderr, "watch: EE %08X on %s\n", watch_address[i], rw ? "loads and stores" : "stores");
        spec = strchr(end, ',');
        spec = spec ? spec + 1 : NULL;
    }
    SetThreadContext(game_thread, &c);
    ResumeThread(game_thread);
    return 0;
}
#else
/* Hardware data watchpoints need another thread's debug registers; the
   state-trace watch (DHD_STATE_WATCH, state_trace.c) is the portable one. */
static DWORD watch_arm(void *unused)
{
    const char *spec = getenv("DHD_WATCH");
    if (!spec) spec = state_trace_watch_spec();
    (void)unused;
    (void)spec;
    fprintf(stderr, "watch: hardware watchpoints need Windows; use DHD_STATE_WATCH\n");
    return 0;
}
#endif
static volatile LONG dump_pending;

/* Kernel details are printed by the game thread at its next safe boundary. */
#ifdef _WIN32
static DWORD WINAPI watchdog(void *seconds)
{
    Sleep((DWORD)(uintptr_t)seconds * 1000);
    Snapshot s;
    CONTEXT c = {.ContextFlags = CONTEXT_FULL};
    if (SuspendThread(game_thread) == (DWORD)-1) return 0;
    BOOL ok = GetThreadContext(game_thread, &c);
    if (ok) snapshot(&s, &c);
    ResumeThread(game_thread);
    if (!ok) return 0;
    fprintf(stderr, "dump after %u s:\n", (unsigned)(uintptr_t)seconds);
    print_snapshot(&s);
    peek();
    InterlockedExchange(&dump_pending, 1);
    return 0;
}
#else
/* The game thread reports itself in a SIGUSR1 handler (no cross-thread
   register read exists here); a watchdog thread only rings the bell. */
static void dump_signal(int sig, siginfo_t *info, void *ctx)
{
    Snapshot s;
    (void)sig;
    (void)info;
    snapshot(&s, (const CONTEXT *)ctx);
    fprintf(stderr, "dump:\n");
    print_snapshot(&s);
    peek();
    InterlockedExchange(&dump_pending, 1);
}
static DWORD WINAPI watchdog(void *seconds)
{
    Sleep((DWORD)(uintptr_t)seconds * 1000);
    if (game_thread_set) pthread_kill(game_thread, SIGUSR1);
    return 0;
}
#endif

/* The port's own OS threads by name (game, vu, gs, native, sound): the bench reports each one's
   CPU time. */
enum { NAMED_THREADS = 16 };
#ifdef _WIN32
static struct { const char *name; HANDLE handle; } named[NAMED_THREADS];
#else
static struct { const char *name; pthread_t handle; } named[NAMED_THREADS];
#endif
static volatile LONG named_count;

void host_thread_name(const char *name)
{
#ifdef _WIN32  /* the OS's name too, for debuggers and profilers */
    WCHAR wide[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64)) SetThreadDescription(GetCurrentThread(), wide);
#else
    pthread_setname_np(pthread_self(), name);
#endif
#ifdef __ANDROID__
    { void android_perf_thread(const char *name); android_perf_thread(name); }  /* port/host/android_perf.c */
#endif
    LONG at = InterlockedIncrement(&named_count) - 1;
    if (at >= NAMED_THREADS) return;
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &named[at].handle, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
#else
    named[at].handle = pthread_self();
#endif
    named[at].name = name;  /* published last: a reader skips a slot still being filled */
}

unsigned host_thread_cpu(const char **names, double *seconds, unsigned max)
{
    unsigned n = 0;
    for (LONG i = 0; i < named_count && i < NAMED_THREADS && n < max; i++) {
#ifdef _WIN32
        FILETIME created, exited, kernel, user;
        if (!named[i].name || !GetThreadTimes(named[i].handle, &created, &exited, &kernel, &user)) continue;
        names[n] = named[i].name;
        seconds[n++] = (((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                        ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 1e-7;
#else
        clockid_t id;
        struct timespec t;
        if (!named[i].name || pthread_getcpuclockid(named[i].handle, &id) != 0 ||
            clock_gettime(id, &t) != 0)
            continue;
        names[n] = named[i].name;
        seconds[n++] = (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
    }
    return n;
}

/* Raw scheduled CPU cycles, complementary to GetThreadTimes (coarse on Windows). */
unsigned host_thread_cycles(const char **names, uint64_t *cycles, unsigned max)
{
    unsigned n = 0;
#ifdef _WIN32
    for (LONG i = 0; i < named_count && i < NAMED_THREADS && n < max; i++) {
        ULONG64 value;
        if (!named[i].name || !QueryThreadCycleTime(named[i].handle, &value)) continue;
        names[n] = named[i].name;
        cycles[n++] = value;
    }
#else
    (void)names; (void)cycles; (void)max;
#endif
    return n;
}

void host_init(void)
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashed);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char *dump = getenv("DHD_DUMP");
    host_thread_name("game");
    if (getenv("DHD_WATCH") || state_trace_watch_spec()) {
        HANDLE armer;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        AddVectoredExceptionHandler(1, watch_hit);
        armer = CreateThread(NULL, 0, watch_arm, NULL, 0, NULL);
        WaitForSingleObject(armer, INFINITE);
        CloseHandle(armer);
    }
    if (dump) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)atoi(dump), 0, NULL);
    }
#else
    struct sigaction sa, dump_sa;
    const char *dump = getenv("DHD_DUMP");
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crashed;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    game_thread = pthread_self();
    game_thread_set = 1;
    host_thread_name("game");
    /* Snapshot helpers read the executable's code range; warm it outside any handler. */
    {
        uintptr_t lo, hi;
        host_code_range(&lo, &hi);
    }
    if (getenv("DHD_WATCH") || state_trace_watch_spec()) {
        HANDLE armer = CreateThread(NULL, 0, watch_arm, NULL, 0, NULL);
        if (armer) {
            WaitForSingleObject(armer, INFINITE);
            CloseHandle(armer);
        }
    }
    if (dump) {
        memset(&dump_sa, 0, sizeof dump_sa);
        dump_sa.sa_sigaction = dump_signal;
        sigemptyset(&dump_sa.sa_mask);
        dump_sa.sa_flags = SA_SIGINFO;
        sigaction(SIGUSR1, &dump_sa, NULL);
        CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)atoi(dump), 0, NULL);
    }
#endif
}

/* dhd.exe is a Windows program (/subsystem:windows), so a double-click opens no console window.
   Its log (stderr, stdout) goes where the starter put it: the tools' files and pipes as before;
   started from a console window, that console; else (Explorer) dhd.log beside dhd.exe, which the
   relaunched process appends to. A start with the launcher beside dhd.exe (not by the launcher)
   appends too: it most likely only opens the launcher (update.c start_launcher), and the last
   game's log, the one players attach to a report, stays. Before anything prints. */
void host_console_init(void)
{
#ifdef _WIN32
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stderr);
        freopen("CONOUT$", "w", stdout);
    } else {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
        while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
        snprintf(path + n, sizeof path - n, "DownhillRevived.exe");
        int append = getenv("DHD_RELAUNCHED") != NULL;
        if (!append && !getenv("DHD_LAUNCHER") && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            WIN32_FILE_ATTRIBUTE_DATA log;
            snprintf(path + n, sizeof path - n, "dhd.log");
            append = !GetFileAttributesExA(path, GetFileExInfoStandard, &log) ||
                     (!log.nFileSizeHigh && log.nFileSizeLow < (1u << 20));  /* launcher = off: kept under 1 MB */
        }
        snprintf(path + n, sizeof path - n, "dhd.log");
        if (!freopen(path, append ? "a" : "w", stderr)) return;
        freopen(path, "a", stdout);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
#else
    /* A console (or the tools' pipes) owns the log already; keep it unbuffered. */
    setvbuf(stderr, NULL, _IONBF, 0);
#endif
}

/* ---- controllers ---- */
#ifdef _WIN32
#pragma comment(lib, "user32")

typedef struct {  /* XINPUT_STATE */
    DWORD packet;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
} XState;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XState *);
typedef struct { WORD left, right; } XVibration;  /* XINPUT_VIBRATION: low- and high-frequency motor */
typedef DWORD(WINAPI *XInputSetStateFn)(DWORD, XVibration *);
static XInputSetStateFn xinput_set;

static XInputGetStateFn xinput(void)
{
    static int tried;
    static XInputGetStateFn fn;
    if (!tried++) {
        HMODULE dll = LoadLibraryA("xinput1_4.dll");
        if (!dll)
            dll = LoadLibraryA("xinput9_1_0.dll");
        if (dll) {
            fn = (XInputGetStateFn)(void *)GetProcAddress(dll, "XInputGetState");
            xinput_set = (XInputSetStateFn)(void *)GetProcAddress(dll, "XInputSetState");
        }
    }
    return fn;
}

/* The DualShock 2's motors (pad.c, scePadSetActDirect): the small one on or off, the large one
   0-255. A pad gets them as its high- and low-frequency motors (SDL: every pad with motors,
   gamepad.c; without SDL, XInput pads only), unless DHD_VIBRATION=0 (dhd.ini "vibration", the
   PC settings page). Output only: nothing comes back into the game. */
void host_pad_vibrate(int port, int light, int heavy)
{
    static int last[4] = {-1, -1, -1, -1};
    const char *setting = getenv("DHD_VIBRATION");
    if (setting && !atoi(setting)) light = heavy = 0;
    int key = (light ? 256 : 0) | (heavy & 255);
    if (port < 0 || port > 3 || key == last[port]) return;
    last[port] = key;
    if (gamepad_start()) {
        gamepad_vibrate(port, light, heavy);
        return;
    }
    xinput();
    if (!xinput_set) return;
    XVibration v = {(WORD)((heavy & 255) * 257), (WORD)(light ? 0xFFFF : 0)};
    xinput_set((DWORD)port, &v);
}



/* Sony controllers (DualSense, DualSense Edge, DualShock 4) through raw HID: Windows doesn't
   offer them as XInput pads. A reader thread opens the first one it finds, over USB or
   Bluetooth, and keeps its latest input report; it looks again every second while none is
   connected. Report layouts as Linux's hid-playstation and hid-sony drivers read them. */
typedef struct { ULONG size; USHORT vendor, product, version; } HidAttributes;
typedef struct { DWORD size; GUID class_guid; DWORD flags; ULONG_PTR reserved; } DevInterfaceData;
typedef struct { DWORD size; char path[1]; } DevInterfaceDetail;
typedef void(WINAPI *HidGetGuidFn)(GUID *);
typedef BOOLEAN(WINAPI *HidGetAttributesFn)(HANDLE, HidAttributes *);
typedef BOOLEAN(WINAPI *HidGetFeatureFn)(HANDLE, void *, ULONG);
typedef void *(WINAPI *GetClassDevsFn)(const GUID *, const char *, HWND, DWORD);
typedef BOOL(WINAPI *EnumInterfacesFn)(void *, void *, const GUID *, DWORD, DevInterfaceData *);
typedef BOOL(WINAPI *GetInterfaceDetailFn)(void *, DevInterfaceData *, DevInterfaceDetail *, DWORD, DWORD *, void *);
typedef BOOL(WINAPI *DestroyListFn)(void *);

enum { SONY_NONE, SONY_DUALSENSE, SONY_DS4 };
typedef struct {  /* sticks, then the buttons and triggers as the DualShock 4's simple report has them */
    uint8_t lx, ly, rx, ry, face, shoulder, l2, r2;
} SonyState;

static CRITICAL_SECTION sony_lock;
static SonyState sony_state;
static volatile LONG sony_connected;

static HANDLE sony_open(int *kind)
{
    HMODULE hid = LoadLibraryA("hid.dll"), setup = LoadLibraryA("setupapi.dll");
    HidGetGuidFn get_guid;
    HidGetAttributesFn get_attributes;
    HidGetFeatureFn get_feature;
    GetClassDevsFn get_class_devs;
    EnumInterfacesFn enum_interfaces;
    GetInterfaceDetailFn get_detail;
    DestroyListFn destroy;
    GUID guid;
    void *list;
    HANDLE found = INVALID_HANDLE_VALUE;
    DWORD i;
    if (!hid || !setup)
        return found;
    get_guid = (HidGetGuidFn)(void *)GetProcAddress(hid, "HidD_GetHidGuid");
    get_attributes = (HidGetAttributesFn)(void *)GetProcAddress(hid, "HidD_GetAttributes");
    get_feature = (HidGetFeatureFn)(void *)GetProcAddress(hid, "HidD_GetFeature");
    get_class_devs = (GetClassDevsFn)(void *)GetProcAddress(setup, "SetupDiGetClassDevsA");
    enum_interfaces = (EnumInterfacesFn)(void *)GetProcAddress(setup, "SetupDiEnumDeviceInterfaces");
    get_detail = (GetInterfaceDetailFn)(void *)GetProcAddress(setup, "SetupDiGetDeviceInterfaceDetailA");
    destroy = (DestroyListFn)(void *)GetProcAddress(setup, "SetupDiDestroyDeviceInfoList");
    if (!get_guid || !get_attributes || !get_class_devs || !enum_interfaces || !get_detail || !destroy)
        return found;
    get_guid(&guid);
    list = get_class_devs(&guid, NULL, NULL, 0x12 /* DIGCF_PRESENT | DIGCF_DEVICEINTERFACE */);
    if (list == INVALID_HANDLE_VALUE)
        return found;
    for (i = 0; found == INVALID_HANDLE_VALUE; i++) {
        union { DevInterfaceDetail d; char bytes[1024]; } detail;
        HidAttributes attributes = {sizeof attributes};
        DevInterfaceData data = {sizeof data};
        HANDLE h;
        if (!enum_interfaces(list, NULL, &guid, i, &data))
            break;
        detail.d.size = sizeof(DWORD) + sizeof(char);  /* the fixed part's size, as SetupAPI checks it */
        if (!get_detail(list, &data, &detail.d, sizeof detail, NULL, NULL))
            continue;
        h = CreateFileA(detail.d.path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (get_attributes(h, &attributes) && attributes.vendor == 0x054C) {
            USHORT p = attributes.product;
            *kind = p == 0x0CE6 || p == 0x0DF2 ? SONY_DUALSENSE
                  : p == 0x05C4 || p == 0x09CC || p == 0x0BA0 ? SONY_DS4 : SONY_NONE;
            if (*kind != SONY_NONE) {
                /* Over Bluetooth, reading the calibration feature report switches the pad to its
                   full report (0x31 DualSense, 0x11 DualShock 4); over USB it changes nothing. */
                uint8_t feature[64] = {*kind == SONY_DUALSENSE ? 0x05 : 0x02};
                if (get_feature)
                    get_feature(h, feature, *kind == SONY_DUALSENSE ? 41 : 37);
                found = h;
                continue;
            }
        }
        CloseHandle(h);
    }
    destroy(list);
    return found;
}

/* One input report to the common state; 0 if it isn't one we read. */
static int sony_parse(int kind, const uint8_t *r, DWORD n, SonyState *s)
{
    const uint8_t *b;
    if (kind == SONY_DUALSENSE && (r[0] == 0x01 || r[0] == 0x31) && n >= 64) {
        /* USB (0x01) or full Bluetooth (0x31, one more header byte): sticks, L2, R2, a counter,
           then the buttons */
        b = r + (r[0] == 0x31 ? 2 : 1);
        s->lx = b[0], s->ly = b[1], s->rx = b[2], s->ry = b[3], s->l2 = b[4], s->r2 = b[5];
        s->face = b[7], s->shoulder = b[8];
        return 1;
    }
    if (kind == SONY_DS4 && r[0] == 0x11 && n >= 12)  /* DualShock 4, full Bluetooth */
        b = r + 3;
    else if (r[0] == 0x01 && n >= 10)  /* DualShock 4 over USB; either pad's simple Bluetooth report */
        b = r + 1;
    else
        return 0;
    s->lx = b[0], s->ly = b[1], s->rx = b[2], s->ry = b[3];
    s->face = b[4], s->shoulder = b[5], s->l2 = b[7], s->r2 = b[8];
    return 1;
}

static DWORD WINAPI sony_reader(void *unused)
{
    (void)unused;
    for (;;) {
        int kind = SONY_NONE;
        HANDLE h = sony_open(&kind);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        fprintf(stderr, "controller: %s connected\n", kind == SONY_DUALSENSE ? "DualSense" : "DualShock 4");
        for (;;) {
            uint8_t report[128];
            DWORD n = 0;
            SonyState s;
            if (!ReadFile(h, report, sizeof report, &n, NULL))
                break;
            if (!sony_parse(kind, report, n, &s))
                continue;
            EnterCriticalSection(&sony_lock);
            sony_state = s;
            LeaveCriticalSection(&sony_lock);
            InterlockedExchange(&sony_connected, 1);
        }
        InterlockedExchange(&sony_connected, 0);
        CloseHandle(h);
        fprintf(stderr, "controller: disconnected\n");
    }
    return 0;
}

static int sony_read(SonyState *out)
{
    static volatile LONG started;
    if (!InterlockedExchange(&started, 1)) {
        InitializeCriticalSection(&sony_lock);
        CloseHandle(CreateThread(NULL, 0, sony_reader, NULL, 0, NULL));
    }
    if (!sony_connected)
        return 0;
    EnterCriticalSection(&sony_lock);
    *out = sony_state;
    LeaveCriticalSection(&sony_lock);
    return 1;
}
#else
/* No XInput or HID pads on this host: SDL (gamepad.c) or nobody. */
typedef struct { uint16_t left, right; } XVibration;
static void (*xinput_set)(unsigned, void *);
static void *xinput(void) { return NULL; }

/* The motors without XInput: gamepad.c's pads (a no-op where it has no backend), off when
   DHD_VIBRATION=0, same dedupe as above. */
void host_pad_vibrate(int port, int light, int heavy)
{
    static int last[4] = {-1, -1, -1, -1};
    const char *setting = getenv("DHD_VIBRATION");
    if (setting && !atoi(setting)) light = heavy = 0;
    int key = (light ? 256 : 0) | (heavy & 255);
    if (port < 0 || port > 3 || key == last[port]) return;
    last[port] = key;
    if (gamepad_start()) gamepad_vibrate(port, light, heavy);
}
#endif

#ifdef _WIN32
/* The pressure byte of each pressure-sensitive button, in the DualShock 2's order. */
static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

static uint8_t stick(SHORT v, int flip)
{
    int x = (v >> 8) + 128;
    if (flip)
        x = 255 - x;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}
#endif

/* The game's controls are in riding mode (D_0029E290, func_00220550): keyboard.c moves the left
   stick with the arrows then, and the d-pad in menus. */
static int riding_controls(void) { return *(volatile int32_t *)GUEST(0x0829E290) != 0; }

/* The keyboard (layout: keyboard.c), only while the game's window has the keyboard (keyboard_focused,
   fed by the SDL thread: sdl_host.c), and not while Alt, Ctrl or a Windows key is held, so Alt+Tab,
   Alt+Enter, Ctrl+Esc and the like stay the system's. A key let go elsewhere is let go here too
   (keyboard_focus clears the keys when the window loses the keyboard). Letters are read by their
   place on a US keyboard (keyboard.c), and text_input_key_allowed takes the character the key types. */
static void keyboard_merge(HostPad *out)
{
    uint8_t down[256] = {0};
    if (keyboard_focused() && !keyboard_is_down(VKEY_MENU) && !keyboard_is_down(VKEY_CONTROL) &&
        !keyboard_is_down(VKEY_LWIN) && !keyboard_is_down(VKEY_RWIN)) {
        int count;
        const uint8_t *keys = keyboard_keys(&count);
        for (int i = 0; i < count; i++) {
            unsigned place = keys[i];
            if (text_input_key_allowed(keyboard_typed_of(place)) && keyboard_position_down(place)) down[keys[i]] = 1;
        }
    }
    keyboard_pad(down, riding_controls(), host_seconds(), out);
}

/* Without SDL (DHD_SDL=0, or it didn't start): XInput pad `port`, else (port 0) a DualSense or
   DualShock 4 over raw HID. */
#ifdef _WIN32
static int direct_pad(int port, HostPad *out)
{
    XInputGetStateFn get = xinput();
    XState x;
    size_t i;
    if (get && get((DWORD)port, &x) == ERROR_SUCCESS) {
        static const struct { WORD x; uint16_t button; } map[] = {
            {0x0001, PAD_UP}, {0x0002, PAD_DOWN}, {0x0004, PAD_LEFT}, {0x0008, PAD_RIGHT}, {0x0010, PAD_START},
            {0x0020, PAD_SELECT}, {0x0040, PAD_L3}, {0x0080, PAD_R3}, {0x0100, PAD_L1}, {0x0200, PAD_R1},
            {0x1000, PAD_CROSS}, {0x2000, PAD_CIRCLE}, {0x4000, PAD_SQUARE}, {0x8000, PAD_TRIANGLE},
        };
        for (i = 0; i < sizeof map / sizeof *map; i++)
            if (x.buttons & map[i].x)
                out->buttons |= map[i].button;
        if (x.left_trigger > 30)
            out->buttons |= PAD_L2;
        if (x.right_trigger > 30)
            out->buttons |= PAD_R2;
        out->left_x = stick(x.lx, 0), out->left_y = stick(x.ly, 1);
        out->right_x = stick(x.rx, 0), out->right_y = stick(x.ry, 1);
        for (i = 0; i < 12; i++)
            out->pressure[i] = pressure_order[i] == PAD_L2 ? x.left_trigger
                             : pressure_order[i] == PAD_R2 ? x.right_trigger
                             : out->buttons & pressure_order[i] ? 255 : 0;
        return 1;
    }
    SonyState sony;
    if (port == 0 && sony_read(&sony)) {
        /* d-pad: a hat, 0 up then clockwise, 8 none */
        static const uint16_t hat[9] = {PAD_UP, PAD_UP | PAD_RIGHT, PAD_RIGHT, PAD_DOWN | PAD_RIGHT, PAD_DOWN,
                                        PAD_DOWN | PAD_LEFT, PAD_LEFT, PAD_UP | PAD_LEFT, 0};
        static const struct { uint8_t bit; uint16_t button; } face[] = {
            {0x10, PAD_SQUARE}, {0x20, PAD_CROSS}, {0x40, PAD_CIRCLE}, {0x80, PAD_TRIANGLE}};
        /* Create/Share is SELECT, Options START */
        static const struct { uint8_t bit; uint16_t button; } shoulder[] = {
            {0x01, PAD_L1}, {0x02, PAD_R1}, {0x10, PAD_SELECT}, {0x20, PAD_START}, {0x40, PAD_L3}, {0x80, PAD_R3}};
        out->buttons = hat[(sony.face & 0x0F) > 8 ? 8 : sony.face & 0x0F];
        for (i = 0; i < sizeof face / sizeof *face; i++)
            if (sony.face & face[i].bit)
                out->buttons |= face[i].button;
        for (i = 0; i < sizeof shoulder / sizeof *shoulder; i++)
            if (sony.shoulder & shoulder[i].bit)
                out->buttons |= shoulder[i].button;
        if (sony.l2 > 30)
            out->buttons |= PAD_L2;
        if (sony.r2 > 30)
            out->buttons |= PAD_R2;
        out->left_x = sony.lx, out->left_y = sony.ly, out->right_x = sony.rx, out->right_y = sony.ry;
        for (i = 0; i < 12; i++)
            out->pressure[i] = pressure_order[i] == PAD_L2 ? sony.l2
                             : pressure_order[i] == PAD_R2 ? sony.r2
                             : out->buttons & pressure_order[i] ? 255 : 0;
        return 1;
    }
    return 0;
}
#else
static int direct_pad(int port, HostPad *out)
{
    (void)port;
    (void)out;
    return 0;
}
#endif

/* Player `port`'s controller (SDL, gamepad.c, else XInput and the Sony pads directly: one of the
   two, never both, so a pad isn't read twice), and for player 1 the keyboard as well, merged. */
void host_pad_device(int port, HostPad *out)
{
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    int connected = gamepad_start() ? gamepad_read(port, out) : direct_pad(port, out);
    if (port == 0) keyboard_merge(out);
    out->connected = (uint8_t)(connected || port == 0);  /* player 1 always has the keyboard */
}

/* The startup movies (func_001EBFD8: the logos, then the attract movie) play unless dhd.ini's
   skip_movies (DHD_SKIP_MOVIES, PC SETTINGS > Skip Movies) is nonzero; movies play natively
   (port/hle/movie.c, finding 055). The race, gate and bench tools set DHD_SKIP_MOVIES=1, so their
   runs stay as they were. OPTIONS > VIDEO CLIPS always plays the clip the player chose. */
int host_service_host_skip_movies(void)
{
    static int logged;
    const char *skip = getenv("DHD_SKIP_MOVIES");
    if (!skip || !atoi(skip)) return 0;
    if (!logged++) hle_log("DHD_SKIP_MOVIES: bypassing startup movie sequence\n");
    return 1;
}

/* PC settings: dhd.ini next to dhd.exe (or the file DHD_SETTINGS names), one "key = value" a
   line, [sections] and ;/# comments ignored. Each setting becomes the environment variable it
   stands for, unless that variable is already set: the environment overrides the file. A line
   may also name a DHD_* variable directly. Options are host-side only; none reaches game state. */
static void setting(const char *name, const char *value)
{
    if (!getenv(name)) _putenv_s(name, value);
}

static void apply_setting(const char *key, const char *value)
{
    if (!_stricmp(key, "game_options")) return; /* read after the game's own boot/load */
    static const struct { const char *key, *name; } plain[] = {
        {"iso", "DHD_ISO"}, {"skip_movies", "DHD_SKIP_MOVIES"}, {"window", "DHD_WINDOW"},
        {"width", "DHD_WINDOW_WIDTH"}, {"height", "DHD_WINDOW_HEIGHT"}, {"monitor", "DHD_MONITOR"},
        {"scale", "DHD_SCALE"}, {"widescreen", "DHD_WIDESCREEN"}, {"vibration", "DHD_VIBRATION"},
        {"memory_card", "DHD_MEMCARD"},
        {"sound_latency_ms", "DHD_SND_LATENCY_MS"},
        {"server", "DHD_NET_SERVER"}, {"name", "DHD_NET_NAME"}, {"rider", "DHD_NET_RIDER"},  /* online (port/net) */
        {"update_url", "DHD_UPDATE_URL"}, {"auto_update", "DHD_AUTO_UPDATE"},  /* the updater (update.c) */
        {"launcher", "DHD_LAUNCHER_MODE"},  /* off: dhd.exe doesn't open the launcher (update.c) */
        {"texture_pack", "DHD_TEXTURE_PACK"}, {"textures", "DHD_TEXTURES"},  /* texture packs (port/gs/texpack.c) */
        {"texture_cache_mb", "DHD_TEXTURE_CACHE_MB"},
        {"font", "DHD_FONT"},  /* the sharp font (port/gs/font_sharp.c) */
        {"fps", "DHD_FPS"},
        {"vsync", "DHD_VSYNC"},  /* 1: no tearing (port/gs/gpu_rust.c) */
        {"unlock_all", "DHD_UNLOCK_ALL"},  /* 1: every track, rider, bike (port_unlock_all, pc_settings.c) */
    };
    if (!_strnicmp(key, "DHD_", 4)) { setting(key, value); return; }
    if (!_stricmp(key, "renderer")) { setting("DHD_RENDER", _stricmp(value, "native") ? "exact" : "native"); return; }
    /* fps selects real offline simulation steps (finding 067). */
    for (unsigned i = 0; i < sizeof plain / sizeof *plain; i++)
        if (!_stricmp(key, plain[i].key)) { setting(plain[i].name, value); return; }
    hle_log("settings: unknown key %s\n", key);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
    return s;
}

/* dhd.ini's path: DHD_SETTINGS, else beside dhd.exe. */
static void settings_path(char *path, size_t size)
{
    const char *named = getenv("DHD_SETTINGS");
    if (named) { snprintf(path, size, "%s", named); return; }
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
#else
    unsigned n = host_exe_path(path, (unsigned)size);
#endif
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    snprintf(path + n, size - n, "dhd.ini");
}

/* A value in double quotes is taken as it is (a path may hold ; or #), else up to a comment. */
static char *setting_value(char *value)
{
    if (*value == '"') {
        char *end = strchr(value + 1, '"');
        if (end) { *end = 0; return value + 1; }
    }
    char *comment = strpbrk(value, ";#");
    if (comment && (comment == value || comment[-1] == ' ' || comment[-1] == '\t')) { *comment = 0; value = trim(value); }
    return value;
}

static int iso_from_environment;  /* DHD_ISO was set before dhd.ini was read: it overrides the file */

/* The PC settings the environment sets (so dhd.ini can't), for the settings page to show them
   locked. The first process looks before reading dhd.ini; the relaunched one, whose environment
   already holds dhd.ini's values, is told in DHD_ENV_OVERRIDES. */
static void note_overrides(void)
{
    static const char *const names[] = {"DHD_SCALE", "DHD_WINDOW", "DHD_WINDOW_WIDTH", "DHD_WINDOW_HEIGHT",
                                        "DHD_ASPECT", "DHD_PRESENT_FILTER", "DHD_WIDESCREEN", "DHD_SKIP_MOVIES",
                                        "DHD_VIBRATION", "DHD_RENDER", "DHD_TEXTURES", "DHD_FONT"};
    char list[256] = ",";
    if (getenv("DHD_RELAUNCHED")) return;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (getenv(names[i]) && strlen(list) + strlen(names[i]) + 2 < sizeof list)
            strcat(strcat(list, names[i]), ",");
    _putenv_s("DHD_ENV_OVERRIDES", list);
}

int host_setting_locked(const char *name)
{
    char key[64];
    const char *list = getenv("DHD_ENV_OVERRIDES");
    snprintf(key, sizeof key, ",%s,", name);
    return list && strstr(list, key) != NULL;
}

/* The official public race server (online/server, deployed by the project). DHD_NET_SERVER or
   a "server =" line with an address replaces it, for anyone running their own. */
#define OFFICIAL_SERVER "wss://dhd.skuza.app"

static int settings_has(const char *key);

/* A player's settings where dhd.ini doesn't say (round 40, finding 056): the native renderer at
   2x, borderless on the monitor at its own resolution, widescreen in the monitor's shape with
   the HUD kept in shape, the startup movies skipped (skip_movies 1, finding 086 S1; 0 before),
   V-Sync on (no tearing; issue #2).
   The window size, for the windowed display, is the monitor's desktop resolution ("monitor":
   present.c reads it through SDL when it makes the window and puts the numbers in its place;
   only when dhd.ini has neither a width nor a height line). Only with a dhd.ini (the game
   writes one on the first start; the release ships none): the tools run without one and keep
   the old defaults, and a line in dhd.ini or a DHD_* variable wins. */
static const char *const player_defaults[][3] = {
    {"renderer", "native", "DHD_RENDER"}, {"scale", "2", "DHD_SCALE"}, {"window", "borderless", "DHD_WINDOW"},
    {"widescreen", "1", "DHD_WIDESCREEN"}, {"DHD_ASPECT", "wide", "DHD_ASPECT"}, {"skip_movies", "1", "DHD_SKIP_MOVIES"},
    {"vsync", "1", "DHD_VSYNC"},
};

void host_settings_load(void)
{
    char path[MAX_PATH];
    iso_from_environment = getenv("DHD_ISO") != NULL;
    note_overrides();
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == ';' || *s == '#' || *s == '[' || !eq) continue;
        *eq = 0;
        char *key = trim(s), *value = setting_value(trim(eq + 1));
        if (*key && *value) apply_setting(key, value);
    }
    if (f) {
        fclose(f);
        for (unsigned i = 0; i < sizeof player_defaults / sizeof *player_defaults; i++)
            if (!settings_has(player_defaults[i][0])) setting(player_defaults[i][2], player_defaults[i][1]);
        if (!settings_has("width") && !settings_has("height") && !getenv("DHD_WINDOW_WIDTH") && !getenv("DHD_WINDOW_HEIGHT")) {
            setting("DHD_WINDOW_WIDTH", "monitor");
            setting("DHD_WINDOW_HEIGHT", "monitor");
        }
    }
    setting("DHD_NET_SERVER", OFFICIAL_SERVER);  /* no server set (an empty "server =" sets none) */
#if defined(__linux__) && !defined(__ANDROID__)
    /* Linux scheduler/presenter jitter must not keep moving the simulation's deadline. */
    setting("DHD_LATENCY", "0");
#endif
}

/* Whether dhd.ini has a line for key. */
static int settings_has(const char *key)
{
    char path[MAX_PATH], line[1024];
    int found = 0;
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && !found && fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == ';' || *s == '#' || *s == '[' || !eq) continue;
        *eq = 0;
        found = !_stricmp(trim(s), key);
    }
    if (f) fclose(f);
    return found;
}

/* What a new dhd.ini starts with. The release ships none (port/tools/package_release.py), so
   unzipping a new version over an old folder never replaces the player's: the game writes this
   the first time it saves a setting (the disc image, on the first start). */
static const char settings_template[] =
    "; Downhill Revived settings: one \"key = value\" per line. The game adds your disc image (iso)\n"
    "; on the first start, and OPTIONS > PC SETTINGS writes the display settings here. DHD_*\n"
    "; environment variables override them.\n"
    "\n"
    "; scale: the 3D picture's resolution, 1-8 times the PlayStation 2's,\n"
    ";   or window.\n"
    "; window: windowed (width x height; the monitor's resolution when not set), borderless\n"
    ";   or fullscreen (borderless over the whole monitor).\n"
    "; widescreen: 1 the monitor's shape (a wider view, the HUD and menus kept in shape), 0 off,\n"
    ";   or a shape such as 21:9. DHD_ASPECT, the picture's shape: wide (widescreen's), 4:3, 16:9,\n"
    ";   stretch or pixels.\n"
    "renderer = native\n"
    "scale = 2\n"
    "window = borderless\n"
    "widescreen = 1\n"
    "DHD_ASPECT = wide\n"
    "skip_movies = 1\n"
    "; Offline real simulation rate: 60 (original), 120, 144, 165, 240 or monitor (max 240).\n"
    "; Online always uses 60. Frame rate changes apply at the next start.\n"
    "fps = 60\n"
    "\n"
    "; Online races use the official server, wss://dhd.skuza.app, when\n"
    "; server is empty. To use a server of your own, put its address here, for example\n"
    ";   server = wss://races.example.org\n"
    "server =\n"
    "\n"
    "; At start the game looks for a newer release and offers it; online races need the newest.\n"
    "; auto_update = ask (asks first), on (updates without asking) or off (never looks).\n"
    "; update_url: where to look, a latest.json or a GitHub releases API address; empty means\n"
    "; the official releases. An update replaces dhd.exe and the text files, never dhd.ini or saves.\n"
    "update_url =\n"
    "auto_update = ask\n"
    "; launcher: on (dhd.exe opens DownhillRevived.exe, the launcher, when it is beside it) or off\n"
    "; (dhd.exe starts the game straight away).\n"
    "launcher = on\n"
    "\n"
    "; A texture pack (upscale mod) made for PCSX2 works unchanged: texture_pack is its folder of\n"
    "; PNGs, the full path of PCSX2's textures\\SCUS-97177\\replacements folder for example (in\n"
    "; Documents\\PCSX2). When empty, a textures\\SCUS-97177\\replacements folder beside dhd.exe is\n"
    "; used if there is one. textures = off draws the game's own textures (PC SETTINGS > Texture\n"
    "; Pack).\n"
    "texture_pack =\n"
    "textures = on\n"
    "\n"
    "; font: sharp draws the game's font with clean edges at any resolution (made in memory from\n"
    "; the font on your disc each time the game starts); original draws it as the PS2 does.\n"
    "font = sharp\n";

/* Writes key = value into dhd.ini: replaces the key's first line, else adds one at the end; every
   other line (comments, sections, unknown keys) stays as it was. A missing dhd.ini starts as
   settings_template. Nonzero when saved. */
static int settings_save(const char *key, const char *value)
{
    char path[MAX_PATH], temp[MAX_PATH + 8], line[1024];
    settings_path(path, sizeof path);
    snprintf(temp, sizeof temp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(temp, "w");
    if (!out) { if (in) fclose(in); return 0; }
    const char *quote = strpbrk(value, ";#\"") || value[0] == ' ' ? "\"" : "";
    int written = 0;
    if (!in) fputs(settings_template, out);
    while (in && fgets(line, sizeof line, in)) {
        char copy[1024], *s, *eq;
        snprintf(copy, sizeof copy, "%s", line);
        s = trim(copy);
        if (!written && (eq = strchr(s, '=')) && *s != ';' && *s != '#' && *s != '[') {
            *eq = 0;
            if (!_stricmp(trim(s), key)) { fprintf(out, "%s = %s%s%s\n", key, quote, value, quote); written = 1; continue; }
        }
        fputs(line, out);
        if (!strchr(line, '\n')) fputc('\n', out);
    }
    if (!written) fprintf(out, "%s = %s%s%s\n", key, quote, value, quote);
    if (in) fclose(in);
    int ok = !ferror(out);
    ok &= fclose(out) == 0;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileA(temp);
#else
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
#endif
    return ok;
}

int host_setting_save(const char *key, const char *value) { return settings_save(key, value); }

/* Read afresh after each game-data load; these values never become DHD_* overrides. */
int host_game_options_read(char *value, size_t size)
{
    char path[MAX_PATH], line[1024];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!eq || *s == ';' || *s == '#' || *s == '[') continue;
        *eq = 0;
        if (!_stricmp(trim(s), "game_options")) {
            snprintf(value, size, "%s", setting_value(trim(eq + 1)));
            found = 1; break;
        }
    }
    fclose(f);
    return found;
}

/* ---- the disc image (first start) ---- */

static uint32_t le32_at(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Whether path is Downhill Domination (USA): the ISO 9660 root holds a SYSTEM.CNF whose boot
   line names SCUS_971.77, and SCUS_971.77 is the retail one (game_data_read: the game's data
   comes from it). NULL if it is, else why not. disc_shallow (the Android start, before the core
   exists) leaves out game_data_read, which runs in the core; host_disc_ready checks it later. */
static int disc_shallow;
static const char *disc_problem(const char *path, char *why, size_t size)
{
    enum { SECTOR = 2048 };
    static uint8_t sector[SECTOR];
    static char text[SECTOR + 1];
    FILE *f = fopen(path, "rb");
    if (!f) return "The file can't be opened.";
    const char *problem = NULL;
    uint32_t cnf = 0, cnf_size = 0;
#define READ(lsn) (_fseeki64(f, (long long)(lsn) * SECTOR, SEEK_SET) == 0 && fread(sector, SECTOR, 1, f) == 1)
    if (!READ(16) || sector[0] != 1 || memcmp(sector + 1, "CD001", 5)) {
        problem = "It isn't a disc image (no ISO 9660 volume in it).";
        goto done;
    }
    /* The primary volume descriptor's root directory record (at 156): extent at +2, size at +10. */
    uint32_t dir = le32_at(sector + 156 + 2), dir_size = le32_at(sector + 156 + 10);
    for (uint32_t off = 0; off < dir_size && off < 16 * SECTOR && !cnf; off += SECTOR) {
        if (!READ(dir + off / SECTOR)) break;
        for (unsigned i = 0; i + 33 < SECTOR && sector[i]; i += sector[i]) {
            const uint8_t *r = sector + i;
            if (r[32] >= 10 && i + 33 + r[32] <= SECTOR && !_strnicmp((const char *)r + 33, "SYSTEM.CNF", 10) &&
                (r[32] == 10 || r[33 + 10] == ';')) {
                cnf = le32_at(r + 2);
                cnf_size = le32_at(r + 10);
                break;
            }
        }
    }
    if (!cnf || !READ(cnf)) {
        problem = "It has no SYSTEM.CNF, so it isn't a PlayStation 2 game disc.";
        goto done;
    }
    memcpy(text, sector, SECTOR);
    text[cnf_size < SECTOR ? cnf_size : SECTOR] = 0;
    if (strstr(text, "SCUS_971.77")) {
        if (!disc_shallow) problem = game_data_read(path, NULL);  /* the game's program and data, unchanged (game_data.c) */
        goto done;
    }
    /* Name the game it is: BOOT2 = cdrom0:\SLUS_123.45;1 */
    const char *boot = strstr(text, "BOOT2");
    char id[32] = "";
    if (boot && (boot = strpbrk(boot, "\\:"))) {
        while (*boot == '\\' || *boot == ':') boot++;
        size_t n = strcspn(boot, ";\r\n");
        snprintf(id, sizeof id, "%.*s", (int)(n < 31 ? n : 31), boot);
    }
    snprintf(why, size, "It is another PlayStation 2 disc%s%s%s, not Downhill Domination (USA), SCUS-97177.",
             *id ? " (" : "", id, *id ? ")" : "");
    problem = why;
done:
#undef READ
    fclose(f);
    return problem;
}

#ifdef _WIN32
typedef BOOL(WINAPI *GetOpenFileNameFn)(OPENFILENAMEA *);

/* The file-open dialog for a disc image (comdlg32 loaded here: no new import); 0 if cancelled. */
static int choose_disc(char *path, size_t size)
{
    static GetOpenFileNameFn open_file;
    if (!open_file) {
        HMODULE dll = LoadLibraryA("comdlg32.dll");
        if (dll) open_file = (GetOpenFileNameFn)(void *)GetProcAddress(dll, "GetOpenFileNameA");
        if (!open_file) return 0;
    }
    OPENFILENAMEA o = {sizeof o};
    char folder[MAX_PATH];  /* dhd.exe's: players often keep the disc image beside the game */
    DWORD n = GetModuleFileNameA(NULL, folder, sizeof folder);
    while (n && folder[n - 1] != '\\' && folder[n - 1] != '/') n--;
    folder[n] = 0;
    o.lpstrInitialDir = folder;
    path[0] = 0;
    o.lpstrFilter = "Disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    o.nFilterIndex = 1;
    o.lpstrFile = path; o.nMaxFile = (DWORD)size;
    o.lpstrTitle = "Choose your Downhill Domination (USA) disc image";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    return open_file(&o) && path[0];
}
#endif

/* Before the relaunch (only the first of the two dhd.exe asks): the disc image dhd.ini names
   (iso), if it is there and is Downhill Domination (USA); else a message, a file-open dialog,
   the choice checked and saved to dhd.ini. DHD_ISO in the environment overrides the file and is
   never questioned (the tools set it). Returns 0 to go on, 1 if the player cancelled. */
int host_disc_ready(void)
{
    static const char title[] = "Downhill Revived";
    static char why[256], chosen[MAX_PATH], message[1024];
    if (iso_from_environment || getenv("DHD_HEADLESS")) return 0;
    const char *iso = getenv("DHD_ISO");
    const char *problem = iso ? disc_problem(iso, why, sizeof why) : NULL;
    if (iso && !problem) return 0;
#ifdef _WIN32
    if (!iso)
        snprintf(message, sizeof message, "Choose your Downhill Domination (USA) disc image.\n\n"
                 "The game plays from your own copy of the disc, as an .iso file (SCUS-97177). "
                 "You choose it once; the game remembers it.");
    else if (GetFileAttributesA(iso) == INVALID_FILE_ATTRIBUTES)
        snprintf(message, sizeof message, "The disc image chosen before is gone:\n%s\n\n"
                 "Choose your Downhill Domination (USA) disc image.", iso);
    else
        snprintf(message, sizeof message, "The disc image chosen before can't be used:\n%s\n%s\n\n"
                 "Choose your Downhill Domination (USA) disc image.", iso, problem);
    if (MessageBoxA(NULL, message, title, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDOK) return 1;
    for (;;) {
        if (!choose_disc(chosen, sizeof chosen)) return 1;
        if (!(problem = disc_problem(chosen, why, sizeof why))) break;
        snprintf(message, sizeof message, "%s\n\n%s\n\nChoose your Downhill Domination (USA) disc image (SCUS-97177).",
                 chosen, problem);
        if (MessageBoxA(NULL, message, title, MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND) != IDRETRY) return 1;
    }
    _putenv_s("DHD_ISO", chosen);
    hle_log("settings: disc image %s\n", chosen);
    /* The player defaults where dhd.ini doesn't say (a new dhd.ini has them from its template;
       without dhd.ini the tools' defaults stay, the exact renderer among them). This start uses
       them too. */
    int saved = settings_save("iso", chosen);
    for (unsigned i = 0; i < sizeof player_defaults / sizeof *player_defaults; i++)
        if (!settings_has(player_defaults[i][0])) {
            setting(player_defaults[i][2], player_defaults[i][1]);
            if (saved) settings_save(player_defaults[i][0], player_defaults[i][1]);
        }
    if (!saved) {
        char path[MAX_PATH];
        settings_path(path, sizeof path);
        snprintf(message, sizeof message, "The disc image can't be saved in\n%s\n\n"
                 "The game starts with it now, but will ask again next time.", path);
        MessageBoxA(NULL, message, title, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    return 0;
#else
    /* No dialogs on this host: without DHD_ISO the game cannot start. */
#ifdef __ANDROID__
    /* The copy android_main.c made passed its first look but not the game's data: say so and drop
       the copy, so the next start asks for the disc again. */
    if (iso && problem) {
        snprintf(message, sizeof message, "The disc image can't be used:\n%s\n\nStart the game again to choose "
                 "your Downhill Domination (USA) disc image (SCUS-97177).", problem);
        host_error_box(title, message);
        remove(iso);
    }
#endif
    if (!iso)
        fprintf(stderr, "settings: set DHD_ISO to your Downhill Domination (USA) disc image\n");
    else
        fprintf(stderr, "settings: disc image problem: %s\n", problem ? problem : iso);
    return 1;
#endif
}

/* Widescreen: port/hle/widescreen.c. */

/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_host_init(void) { host_init(); }
void host_service_host_console_init(void) { host_console_init(); }
int host_service_host_disc_ready(void) { return host_disc_ready(); }
#ifdef __ANDROID__
/* The Android start (port/host/android_main.c) checks the copy it made of the player's disc image:
   NULL when it is Downhill Domination (USA), else why not. */
const char *host_disc_check(const char *path)
{
    static char why[256];
    disc_shallow = 1;
    const char *problem = disc_problem(path, why, sizeof why);
    disc_shallow = 0;
    return problem;
}
#endif
int host_service_host_poll_dump(void) { return InterlockedExchange(&dump_pending,0)!=0; }
void host_service_host_settings_load(void) { host_settings_load(); }
int host_service_host_setting_save(uint32_t a0, uint32_t a1) { return host_setting_save((const char *)GUEST(a0), (const char *)GUEST(a1)); }
int host_service_host_setting_locked(uint32_t a0) { return host_setting_locked((const char *)GUEST(a0)); }
void host_service_host_pad_device(int a0, uint32_t a1) { host_pad_device(a0, (HostPad *)GUEST(a1)); }
void host_service_host_pad_vibrate(int a0, int a1, int a2) { host_pad_vibrate(a0, a1, a2); }
int host_service_host_relaunch_reserving(uint32_t a0, uint32_t a1) {
    (void)a0; (void)a1; return -1; /* linear memory is already reserved by the allocator */
}


/* Wall-clock pacing is shared by the EE producer and native presenter. */
#define WORK_SAMPLES 300
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
#define FIELD_SECONDS pacing_period
#else
#include "../include/fps.h"
#define FIELD_SECONDS port_frame_period()
#endif
static double pacing_period=1001.0/60000.0;
static uint64_t pacing_field;
static double jit_due, frame_began;           /* the next picture's due; when this frame's pad was read */
static double handed_due, handed_began;       /* the same for the picture being handed on now */
static volatile float work_ms[WORK_SAMPLES];  /* pad read to picture ready, by the presenter */
static volatile unsigned work_count;
static volatile double shown_due, shown_at;   /* the presenter's last picture: its due (0 while written), when shown */
static volatile int shown_late;               /* ... and whether it was late (ready after its due) */
static FILE *pacing_log[2];  /* DHD_PACING_LOG=prefix: prefix-ee.csv, prefix-present.csv (each frame) */

/* Pictures above 60 a second follow the display's refresh (finding 086, "Frame-time spikes at 144").
   The requested grid (1/144 s) is exact on the QueryPerformanceCounter clock, but a "144 Hz" monitor
   refreshes at its own rate (143.855 Hz by its mode on the test PC, 143.858 Hz on the QPC clock by
   DWM's vblanks): every 1 / (144 - 143.858) = 7 s one refresh got two pictures, which drops one on a
   fixed-refresh display and, with G-SYNC at its top rate, makes a present wait a whole refresh (a
   frame-time spike every 7 s). With "monitor" (143, the whole hertz) the grid was slower than the
   display: a repeated picture every 1.2 s.
   Now, when the requested rate is within 2% of the primary display's refresh (or of a whole fraction
   of it), a picture lasts n refreshes: the mode's exact rate (QueryDisplayConfig), refined from DWM's
   vblanks (DwmGetCompositionTimingInfo's qpcVBlank) to the QPC clock, plus 20 ppm so that with a
   variable refresh rate the pictures stay just under its top. (DWM's own qpcRefreshPeriod is a noisy
   running estimate that falls back to 144.000 here, so it isn't used.) While DWM's new vblanks lie
   on one fixed grid (two in a row: not a variable refresh rate, where the period alone is right),
   each due also moves a quarter of its distance, at most 1% of a refresh, towards half a refresh
   after a vblank, so presents keep clear of the compositor's latch. Only an on-time picture after
   an on-time one may come that 1% sooner than a period after it; a late picture (the CPU behind),
   and the one after it, wait the whole period, so late pictures never come faster than the refresh
   and the first on-time one never falls back over the vblank after them (two in one refresh).
   Late pictures still move the grid as before and never wait for a vblank. 60 a second (and
   online, always 60) keeps its 59.94 grid; DHD_PACE_LOCK=0 turns this off. x64 only: i686 keeps
   port_frame_period (FIELD_SECONDS). */
static struct {
    double next_query;         /* EE thread: when to ask again */
    double nominal;            /* the display mode's refresh period (s); 0 unknown */
    double refresh;            /* the refresh period on the QPC clock (nominal, refined from vblanks) */
    double vblank, anchor;     /* the newest vblank DWM reported, and an older one on its grid (host_wall_seconds) */
    unsigned long long vblank_qpc;
    int fits, misfits;         /* new vblanks in a row on (off) the grid of the one before */
    int frames;                /* refreshes per picture while the pictures follow the display; 0 not */
    int logged;                /* the state last logged (-1 none) */
} display = {0, 0, 0, 0, 0, 0, 0, 0, 0, -1};
static volatile double pacing_slack;  /* how much sooner than a period an on-time (steered) picture may come */

#if defined(_WIN64)
#include <dwmapi.h>
/* The primary display's refresh period by its mode (the path whose source is at 0,0), 0 unknown. */
static double display_nominal(void)
{
    static DISPLAYCONFIG_PATH_INFO path[64];
    static DISPLAYCONFIG_MODE_INFO mode[128];
    UINT32 paths = 0, modes = 0;
    double period = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || !paths || paths > 64 ||
        modes > 128 || QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path, &modes, mode, NULL) != ERROR_SUCCESS)
        return 0;
    for (UINT32 i = 0; i < paths; i++) {
        UINT32 m = path[i].sourceInfo.modeInfoIdx;
        int primary = m < modes && mode[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                      mode[m].sourceMode.position.x == 0 && mode[m].sourceMode.position.y == 0;
        DISPLAYCONFIG_RATIONAL r = path[i].targetInfo.refreshRate;
        if ((primary || !period) && r.Numerator && r.Denominator) period = (double)r.Denominator / r.Numerator;
        if (primary) break;
    }
    return period > 1.0 / 1000 && period < 1.0 / 20 ? period : 0;
}

/* EE thread, every 0.5 s while above 60 a second: the refresh period and DWM's newest vblank. */
static void display_query(void)
{
    static HRESULT (WINAPI *timing)(HWND, DWM_TIMING_INFO *);
    static int loaded;
    if (!loaded) {
        loaded = 1;
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        timing = dwm ? (HRESULT (WINAPI *)(HWND, DWM_TIMING_INFO *))(void *)GetProcAddress(dwm, "DwmGetCompositionTimingInfo")
                     : NULL;
    }
    if (!display.nominal || display.misfits >= 10) {  /* first, or the mode may have changed */
        double nominal = display_nominal();
        if (nominal != display.nominal) display.refresh = nominal, display.anchor = 0, display.fits = 0;
        display.nominal = nominal;
        display.misfits = 0;
    }
    DWM_TIMING_INFO info;
    LARGE_INTEGER freq, qpc;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    if (!display.nominal || !timing || FAILED(timing(NULL, &info)) || !QueryPerformanceFrequency(&freq) ||
        !info.qpcVBlank || info.qpcVBlank == display.vblank_qpc)
        return;
    QueryPerformanceCounter(&qpc);
    double refresh = display.refresh,
           vblank = host_wall_seconds() - (double)((LONGLONG)qpc.QuadPart - (LONGLONG)info.qpcVBlank) / (double)freq.QuadPart;
    if (display.vblank_qpc) {
        double span = vblank - display.vblank, k = floor(span / refresh + 0.5);
        int fit = k >= 1 && fabs(span - k * refresh) < 0.0002 + span * 5e-5;  /* 0.2 ms, plus 50 ppm of the span */
        display.fits = fit ? display.fits + 1 : 0;
        display.misfits = fit ? 0 : display.misfits + 1;
        if (!fit) display.anchor = 0;
        else if (!display.anchor) display.anchor = display.vblank;
        else {  /* the period on the QPC clock: whole refreshes since the anchor (2 s and more) */
            double whole = vblank - display.anchor, n = floor(whole / refresh + 0.5);
            if (whole >= 2 && fabs(whole / n / display.nominal - 1) < 5e-4) display.refresh = whole / n;
            if (whole > 600) display.anchor = vblank;
        }
    }
    display.vblank_qpc = info.qpcVBlank;
    display.vblank = vblank;
}
#endif

/* host_pacing_config: the period the pictures keep (EE thread). */
static double display_period(double period)
{
#if defined(_WIN64)
    static int off = -1;
    if (off < 0) {
        const char *s = host_getenv("DHD_PACE_LOCK");
        off = s && !atoi(s);
    }
    int frames = 0;
    if (!off && period < 1.0 / 61) {  /* above 60 a second */
        double now = host_wall_seconds();
        if (now >= display.next_query) {
            display.next_query = now + 0.5;
            display_query();
        }
        double n = display.refresh ? floor(period / display.refresh + 0.5) : 0;
        if (n >= 1 && fabs(n * display.refresh - period) <= 0.02 * period) frames = (int)n;
        if (display.refresh && frames != display.logged) {
            display.logged = frames;
            /* (the EE thread rounds toward zero, so printf truncates the last digit) */
            if (frames)
                fprintf(stderr, "pacing: %.0f pictures/s follow the display's refresh, %.4f Hz (%d per picture)\n",
                        1 / period, 1 / display.refresh, frames);
            else
                fprintf(stderr, "pacing: the display refreshes at %.4f Hz, not near %.0f pictures/s: they keep their own grid\n",
                        1 / display.refresh, 1 / period);
        }
    }
    display.frames = frames;
    pacing_slack = frames ? 0.01 * display.refresh : 0;  /* display_steer's limit */
    return frames ? frames * display.refresh * (1 + 2e-5) : period;
#else
    return period;
#endif
}

/* How far to move a due towards half a refresh after a vblank (0 unless DWM's vblanks lie on a grid). */
static double display_steer(double due)
{
    double refresh = display.refresh;
    if (!display.frames || display.fits < 2 || due - display.vblank > 30) return 0;
    double error = fmod(display.vblank + 0.5 * refresh - due, refresh), limit = 0.01 * refresh;
    if (error > 0.5 * refresh) error -= refresh;
    else if (error < -0.5 * refresh) error += refresh;
    error *= 0.25;
    return error > limit ? limit : error < -limit ? -limit : error;
}

static FILE *pacing_file(int which)
{
    static int opened;
    if (!opened) {
        opened = 1;
        const char *prefix = host_getenv("DHD_PACING_LOG");
        char path[512];
        for (int i = 0; prefix && i < 2; i++) {
            snprintf(path, sizeof path, "%s-%s.csv", prefix, i ? "present" : "ee");
            pacing_log[i] = fopen(path, "w");
            if (pacing_log[i]) setvbuf(pacing_log[i], NULL, _IOFBF, 1 << 16);
        }
        if (pacing_log[0]) fprintf(pacing_log[0], "field,now,shown,due,start,work_ms,began\n");
        if (pacing_log[1]) fprintf(pacing_log[1], "due,began,ready,presented,refresh_phase\n");
#ifdef _WIN32
        LARGE_INTEGER freq, qpc;  /* the logs' clock, to set them against other QPC timestamps (DWM's vblanks) */
        if (prefix && QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&qpc))
            fprintf(stderr, "pacing: log seconds = QueryPerformanceCounter / %lld - %.7f\n", (long long)freq.QuadPart,
                    (double)qpc.QuadPart / (double)freq.QuadPart - host_wall_seconds());
#endif
    }
    return pacing_log[which];
}

void host_pacing_wait(void)
{
    const double period = FIELD_SECONDS;
    unsigned count = work_count, n = count < WORK_SAMPLES ? count : WORK_SAMPLES;
    double work = period;  /* until there are samples (no presenter: always), pace()'s timing */
    if (n >= 30) {
        /* Frames longer than a field (a load, a stall) are late however early they start, so they
           don't count, unless they are more than one in ten (a slow stretch: then no delay). */
        float worst = 0, field_ms = (float)(period * 1000);
        unsigned longer = 0;
        for (unsigned i = 0; i < n; i++) {
            float w = work_ms[i];
            if (w > field_ms) longer++;
            else if (w > worst) worst = w;
        }
        if (longer * 10 <= n) work = worst / 1000.0 + 0.0015;
    }
    double now = host_wall_seconds(), shown = jit_due;
    /* The presenter moved the grid if the picture handed now, or the one before it, was late. Its
       least spacing is the period, less pacing_slack while the pictures follow the display and both
       are on time (this one isn't known yet: on time is the least it can wait). */
    double last_due, last_at, spacing;
    int last_late;
#ifdef __aarch64__  /* arm64 reorders loads: the writer's order (below) is kept with fences */
    do {
        last_due = shown_due;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        last_at = shown_at, last_late = shown_late;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while (last_due != shown_due);
#else
    do last_due = shown_due, last_at = shown_at, last_late = shown_late; while (last_due != shown_due);
#endif
    spacing = period - (last_late ? 0 : pacing_slack);
    int previous = display.frames ? fabs(shown - last_due - period) < 0.03 * period  /* dues steered a little */
                                  : last_due + period - shown < 1e-6 && shown - last_due - period < 1e-6;
    if (shown && last_due == shown && last_at > shown) shown = last_at;
    else if (shown && last_due > 0 && previous && last_at + spacing > shown) shown = last_at + spacing;
    double due = shown + period;
    if (!shown || now > due) {  /* the first field, or more than a field late: start again from now */
        jit_due = now + (work < period ? work : period);
        frame_began = now;
        return;
    }
    due += display_steer(due);
    double start = due - work;
    if (start < shown) start = shown;  /* never two frames in one field */
    host_wait_wall(start);
    jit_due = due;
    frame_began = host_wall_seconds();
    FILE *log = pacing_file(0);
    if (log) fprintf(log, "%llu,%.6f,%.6f,%.6f,%.6f,%.3f,%.6f\n", (unsigned long long)pacing_field, now, shown,
                     due, start, work * 1000, frame_began);
}

/* The picture handed on at this VBLANK: when to show it (0: at once) and when its pad was read. */
void pacing_handed(double *due, double *began)
{
    *due = handed_due;
    *began = handed_began;
}

/* The presenter, just before it presents a picture pacing_handed described: notes how long the
   frame took, then waits for the due time, and at least a field after its last picture. */
void pacing_present(double due, double began)
{
    static double last_at;
    static int last_late;
    double now = host_wall_seconds(), target = due;
    if (due > 0) {
        if (began > 0 && now > began) {
            unsigned i = work_count;
            work_ms[i % WORK_SAMPLES] = (float)((now - began) * 1000);
            work_count = i + 1;
        }
        int late = now > due;  /* this or the last picture late: a whole period after the last */
        double spacing = FIELD_SECONDS - (late || last_late ? 0 : pacing_slack);
        if (last_at + spacing > target) target = last_at + spacing;
        if (target > now && target < now + 0.05)
            host_wait_wall(target);
        last_at = host_wall_seconds();
        last_late = late;
        shown_due = 0;  /* x86 keeps stores in order: a reader sees due 0 or all three values */
#ifdef __aarch64__  /* arm64 does not: fences keep it */
        __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
        shown_at = last_at;
        shown_late = late;
#ifdef __aarch64__
        __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
        shown_due = due;
    }
    FILE *log = pacing_file(1);
    if (log) {
        /* where in the display's refresh this picture went, from DWM's newest vblank (-1 unknown) */
        double at = host_wall_seconds(), refresh = display.refresh, phase = -1;
        if (refresh > 0 && display.vblank) phase = fmod(at - display.vblank, refresh) / refresh;
        if (refresh > 0 && display.vblank && phase < 0) phase += 1;  /* a vblank newer than the picture */
        fprintf(log, "%.6f,%.6f,%.6f,%.6f,%.4f\n", due, began, now, at, phase);
    }
    if (log && !(work_count % 60)) fflush(log), fflush(pacing_log[0]);  /* DHD_BENCH_EXIT ends with ExitProcess */
    /* DHD_PACING_STATS=1: every 600 pictures, how old the pad state was when each was presented. */
    static int stats = -1;
    static double sum, worst;
    static unsigned count;
    if (stats < 0) stats = host_getenv("DHD_PACING_STATS") != NULL;
    if (stats && began > 0) {
        double age = (host_wall_seconds() - began) * 1000;
        sum += age, count++;
        if (age > worst) worst = age;
        if (count == 600) {
            fprintf(stderr, "pacing: %s, pad to present mean %.2f ms max %.2f ms\n",
                    due > 0 ? "just-in-time" : "wait before VBLANK", sum / count, worst);
            sum = worst = 0, count = 0;
        }
    }
}

void host_pacing_config(double period, uint64_t field) { pacing_period=display_period(period); pacing_field=field; }
void host_pacing_handoff(int fresh) { handed_due=fresh ? jit_due : 0; handed_began=frame_began; }
void host_pacing_input(void) { frame_began=host_wall_seconds(); }

/* Native diagnostics consume published scalar state, never call the module
   from graphics, presenter or device OS threads. i686 keeps its direct clock. */
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
uint64_t kernel_vsync_count;
static double module_seconds;
double host_seconds(void) { return module_seconds; }
#endif
void host_frame_context(uint64_t field, double seconds) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    kernel_vsync_count=field; module_seconds=seconds;
#else
    (void)field; (void)seconds;
#endif
}

void host_frame_registers(uint32_t address) {
    memcpy(published_registers,GUEST(address),sizeof published_registers);
}

double host_pacing_period(void) { return pacing_period; }

#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
static double published_aspect;
static int published_flat_limit;
double host_widescreen_aspect(void) { return published_aspect; }
int host_widescreen_flat_limit(void) { return published_flat_limit; }
#endif
void host_frame_video(double aspect, int flat_limit) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    published_aspect=aspect; published_flat_limit=flat_limit;
#else
    (void)aspect; (void)flat_limit;
#endif
}

/* Guest-address-only module services. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_host_backtrace(void) { host_backtrace(); }
int host_service_host_game_options_read(uint32_t a0, uint32_t a1) { return host_game_options_read((char *)GUEST(a0), a1); }
