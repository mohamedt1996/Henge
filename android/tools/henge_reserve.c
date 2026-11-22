/* henge-reserve: holds the Mac's RAM for Gunyah, in 2 MB pages, taken while the tablet's memory is still whole.

   Gunyah takes the Mac's memory only as 2 MB pages (a parcel of 4 KB pages reset the tablet), and once Android has
   run a while its free memory is cut up: the tablet gave 186 of 2048 MB in 2 MB pages (0.3.74). So this runs at boot
   (the henge-gunyah Magisk module's service.sh) or when the app asks, takes the memory once, pins it, and hands the
   same memory to each engine run as a memfd over an abstract unix socket. Ending it gives the memory back.

     henge-reserve start MB      the daemon (stays in the foreground: the caller backgrounds it)
     henge-reserve status        prints "reserved MB HUGE_MB PID", or "none"
     henge-reserve release       the daemon frees the memory and exits
     henge-reserve get           (test) receives the descriptor and prints its size

   Root only: the socket answers only uid 0 (the engine runs as root on Gunyah, the app asks through su). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCK_NAME "henge-gunyah-ram"
#define STATUS "/data/local/tmp/henge-gunyah-reserve.txt"
#define TWO (2ull << 20)

static void put_file(const char *path, const char *v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void)!write(fd, v, strlen(v));
    close(fd);
}

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    FILE *f = fopen(STATUS, "a");
    if (f) {
        fprintf(f, "%s %s\n", when, line);
        fclose(f);
        chmod(STATUS, 0644);
    }
    fprintf(stderr, "henge-reserve: %s\n", line);
}

/* MB of [p, p+len) mapped by PMDs (2 MB pages), from this process's smaps. */
static uint64_t huge_mb(void *p, uint64_t len) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return 0;
    char line[256];
    int in = 0;
    uint64_t kb = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx ", &lo, &hi) == 2 && strchr(line, '-') < strchr(line, ' ')) {
            in = lo < (uintptr_t)p + len && hi > (uintptr_t)p;
            continue;
        }
        unsigned long v;
        if (in && (sscanf(line, "ShmemPmdMapped: %lu kB", &v) == 1 || sscanf(line, "FilePmdMapped: %lu kB", &v) == 1)) kb += v;
    }
    fclose(f);
    return kb >> 10;
}

static int connect_daemon(void) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    memcpy(a.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCK_NAME));
    if (s < 0 || connect(s, (struct sockaddr *)&a, len) < 0) {
        if (s >= 0) close(s);
        return -1;
    }
    return s;
}

static int ask(const char *cmd, char *reply, size_t cap, int *fd_out) {
    int s = connect_daemon();
    if (s < 0) return -1;
    (void)!write(s, cmd, strlen(cmd));
    char ctl[CMSG_SPACE(sizeof(int))];
    struct iovec io = {.iov_base = reply, .iov_len = cap - 1};
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl};
    ssize_t n = recvmsg(s, &m, MSG_CMSG_CLOEXEC);
    close(s);
    if (n <= 0) return -1;
    reply[n] = 0;
    if (fd_out) {
        *fd_out = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd_out, CMSG_DATA(c), sizeof(int));
    }
    return 0;
}

