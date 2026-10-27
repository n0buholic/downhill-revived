/* The POSIX host of host_api.h (port/host): the module's OS services on Linux
   (and other POSIX hosts where the noted calls exist). Counterpart of
   host_win32.c; the module sees only host_api.h. Fibers, guest RAM, the FP
   mode, the guest-safe callback contracts and the thread name live in the
   host64 glue (fiber_posix.c, guest_memory*.c, fp_mode.c, the generated
   imports_gen.c, host.c), which provide those symbols. */
#define _GNU_SOURCE /* readlink, sigaction, pthread names */
#include "host_api.h"
#include "../include/guest.h"
#include "../net/net.h"
#include "../runtime/sha256.h"

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#include <x86intrin.h>
#elif defined(__aarch64__)
#else
#error Implement the POSIX host for this architecture
#endif

/* ---- environment, errors and the program ---- */
const char *host_getenv(const char *name) { return getenv(name); }

unsigned long host_last_error(void) { return (unsigned long)errno; }

void host_error_box(const char *title, const char *message)
{
#ifdef __ANDROID__
    void android_error_box(const char *title, const char *message);  /* android_main.c: logcat and a message box */
    fprintf(stderr, "%s: %s\n", title, message);
    android_error_box(title, message);
#else
    /* No message boxes on a headless host: the log carries it. */
    fprintf(stderr, "%s: %s\n", title, message);
    fflush(stderr);
#endif
}

unsigned host_exe_path(char *out, unsigned size)
{
#ifdef __ANDROID__
    /* No executable of our own (the app is app_process with libmain.so): the app's files folder
       stands for dhd.exe's (android_main.c), so dhd.ini, saves and dhd.log go there. */
    const char *android_exe_path(void);
    int k = snprintf(out, size, "%s", android_exe_path());
    return k > 0 && (unsigned)k < size ? (unsigned)k : 0;
#endif
    ssize_t n = readlink("/proc/self/exe", out, size ? size - 1 : 0);
    if (n < 0) {
        if (size) out[0] = 0;
        return 0;
    }
    out[n] = 0;
    return (unsigned)n;
}

void host_exit(int code)
{
#ifdef __ANDROID__
    extern int __llvm_profile_write_file(void) __attribute__((weak));  /* --pgo-generate builds only */
    if (__llvm_profile_write_file) __llvm_profile_write_file();
    void android_log_drain(void);  /* android_main.c: the last lines reach logcat and dhd.log */
    android_log_drain();
#endif
    _exit(code);
}

int host_display_hz(void) { return 0; }

/* The callers (vu0_interp.c, ps2asm.c) speak MXCSR: they read the word, set rounding toward zero
   with FTZ/DAZ ((c & ~0x603F) | 0xFFC0) and restore what they read. On arm64 the word is translated
   to and from FPCR: MXCSR RC (bits 13-14: nearest, down, up, zero) is FPCR RMode (bits 22-23:
   nearest, up, down, zero), FTZ or DAZ is FZ (bit 24, which flushes inputs and results), and every
   exception reads as masked (arm64 has no FP traps here). Writing the MXCSR bits into FPCR as they
   are (AND1) left RMode and FZ alone: VU1 on the vu thread (FPCR IEEE) then rounded to nearest with
   denormals, unlike the PS2 and the x64 build. */
#if defined(__aarch64__)
static const unsigned rc_to_rmode[4] = {0, 2, 1, 3}, rmode_to_rc[4] = {0, 2, 1, 3};
#endif

unsigned host_fp_csr(void)
{
#if defined(__x86_64__)
    return _mm_getcsr();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return 0x1F80u | rmode_to_rc[(v >> 22) & 3] << 13 | ((v >> 24) & 1 ? 0x8040u : 0);
#endif
}

void host_fp_set_csr(unsigned csr)
{
#if defined(__x86_64__)
    _mm_setcsr(csr);
#elif defined(__aarch64__)
    uint64_t now, v = (uint64_t)rc_to_rmode[(csr >> 13) & 3] << 22 | (csr & 0x8040u ? 1ull << 24 : 0);
    __asm__ volatile("mrs %0, fpcr" : "=r"(now));
    if (now != v) __asm__ volatile("msr fpcr, %0\n\tisb" :: "r"(v) : "memory");
#endif
}

