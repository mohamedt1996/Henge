#pragma once
/* Linux/Android stand-in for the one copyfile call host/share.c makes (a WebDAV COPY): a file, or a folder and
   everything in it, with modes and times. No extended attributes or ACLs: Android's storage keeps none. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef void *copyfile_state_t;
#define COPYFILE_ACL (1 << 0)
#define COPYFILE_STAT (1 << 1)
#define COPYFILE_XATTR (1 << 2)
#define COPYFILE_DATA (1 << 3)
#define COPYFILE_ALL (COPYFILE_ACL | COPYFILE_STAT | COPYFILE_XATTR | COPYFILE_DATA)
#define COPYFILE_RECURSIVE (1 << 15)

static inline int hv_copy_one(const char *from, const char *to, const struct stat *st) {
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st->st_mode & 07777);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    static __thread char buf[1 << 16];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            done += w;
        }
        if (rc) break;
    }
    int e = errno;
    struct timespec times[2] = {st->st_atim, st->st_mtim};
    futimens(out, times);
    close(in);
    if (close(out) != 0 && !rc) rc = -1, e = errno;
    errno = e;
    return rc;
}

static inline int hv_copy_tree(const char *from, const char *to, int depth) {
    struct stat st;
    if (lstat(from, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(from, target, sizeof target - 1);
        if (n < 0) return -1;
        target[n] = 0;
        return symlink(target, to);
    }
    if (!S_ISDIR(st.st_mode)) return hv_copy_one(from, to, &st);
    if (depth > 64) return errno = ELOOP, -1;
    if (mkdir(to, st.st_mode & 07777) != 0 && errno != EEXIST) return -1;
    DIR *d = opendir(from);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", from, e->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", to, e->d_name) >= (int)sizeof b) {
            errno = ENAMETOOLONG;
            rc = -1;
            break;
        }
        rc = hv_copy_tree(a, b, depth + 1);
    }
    closedir(d);
    return rc;
}

static inline int copyfile(const char *from, const char *to, copyfile_state_t state, unsigned flags) {
    (void)state;
    struct stat st;
    if (lstat(from, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode) && !(flags & COPYFILE_RECURSIVE)) return errno = EISDIR, -1;
    return hv_copy_tree(from, to, 0);
}
