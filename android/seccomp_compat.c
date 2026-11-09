/* Android runs apps under a seccomp filter that kills (SIGSYS, exit 159 from the shell) any syscall bionic
   does not use. The engine is static glibc, which tries newer syscalls first (clone3, faccessat2, rseq,
   statx, ...) and falls back when the kernel says ENOSYS. The filter traps instead of returning, so catch
   SIGSYS and make the trapped call return -ENOSYS: glibc then takes its fallback path. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

static void note(int nr) {
    /* async-signal-safe: report each syscall once, by number. */
    static volatile unsigned char seen[512];
    if (nr >= 0 && nr < 512) {
        if (seen[nr]) return;
        seen[nr] = 1;
    }
    char buf[80] = "hengevm: android blocked syscall ";
    char *p = buf + strlen(buf), d[12];
    int n = 0, v = nr < 0 ? -nr : nr;
    do d[n++] = '0' + v % 10; while (v /= 10);
    if (nr < 0) *p++ = '-';
    while (n) *p++ = d[--n];
    memcpy(p, "; using fallback\n", 17);
    p += 17;
    (void)!write(2, buf, p - buf);
}

static void on_sigsys(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    if (si->si_code != SYS_SECCOMP) return;
    ucontext_t *uc = ctx;
#if defined(__aarch64__)
    /* The kernel has already moved the PC past the svc; only the result is ours to set. */
    unsigned long long *r = uc->uc_mcontext.regs;
    if (si->si_syscall == 202 /* __NR_accept */) {
        /* bionic only uses accept4, so the filter traps plain accept; glibc's accept has no fallback
           (usbmuxd died on it: the restore OS was never seen). Same call, flags 0. */
        long ret;
        register long x0 __asm__("x0") = (long)r[0], x1 __asm__("x1") = (long)r[1], x2 __asm__("x2") = (long)r[2],
                      x3 __asm__("x3") = 0, x8 __asm__("x8") = 242 /* __NR_accept4 */;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
        ret = x0;
        r[0] = (unsigned long long)ret;
        return;
    }
    r[0] = (unsigned long long)(long long)-ENOSYS;
#elif defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
#endif
    note(si->si_syscall);
}

/* glibc registers rseq for every thread with signals blocked, which the filter on older Android kills;
   GLIBC_TUNABLES is read before any code of ours runs, so run again with it set (the app sets it itself). */
static void without_rseq(char **argv) {
    const char *t = getenv("GLIBC_TUNABLES");
    if (t && strstr(t, "glibc.pthread.rseq=0")) return;
    static char v[1024];
    if (t && *t) {
        size_t n = strlen(t);
        if (n + 32 > sizeof v) return;
        memcpy(v, t, n);
        memcpy(v + n, ":glibc.pthread.rseq=0", 22);
    } else {
        memcpy(v, "glibc.pthread.rseq=0", 21);
    }
    setenv("GLIBC_TUNABLES", v, 1);
    execv("/proc/self/exe", argv);
}

/* Before main and before any other constructor (curl, glib) can start a thread. */
__attribute__((constructor(101))) static void henge_seccomp_compat(int argc, char **argv) {
    (void)argc;
    without_rseq(argv);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);
}

/* glibc's pthread_create and posix_spawn call clone3 with every signal blocked, where a trapped syscall
   cannot be caught: the kernel kills the process instead. Take clone3's place in the static link so glibc
   never makes the call and goes straight to its clone fallback. */
struct clone_args;
int __clone3(struct clone_args *cl_args, unsigned long size, int (*func)(void *), void *arg) {
    (void)cl_args, (void)size, (void)func, (void)arg;
    errno = ENOSYS;
    return -1;
}