uint64_t host_cycles(void)
{
#if defined(__x86_64__)
    return __rdtsc();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

void host_putenv(const char *text) { putenv((char *)text); }

void host_putenv_s(const char *name, const char *value)
{
    if (!value || !*value) unsetenv(name);
    else setenv(name, value, 1);
}

void host_dpi_aware(void) {} /* pixels are real without asking */

double host_monitor_aspect(void)
{
#ifdef __ANDROID__
    /* the screen's shape, from the activity (DhdActivity: DHD_MONITOR_ASPECT, the long side over the short) */
    const char *aspect = getenv("DHD_MONITOR_ASPECT");
    if (aspect && atof(aspect) >= 1.0 && atof(aspect) < 4.0) return atof(aspect);
#endif
    return 16.0 / 9.0;
}

/* ---- files: the disc image and the memory card's files ----
   A file is a C library FILE behind a small handle (1..FILE_SLOTS, 0 is none). */
#define FILE_SLOTS 32
static FILE *file_slot[FILE_SLOTS + 1];

static FILE *file_of(HostFile h) { return h > 0 && h <= FILE_SLOTS ? file_slot[h] : NULL; }

/* The game's own files (the memory card's saves\cardN folders, mc.c) are named with Windows
   backslashes. On Android they become real folders: each path below has them turned into slashes
   (other POSIX builds keep the names as they were). */
#ifdef __ANDROID__
static const char *posix_path(const char *path, char *buf, size_t size)
{
    if (!path || !strchr(path, '\\')) return path;
    snprintf(buf, size, "%s", path);
    for (char *c = buf; *c; c++)
        if (*c == '\\') *c = '/';
    return buf;
}
#define POSIX_PATH(path) char path##_buf[4096]; path = posix_path(path, path##_buf, sizeof path##_buf)
#else
#define POSIX_PATH(path) (void)0
#endif

HostFile host_file_open(const char *path, unsigned mode)
{
    POSIX_PATH(path);
    FILE *f;
    int slot;
    /* The module's WASI open shim has no preopened directories, so an absolute POSIX
       path arrives with its leading slash removed (observed: "/tmp/x" as "tmp/x");
       Windows paths have no leading slash and never hit this. A path that fails
       relative to the process directory is retried from the root; the retry only
       runs after the plain open already failed, so working opens are untouched. */
    char *alt = NULL;
    if (path && path[0] != '/' && !(mode & HOST_FILE_DISCARD)) {
        size_t n = strlen(path) + 2;
        alt = malloc(n);
        if (alt) {
            alt[0] = '/';
            memcpy(alt + 1, path, n - 1);
        }
    }
    if (mode & HOST_FILE_WRITE) {
        const char *kind = mode & HOST_FILE_TRUNCATE ? "w+b" : "r+b";
        const char *target = mode & HOST_FILE_DISCARD ? "/dev/null" : path;
        f = fopen(target, kind);
        if (!f && alt) f = fopen(alt, kind);
    } else {
        f = fopen(path, "rb");
        if (!f && alt) f = fopen(alt, "rb");
    }
    free(alt);
    if (!f) return HOST_FILE_NONE;
    for (slot = 1; slot <= FILE_SLOTS && file_slot[slot]; slot++)
        ;
    if (slot > FILE_SLOTS) {
        fclose(f);
        return HOST_FILE_NONE;
    }
    file_slot[slot] = f;
    return slot;
}

void host_file_close(HostFile h)
{
    FILE *f = file_of(h);
    if (f) {
        fclose(f);
        file_slot[h] = NULL;
    }
}

int64_t host_file_read(HostFile h, void *dst, size_t size)
{
    FILE *f = file_of(h);
    size_t n;
    if (!f) return -1;
    n = fread(dst, 1, size, f);
    return n < size && ferror(f) ? -1 : (int64_t)n;
}

size_t host_file_write(HostFile h, const void *src, size_t size)
{
    FILE *f = file_of(h);
    return f ? fwrite(src, 1, size, f) : 0;
}

int host_file_flush(HostFile h)
{
    FILE *f = file_of(h);
    return f ? fflush(f) : -1;
}

int host_file_seek(HostFile h, uint64_t offset)
{
    FILE *f = file_of(h);
    return f ? fseeko(f, (off_t)offset, SEEK_SET) : -1;
}

uint64_t host_file_size(HostFile h)
{
    FILE *f = file_of(h);
    off_t here, end;
    if (!f || (here = ftello(f)) < 0) return 0;
    if (fseeko(f, 0, SEEK_END) != 0) return 0;
    end = ftello(f);
    fseeko(f, here, SEEK_SET);
    return end < 0 ? 0 : (uint64_t)end;
}

/* ---- paths and folders: the memory card's folders ---- */
#define DIR_SLOTS 16
static DIR *dir_slot[DIR_SLOTS + 1];

/* A stat time as local time, as the card's directory entries show it. */
static void host_time_of(time_t t, HostTime *out)
{
    struct tm local;
    memset(out, 0, sizeof *out);
    if (!localtime_r(&t, &local)) return;
    out->year = (uint16_t)(local.tm_year + 1900);
    out->month = (uint16_t)(local.tm_mon + 1);
    out->day = (uint16_t)local.tm_mday;
    out->hour = (uint16_t)local.tm_hour;
    out->minute = (uint16_t)local.tm_min;
    out->second = (uint16_t)local.tm_sec;
}

static void dir_entry_of(const char *path, const struct dirent *fd, HostDirEntry *e)
{
    struct stat st;
    char full[4096];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", fd->d_name);
    snprintf(full, sizeof full, "%s/%s", path, fd->d_name);
    e->is_dir = fd->d_type == DT_DIR || (fd->d_type == DT_UNKNOWN && stat(full, &st) == 0 && S_ISDIR(st.st_mode));
    if (stat(full, &st) == 0) {
        e->size = (uint32_t)st.st_size;
        host_time_of(st.st_ctime, &e->created);
        host_time_of(st.st_mtime, &e->modified);
    }
}

/* The open directory remembers its path for entry stats. */
static char dir_path[DIR_SLOTS + 1][4096];

HostDir host_dir_open(const char *path, HostDirEntry *first)
{
    struct dirent *fd;
    DIR *d;
    int slot;
    POSIX_PATH(path);
    d = opendir(path);
    if (!d) return HOST_DIR_NONE;
    for (slot = 1; slot <= DIR_SLOTS && dir_slot[slot]; slot++)
        ;
    if (slot > DIR_SLOTS) {
        closedir(d);
        return HOST_DIR_NONE;
    }
    /* Skip nothing: "." and ".." are listed, as on Windows. */
    fd = readdir(d);
    if (!fd) {
        closedir(d);
        return HOST_DIR_NONE;
    }
    dir_slot[slot] = d;
    snprintf(dir_path[slot], sizeof dir_path[slot], "%s", path);
    dir_entry_of(path, fd, first);
    return slot;
}

int host_dir_next(HostDir d, HostDirEntry *e)
{
    struct dirent *fd;
    if (d <= 0 || d > DIR_SLOTS || !dir_slot[d]) return 0;
    fd = readdir(dir_slot[d]);
    if (!fd) return 0;
    dir_entry_of(dir_path[d], fd, e);
    return 1;
}

void host_dir_close(HostDir d)
{
    if (d > 0 && d <= DIR_SLOTS && dir_slot[d]) {
        closedir(dir_slot[d]);
        dir_slot[d] = NULL;
    }
}

int host_path_is_dir(const char *path)
{
    struct stat st;
    POSIX_PATH(path);
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int host_path_exists(const char *path)
{
    struct stat st;
    POSIX_PATH(path);
    return stat(path, &st) == 0;
}

int host_dir_create(const char *path)
{
    POSIX_PATH(path);
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 1 : 0;
}

/* ---- time and waiting: the wall clock (the game's own clock is virtual, kernel.c) ---- */
double host_wall_seconds(void)
{
    static struct timespec start;
    static int have;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!have) {
        start = t;
        have = 1;
    }
    return (double)(t.tv_sec - start.tv_sec) + (double)(t.tv_nsec - start.tv_nsec) / 1e9;
}

void host_sleep_ms(unsigned ms)
{
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* Sleep to a millisecond before the deadline, then use a CPU pause, not a
   scheduler yield that can surrender the remaining frame budget. */
void host_wait_wall(double seconds)
{
    double left = seconds - host_wall_seconds();
    if (left > 0.001) {
        struct timespec t = {(time_t)left, (long)((left - (time_t)left) * 1e9) - 1000000L};
        if (t.tv_nsec < 0) {
            t.tv_sec--;
            t.tv_nsec += 1000000000L;
        }
        if (t.tv_sec >= 0) nanosleep(&t, NULL);
    }
    while (host_wall_seconds() < seconds) {
#if defined(__x86_64__)
        _mm_pause();
#elif defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
}

/* ---- memory protection and access faults (graphics.c, deferred store-image reads) ---- */
int host_mem_protect(uint32_t at, uint32_t size, int protection)
{
    /* Page-align the range the way the OS requires; the caller's exact range
       keeps its protection inside it. */
    uintptr_t base = (uintptr_t)GUEST(at);
    uintptr_t lo = base & ~(uintptr_t)4095, hi = (base + size + 4095) & ~(uintptr_t)4095;
    if (hi < base) return 0;
    return mprotect((void *)lo, hi - lo, protection == HOST_MEM_NOACCESS ? PROT_NONE : PROT_READ | PROT_WRITE) == 0;
}

/* The access-fault guard: a SIGSEGV/SIGBUS handler for the thread that
   installed it. The module's handler commits the page and returns 1 (the
   faulting access is retried); 0 passes the fault to the previous handler
   (the guest allocator's reporter, which ends the process with the guest
   address). Only the installing thread is guarded. */
static _Thread_local int (*guard_on_fault)(uint32_t address);
static _Thread_local int guard_armed;
static struct sigaction guard_old_segv, guard_old_bus;
static pthread_mutex_t guard_installed_lock = PTHREAD_MUTEX_INITIALIZER;
static int guard_installed;

static void guard_fault(int sig, siginfo_t *info, void *ctx)
{
    uintptr_t host = (uintptr_t)info->si_addr;
    (void)ctx;
    if (guard_armed && guard_on_fault && host >= guest_base && host - guest_base < 0x100000000ull) {
        if (guard_on_fault((uint32_t)(host - guest_base))) return; /* retry the access */
    }
    /* Not ours, or not handled: run the previous handler. */
    struct sigaction *old = sig == SIGSEGV ? &guard_old_segv : &guard_old_bus;
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

int host_fault_guard(int (*on_fault)(uint32_t address))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = guard_fault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    pthread_mutex_lock(&guard_installed_lock);
    if (!guard_installed) {
        if (sigaction(SIGSEGV, &sa, &guard_old_segv) < 0 || sigaction(SIGBUS, &sa, &guard_old_bus) < 0) {
            pthread_mutex_unlock(&guard_installed_lock);
            return 0;
        }
        guard_installed = 1;
    }
    pthread_mutex_unlock(&guard_installed_lock);
    guard_on_fault = on_fault;
    guard_armed = 1;
    return 1;
}

int host_fault_guard_here(void) { return guard_armed; }

/* ---- threads: a fire-and-forget job (online_menu.c) ---- */
typedef struct {
    void (*fn)(void *);
    void *arg;
} DetachedStart;

static void *detached_run(void *p)
{
    DetachedStart start = *(DetachedStart *)p;
    free(p);
    start.fn(start.arg);
    return NULL;
}

int host_thread_detach(void (*fn)(void *), void *arg)
{
    DetachedStart *start = malloc(sizeof *start);
    pthread_t t;
    pthread_attr_t attr;
    if (!start) return 0;
    start->fn = fn;
    start->arg = arg;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, detached_run, start) != 0) {
        free(start);
        pthread_attr_destroy(&attr);
        return 0;
    }
    pthread_attr_destroy(&attr);
    return 1;
}

/* ---- a path's size and last write time (online.c's cache key) ----
   The size in bytes and the write time in 100 ns ticks since 1601 (Windows
   FILETIME); 1 if it exists. */
int host_path_info(const char *path, uint64_t *size, uint64_t *write_time)
{
    struct stat st;
    POSIX_PATH(path);
    if (stat(path, &st) != 0) return 0;
    *size = (uint64_t)st.st_size;
    *write_time = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    return 1;
}

/* dhd64's .text: the section of the image that holds the code, by its ELF
   program headers (the PE walk's counterpart in host_win32.c). */
void host_code_range(uintptr_t *lo, uintptr_t *hi)
{
    static uintptr_t base, extent;
    static int have;
    *lo = *hi = 0;
    if (!have) {
        char path[4096];
#ifdef __ANDROID__
        /* the code is libmain.so's, not the process's executable (app_process) */
        Dl_info self;
        ssize_t n = dladdr((void *)host_code_range, &self) && self.dli_fname
                    ? snprintf(path, sizeof path, "%s", self.dli_fname) : -1;
#else
        ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
#endif
        if (n > 0) {
            path[n] = 0;
            FILE *f = fopen(path, "rb");
            if (f) {
                Elf64_Ehdr eh;
                if (fread(&eh, 1, sizeof eh, f) == sizeof eh && memcmp(eh.e_ident, "\177ELF", 4) == 0) {
                    Elf64_Phdr ph;
                    if (fseeko(f, (off_t)eh.e_phoff, SEEK_SET) == 0) {
                        for (int i = 0; i < eh.e_phnum; i++) {
                            if (fread(&ph, 1, sizeof ph, f) != sizeof ph) break;
                            if (ph.p_type == PT_LOAD && (ph.p_flags & (PF_R | PF_X)) == (PF_R | PF_X)) {
                                Dl_info info;
                                if (dladdr((void *)host_code_range, &info) && info.dli_fbase) {
                                    base = (uintptr_t)info.dli_fbase + ph.p_vaddr;
                                    extent = ph.p_memsz;
                                    have = 1;
                                }
                                break;
                            }
                        }
                    }
                }
                fclose(f);
            }
        }
    }
    if (extent) {
        *lo = base;
        *hi = base + extent;
    }
}

/* The host process's memory regions (state_trace.c, opt-in: DHD_STATE_NORM=host). */
void host_memory_regions(void (*visit)(uintptr_t base, uintptr_t end, int host_memory))
{
    char self[4096], line[1024];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    FILE *maps;
    if (n > 0) self[n] = 0;
    else self[0] = 0;
    maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    while (fgets(line, sizeof line, maps)) {
        unsigned long lo, hi;
        char perms[8], path[512];
        int fields;
        path[0] = 0;
        fields = sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &lo, &hi, perms, path);
        if (fields < 3 || hi <= 0x10000) continue;
        if (lo < 0x10000) lo = 0x10000;
        if (hi <= lo) continue;
        /* The program's own image is not host memory (its AllocationBase on
           Windows); every other mapping is (anonymous, shared, special and
           other images alike). */
        int self_image = path[0] && !strcmp(path, self);
        visit((uintptr_t)lo, (uintptr_t)hi, !self_image);
    }
    fclose(maps);
}

