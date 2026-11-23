/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The Mac's RAM in memory (HV_RAM_MEMFD, devices/vmapple.c), handed from the engine to the app as an open
   file, not as a name. The engine's memfd has no name the app may open: /proc/<engine>/fd/<n> is an open()
   of a tmpfs file, which Android's SELinux policy gives no app (an app's tmpfs files are read, written and
   mapped through descriptors it was passed, never opened; the tablet, 0.3.22 and 0.3.23: the app's GPU was
   refused, left its note, and every later start of the build put the RAM back in a file on flash). A
   descriptor passed over a Unix socket (SCM_RIGHTS) is exactly what the policy allows.

     engine  hv_ram_fd_serve(fd, name, cap)   listens on an abstract socket "@henge-ram-<pid>" (no file, so
                                              nothing to label, hand over or clean up) and gives the
                                              descriptor to whoever connects, for as long as it runs
     app     hv_ram_fd_take(name)             connects and receives it

   The name goes where the RAM file's did (HvReimsHeader.ram_file); a leading '@' tells the two apart. */
#ifndef HV_RAM_FD_H
#define HV_RAM_FD_H

#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/xattr.h>
#endif

static inline socklen_t hv_ram_fd_address(struct sockaddr_un *a, const char *name) {
    size_t n = strlen(name + 1); /* past the '@' */
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (n > sizeof a->sun_path - 1) n = sizeof a->sun_path - 1;
    memcpy(a->sun_path + 1, name + 1, n); /* sun_path[0] = 0: the abstract namespace */
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

typedef struct { int listener, fd; } HvRamFdServer;

#ifndef SO_PEERSEC
#define SO_PEERSEC 31
#endif

/* HV_RAM_FD_RELABEL (the Gunyah engine, root): the RAM is a memfd made by root (henge-reserve, or this engine
   under su), whose SELinux label no app may read or map; the kernel then drops the descriptor from the message
   (security_file_receive) and the app gets the byte alone (EPROTO; 0.3.103's "guest RAM @henge-ram-N: Protocol
   error"). Root relabels the file, once a peer, to an app's own tmpfs type at that app's exact level (an app
   writes only objects of its own categories): what a memfd the app made itself would carry. No policy change. */
static inline void hv_ram_fd_relabel(int fd, int c) {
#ifdef __linux__
    static int logged;
    if (!getenv("HV_RAM_FD_RELABEL")) return;
    char peer[256] = {0}, now[256] = {0}, want[300];
    socklen_t pl = sizeof peer - 1;
    if (getsockopt(c, SOL_SOCKET, SO_PEERSEC, peer, &pl) != 0) {
        if (!logged++) fprintf(stderr, "hengevm: reims: the app's SELinux context: %s; the RAM keeps its label\n", strerror(errno));
        return;
    }
    /* "u:r:untrusted_app:s0:c149,c256,c512,c768": the level is what follows the third ':' */
    const char *lvl = peer;
    for (int k = 0; k < 3 && lvl; k++) lvl = strchr(lvl, ':'), lvl = lvl ? lvl + 1 : NULL;
    if (!lvl || !*lvl) return;
    snprintf(want, sizeof want, "u:object_r:appdomain_tmpfs:%s", lvl);
    ssize_t nl = fgetxattr(fd, "security.selinux", now, sizeof now - 1);
    if (nl > 0 && !strcmp(now, want)) return;
    int r = fsetxattr(fd, "security.selinux", want, strlen(want) + 1, 0);
    int e = errno;
    char after[256] = {0};
    fgetxattr(fd, "security.selinux", after, sizeof after - 1);
    if (logged++ < 4)
        fprintf(stderr, "hengevm: reims: the Mac's RAM relabelled for the app (%s): %s -> %s%s%s\n", peer,
                nl > 0 ? now : "?", r == 0 ? after : want, r == 0 ? "" : ": FAILED, ", r == 0 ? "" : strerror(e));
#else
    (void)fd, (void)c;
#endif
}

static inline void *hv_ram_fd_server(void *arg) {
    HvRamFdServer s = *(HvRamFdServer *)arg;
    for (;;) {
        int c = accept(s.listener, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return NULL;
        }
        hv_ram_fd_relabel(s.fd, c);
        char byte = 'R';
        union { struct cmsghdr h; char space[CMSG_SPACE(sizeof(int))]; } control;
        memset(&control, 0, sizeof control);
        struct iovec iov = {&byte, 1};
        struct msghdr msg = {0};
        msg.msg_iov = &iov, msg.msg_iovlen = 1;
        msg.msg_control = control.space, msg.msg_controllen = sizeof control.space;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET, cm->cmsg_type = SCM_RIGHTS, cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &s.fd, sizeof(int));
        ssize_t sent;
        do sent = sendmsg(c, &msg, MSG_NOSIGNAL); while (sent < 0 && errno == EINTR);
        if (sent < 0) fprintf(stderr, "hengevm: reims: handing the Mac's RAM to the app: %s\n", strerror(errno));
        close(c);
    }
}

/* The engine: serve `fd` under a name written to `name` ("@henge-ram-<pid>"). 0, or -1 with errno. */
static inline int hv_ram_fd_serve(int fd, char *name, size_t cap) {
    static HvRamFdServer server;
    struct sockaddr_un a;
    snprintf(name, cap, "@henge-ram-%d", (int)getpid());
    int l = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (l < 0) return -1;
    if (bind(l, (struct sockaddr *)&a, hv_ram_fd_address(&a, name)) != 0 || listen(l, 4) != 0) {
        int e = errno;
        close(l);
        errno = e;
        return -1;
    }
    server = (HvRamFdServer){l, fd};
    pthread_t t;
    if (pthread_create(&t, NULL, hv_ram_fd_server, &server) != 0) {
        close(l);
        errno = EAGAIN;
        return -1;
    }
    pthread_detach(t);
    return 0;
}

/* The app: the descriptor the engine serves under `name` (close-on-exec), or -1 with errno. */
static inline int hv_ram_fd_take(const char *name) {
    struct sockaddr_un a;
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c < 0) return -1;
    if (connect(c, (struct sockaddr *)&a, hv_ram_fd_address(&a, name)) != 0) {
        int e = errno;
        close(c);
        errno = e;
        return -1;
    }
    char byte = 0;
    union { struct cmsghdr h; char space[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof control);
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {0};
    msg.msg_iov = &iov, msg.msg_iovlen = 1;
    msg.msg_control = control.space, msg.msg_controllen = sizeof control.space;
    ssize_t got;
    do got = recvmsg(c, &msg, MSG_CMSG_CLOEXEC); while (got < 0 && errno == EINTR);
    /* The byte with no descriptor and MSG_CTRUNC: the kernel refused the app the file (its SELinux label). */
    int e = got < 0 ? errno : (got == 1 && (msg.msg_flags & MSG_CTRUNC)) ? EACCES : EPROTO, fd = -1;
    close(c);
    if (got == 1 && byte == 'R')
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS && cm->cmsg_len >= CMSG_LEN(sizeof(int)) && fd < 0)
                memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    if (fd < 0) errno = e;
    return fd;
}

#endif
