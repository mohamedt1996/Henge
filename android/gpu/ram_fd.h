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
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static inline socklen_t hv_ram_fd_address(struct sockaddr_un *a, const char *name) {
    size_t n = strlen(name + 1); /* past the '@' */
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (n > sizeof a->sun_path - 1) n = sizeof a->sun_path - 1;
    memcpy(a->sun_path + 1, name + 1, n); /* sun_path[0] = 0: the abstract namespace */
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

typedef struct { int listener, fd; } HvRamFdServer;

static inline void *hv_ram_fd_server(void *arg) {
    HvRamFdServer s = *(HvRamFdServer *)arg;
    for (;;) {
        int c = accept(s.listener, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return NULL;
        }
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
    int e = got < 0 ? errno : EPROTO, fd = -1;
    close(c);
    if (got == 1 && byte == 'R')
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS && cm->cmsg_len >= CMSG_LEN(sizeof(int)) && fd < 0)
                memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    if (fd < 0) errno = e;
    return fd;
}

#endif