/* ---- the heartbeat (autopad.c): a host thread calls beat() every 30 s ---- */
static void (*heartbeat_beat)(void);

static void *heartbeat_run(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec t = {30, 0};
        nanosleep(&t, NULL);
        heartbeat_beat();
    }
    return NULL;
}

void host_heartbeat_start(void (*beat)(void))
{
    pthread_t t;
    pthread_attr_t attr;
    heartbeat_beat = beat;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, heartbeat_run, NULL) == 0) pthread_detach(t);
    pthread_attr_destroy(&attr);
}

/* The owner-thread drain (imports_gen.c) cannot suspend itself to sample a
   host PC the way the i686 watchdog does; the generated boundary reports 0. */
uintptr_t host_heartbeat_eip(void) { return 0; }

void host_console_write(uint32_t address, uint32_t bytes)
{
    fwrite(GUEST(address), 1, bytes, stderr);
    fflush(stderr);
}

/* ---- async services: file SHA-256 and the lobby list (online_menu.c) ----
   The blocking part runs on a host thread; only results cross, as guest-address
   buffers the game thread copies at poll time. One job of each kind runs at a
   time. The mutex guards the state; the worker publishes its result before
   marking itself done, and the poller copies it before idling. */
static pthread_mutex_t async_lock = PTHREAD_MUTEX_INITIALIZER;
static int sha_job;  /* 0 idle, 1 working, 2 done, 3 failed */
static char sha_path[1024];
static uint8_t sha_digest[32];

