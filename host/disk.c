#include <fcntl.h>
#include <errno.h>
#include "../core/machine.h"
#include "disk.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

#define DISK_IO_CHUNK (128u << 20)

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly) {
    d->wfd = -1;
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (d->fd < 0) return -1;
    struct stat st;
    if (fstat(d->fd, &st) != 0 || (uint64_t)st.st_size < offset) {
        close(d->fd);
        d->fd = -1;
        return -1;
    }
    d->offset = offset;
    d->size = (uint64_t)st.st_size - offset;
    d->readonly = readonly;
    d->dirty = 1; /* what an earlier run left unflushed is not known */
    d->flushes_spared = 0;
    /* HV_WRITE_CACHE=0: writes skip the host's cache (F_NOCACHE), so a long run of them cannot pile up
       gigabytes of dirty pages; reads stay cached, and the kernel keeps the two coherent. Off by default:
       on the Mac bench big writes took 2-3x as long that way (2 GB + sync 4-6 s -> 9-17 s) and everyday
       work was no faster. The phone's installer turns it on for the restore, which writes ~15 GB at once. */
    const char *wc = getenv("HV_WRITE_CACHE");
    if (!readonly && wc && !atoi(wc)) {
        d->wfd = open(path, O_RDWR);
#ifdef F_NOCACHE
        if (d->wfd >= 0 && fcntl(d->wfd, F_NOCACHE, 1) == -1) {
#else /* Linux/Android: O_DIRECT needs aligned buffers, so through the cache there */
        if (d->wfd >= 0) {
#endif
            close(d->wfd);
            d->wfd = -1;
        }
    }
    return 0;
}

void hv_disk_close(HvDisk *d) {
    if (d->fd >= 0) close(d->fd);
    if (d->wfd >= 0) close(d->wfd);
    d->fd = d->wfd = -1;
}

int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        ssize_t n = pread(d->fd, p, count, (off_t)(d->offset + off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    if (d->readonly || off > d->size || len > d->size - off) return -1;
    __atomic_store_n(&d->dirty, 1, __ATOMIC_RELEASE);
    const uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        ssize_t n = pwrite(d->wfd >= 0 ? d->wfd : d->fd, p, count, (off_t)(d->offset + off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

/* HV_FLUSH: 0 fsync; 1 nothing here, what was written is safe in the host's cache as long as the
   host runs; 2 fcntl(F_BARRIERFSYNC). */
int hv_disk_flush(HvDisk *d) {
    if (d->readonly) return 0;
    /* Nothing written since the last flush: the file already has everything, and a sync of flash costs
       milliseconds with every other request held behind it (virtio_blk.c's fence). macOS flushes in runs. */
    if (!__atomic_exchange_n(&d->dirty, 0, __ATOMIC_ACQ_REL)) {
        __atomic_fetch_add(&d->flushes_spared, 1, __ATOMIC_RELAXED);
        return 0;
    }
    int r;
    switch (hv_knob[HV_K_FLUSH]) {
    case 1: return 0;
    #ifdef __linux__
    case 2: r = fdatasync(d->fd); break;
#else
    case 2: r = fcntl(d->fd, F_BARRIERFSYNC) == -1 ? -1 : 0; break;
#endif
    default: r = fsync(d->fd); break;
    }
    if (r != 0) __atomic_store_n(&d->dirty, 1, __ATOMIC_RELEASE);
    return r;
}