static int serve(uint64_t mb) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    memcpy(a.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCK_NAME));
    if (s < 0 || bind(s, (struct sockaddr *)&a, alen) < 0) {
        if (errno == EADDRINUSE) { fprintf(stderr, "henge-reserve: already running\n"); return 0; }
        status("FAILED: socket (%s)", strerror(errno));
        return 1;
    }
    listen(s, 4);
    uint64_t size = mb << 20;
    status("reserving %llu MB in 2 MB pages", (unsigned long long)mb);

    /* The kernel made to work for 2 MB runs: huge pages on advice for shmem, compaction on a fault in advised
       memory and in the background, compaction preferred to reclaim; then caches dropped and memory compacted. */
    put_file("/sys/kernel/mm/transparent_hugepage/shmem_enabled", "advise");
    put_file("/sys/kernel/mm/transparent_hugepage/enabled", "madvise");
    put_file("/sys/kernel/mm/transparent_hugepage/defrag", "madvise");
    put_file("/sys/kernel/mm/transparent_hugepage/khugepaged/defrag", "1");
    put_file("/proc/sys/vm/compaction_proactiveness", "100");
    put_file("/proc/sys/vm/extfrag_threshold", "100");
    put_file("/proc/sys/vm/drop_caches", "3");
    put_file("/proc/sys/vm/compact_memory", "1");
    struct rlimit nolimit = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &nolimit);

    /* Sealable, and sealed against resizing: /dev/udmabuf (Zero-copy) takes only such a memfd's pages. */
    int fd = (int)syscall(SYS_memfd_create, "henge-ram", 1u /* MFD_CLOEXEC */ | 2u /* MFD_ALLOW_SEALING */);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) { status("FAILED: memfd (%s)", strerror(errno)); return 1; }
    fcntl(fd, 1033 /* F_ADD_SEALS */, 2 /* F_SEAL_SHRINK */ | 4 /* F_SEAL_GROW */);
    uint8_t *res = mmap(NULL, size + TWO, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { status("FAILED: address space (%s)", strerror(errno)); return 1; }
    uint8_t *at = (uint8_t *)(((uintptr_t)res + TWO - 1) & ~(uintptr_t)(TWO - 1));
    uint8_t *w = mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (w == MAP_FAILED) { status("FAILED: mapping (%s)", strerror(errno)); return 1; }
    madvise(w, size, MADV_HUGEPAGE);
    for (uint64_t o = 0; o < size; o += TWO) {
        uint64_t n = size - o < TWO ? size - o : TWO;
        if (madvise(w + o, n, 23 /* MADV_POPULATE_WRITE */) < 0) memset(w + o, 0, n);
        madvise(w + o, n, 25 /* MADV_COLLAPSE */);
        if ((o & ((256ull << 20) - 1)) == 0 && o) put_file("/proc/sys/vm/compact_memory", "1");
    }
    uint64_t h = huge_mb(w, size);
    for (int pass = 0; pass < 4 && h * 1000 < mb * 999; pass++) {
        put_file("/proc/sys/vm/drop_caches", "3");
        put_file("/proc/sys/vm/compact_memory", "1");
        for (uint64_t o = 0; o < size; o += TWO) madvise(w + o, size - o < TWO ? size - o : TWO, 25 /* MADV_COLLAPSE */);
        h = huge_mb(w, size);
    }
    /* Pinned: a shmem page swapped out comes back as 4 KB pages, and Gunyah needs the 2 MB ones. */
    if (mlock(w, size) < 0) status("pinning failed (%s): it may be swapped and split", strerror(errno));
    status("reserved %llu MB, %llu MB of it in 2 MB pages%s (pid %d)", (unsigned long long)mb, (unsigned long long)h,
           h * 1000 >= mb * 999 ? "" : ": not all; the Gunyah engine needs it all (reboot and reserve at boot)", (int)getpid());

    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR) continue; break; }
        struct ucred cr;
        socklen_t crl = sizeof cr;
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &crl) < 0 || cr.uid != 0) { close(c); continue; }
        char cmd[32] = {0};
        ssize_t n = read(c, cmd, sizeof cmd - 1);
        if (n <= 0) { close(c); continue; }
        char reply[128];
        if (!strncmp(cmd, "get", 3)) {
            h = huge_mb(w, size);
            int len = snprintf(reply, sizeof reply, "ok %llu %llu\n", (unsigned long long)size, (unsigned long long)h);
            char ctl[CMSG_SPACE(sizeof(int))];
            memset(ctl, 0, sizeof ctl);
            struct iovec io = {.iov_base = reply, .iov_len = (size_t)len};
            struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl};
            struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
            cm->cmsg_level = SOL_SOCKET, cm->cmsg_type = SCM_RIGHTS, cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &fd, sizeof(int));
            sendmsg(c, &m, 0);
            status("handed to pid %d (%llu MB in 2 MB pages)", (int)cr.pid, (unsigned long long)h);
        } else if (!strncmp(cmd, "status", 6)) {
            int len = snprintf(reply, sizeof reply, "reserved %llu %llu %d\n", (unsigned long long)mb,
                               (unsigned long long)huge_mb(w, size), (int)getpid());
            (void)!write(c, reply, (size_t)len);
        } else if (!strncmp(cmd, "release", 7)) {
            (void)!write(c, "released\n", 9);
            close(c);
            status("released: %llu MB back to Android (an engine still running keeps its copy until it ends)", (unsigned long long)mb);
            return 0;
        }
        close(c);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "start")) {
        uint64_t mb = strtoull(argv[2], NULL, 10);
        if (mb < 256 || mb > 8192) { fprintf(stderr, "henge-reserve: %llu MB? (256-8192)\n", (unsigned long long)mb); return 2; }
        return serve(mb);
    }
    char reply[128];
    int fd = -1;
    if (argc >= 2 && !strcmp(argv[1], "status")) {
        if (ask("status\n", reply, sizeof reply, NULL) < 0) { puts("none"); return 1; }
        fputs(reply, stdout);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "release")) {
        if (ask("release\n", reply, sizeof reply, NULL) < 0) { puts("none"); return 1; }
        fputs(reply, stdout);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "get")) {
        if (ask("get\n", reply, sizeof reply, &fd) < 0 || fd < 0) { puts("none"); return 1; }
        struct stat st;
        fstat(fd, &st);
        printf("%s(descriptor of %lld bytes)\n", reply, (long long)st.st_size);
        return 0;
    }
    fprintf(stderr, "usage: henge-reserve start MB | status | release | get\n");
    return 2;
}