static void *sha_run(void *unused)
{
    static uint8_t buf[1 << 20];
    Sha256 hash;
    FILE *f;
    int ok = 0;
    (void)unused;
    f = fopen(sha_path, "rb");
    if (f) {
        size_t n;
        sha256_init(&hash);
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&hash, buf, n);
        ok = !ferror(f);
        sha256_final(&hash, sha_digest);
        fclose(f);
    }
    pthread_mutex_lock(&async_lock);
    sha_job = ok ? 2 : 3;
    pthread_mutex_unlock(&async_lock);
    return NULL;
}

int host_service_file_sha256_start(uint32_t path)
{
    const char *at = GUEST(path);
    pthread_t t;
    pthread_attr_t attr;
    pthread_mutex_lock(&async_lock);
    if (sha_job) {
        pthread_mutex_unlock(&async_lock);
        return 0;
    }
    if (!path || strlen(at) >= sizeof sha_path) {
        pthread_mutex_unlock(&async_lock);
        return 0;
    }
    snprintf(sha_path, sizeof sha_path, "%s", at);
    sha_job = 1;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, sha_run, NULL) != 0) {
        sha_job = 0;
        pthread_attr_destroy(&attr);
        pthread_mutex_unlock(&async_lock);
        return 0;
    }
    pthread_attr_destroy(&attr);
    pthread_mutex_unlock(&async_lock);
    return 1;
}

int host_service_file_sha256_poll(uint32_t digest)
{
    int s;
    pthread_mutex_lock(&async_lock);
    s = sha_job;
    if (s == 2) {
        memcpy(GUEST(digest), sha_digest, sizeof sha_digest);
        sha_job = 0;
    } else if (s == 3) {
        sha_job = 0;
    }
    pthread_mutex_unlock(&async_lock);
    return s == 1 ? 0 : s == 2 ? 1 : -1;
}

/* The lobby fetch: entries published here, copied to the guest buffer at poll time. */
#define ASYNC_LOBBY_MAX 64
static int lobby_job;  /* 0 idle, 1 working, 2 done, 3 failed */
static NetLobbyInfo lobby_list[ASYNC_LOBBY_MAX];
static int lobby_max, lobby_count;

static void *lobby_run(void *unused)
{
    int n;
    (void)unused;
    n = net_list_lobbies(lobby_list, lobby_max);
    pthread_mutex_lock(&async_lock);
    if (n < 0) {
        lobby_job = 3;
    } else {
        lobby_count = n;
        lobby_job = 2;
    }
    pthread_mutex_unlock(&async_lock);
    return NULL;
}

int host_service_net_list_lobbies_start(int max)
{
    pthread_t t;
    pthread_attr_t attr;
    if (max <= 0 || max > ASYNC_LOBBY_MAX) return 0;
    pthread_mutex_lock(&async_lock);
    if (lobby_job) {
        pthread_mutex_unlock(&async_lock);
        return 0;
    }
    lobby_max = max;
    lobby_count = -1;
    lobby_job = 1;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, lobby_run, NULL) != 0) {
        lobby_job = 0;
        pthread_attr_destroy(&attr);
        pthread_mutex_unlock(&async_lock);
        return 0;
    }
    pthread_attr_destroy(&attr);
    pthread_mutex_unlock(&async_lock);
    return 1;
}

int host_service_net_list_lobbies_poll(uint32_t out, int max)
{
    int s, n;
    pthread_mutex_lock(&async_lock);
    s = lobby_job;
    n = lobby_count;
    if (s == 2) lobby_job = 0;
    else if (s == 3) lobby_job = 0;
    pthread_mutex_unlock(&async_lock);
    if (s == 1) return -2;
    if (s != 2) return -1;
    n = n < max ? n : max;
    if (n > 0) memcpy(GUEST(out), lobby_list, (size_t)n * sizeof lobby_list[0]);
    return n;
}
