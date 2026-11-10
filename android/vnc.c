/* A small RFB (VNC) server for the Android runner: shows the guest's display and
   sends the viewer's pointer and keys to the virtual Mac. One viewer at a time,
   no authentication (bind is 127.0.0.1 unless HV_VNC_ANY is set), raw encoding.
   The picture is the boot framebuffer (logo, progress, verbose boot) until a
   GPU backend draws the desktop. */
#include "vnc.h"
#include "vm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BLANK_W 1024
#define BLANK_H 768

typedef struct {
    int fd;
    int bpp;                       /* 32 or 16 */
    int big_endian;
    unsigned rmax, gmax, bmax, rshift, gshift, bshift;
    int want_update, incremental;
    uint32_t w, h;                 /* what the viewer was told */
    int buttons;                   /* last RFB button mask */
    uint64_t pressed_at;           /* ms: when a button last went down */
} Client;

static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k <= 0) return -1;
        b += k, n -= (size_t)k;
    }
    return 0;
}

static int recv_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t k = recv(fd, b, n, 0);
        if (k <= 0) return -1;
        b += k, n -= (size_t)k;
    }
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8, p[1] = v & 0xff; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v & 0xff; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* The current picture: BGRA rows, or NULL before the guest has a display. */
static int current_frame(const uint8_t **px, uint32_t *w, uint32_t *h, uint32_t *stride) {
    HvBootDisplay d;
    if (hv_vm_boot_display(&d) == 0 && d.pixels && d.width && d.height) {
        *px = d.pixels, *w = d.width, *h = d.height, *stride = d.stride ? d.stride : d.width * 4;
        return 0;
    }
    return -1;
}

static int send_frame(Client *c) {
    const uint8_t *src = NULL;
    uint32_t w = BLANK_W, h = BLANK_H, stride = 0;
    int have = current_frame(&src, &w, &h, &stride) == 0;
    uint8_t hdr[16 + 12];
    size_t hn = 4;
    hdr[0] = 0, hdr[1] = 0;
    int resize = w != c->w || h != c->h;
    put16(hdr + 2, resize ? 2 : 1);
    if (resize) { /* DesktopSize pseudo-rectangle */
        put16(hdr + hn, 0), put16(hdr + hn + 2, 0), put16(hdr + hn + 4, (uint16_t)w), put16(hdr + hn + 6, (uint16_t)h);
        put32(hdr + hn + 8, (uint32_t)-223);
        hn += 12;
        c->w = w, c->h = h;
    }
    put16(hdr + hn, 0), put16(hdr + hn + 2, 0), put16(hdr + hn + 4, (uint16_t)w), put16(hdr + hn + 6, (uint16_t)h);
    put32(hdr + hn + 8, 0); /* raw */
    hn += 12;
    if (send_all(c->fd, hdr, hn)) return -1;
    size_t bpp = (size_t)c->bpp / 8, row = (size_t)w * bpp;
    uint8_t *out = malloc(row);
    if (!out) return -1;
    int rc = 0;
    for (uint32_t y = 0; y < h && !rc; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t b = 0, g = 0, r = 0;
            if (have) {
                const uint8_t *s = src + (size_t)y * stride + (size_t)x * 4;
                b = s[0], g = s[1], r = s[2];
            }
            uint32_t v = (r * c->rmax / 255) << c->rshift | (g * c->gmax / 255) << c->gshift | (b * c->bmax / 255) << c->bshift;
            uint8_t *o = out + x * bpp;
            if (bpp == 4) {
                if (c->big_endian) o[0] = v >> 24, o[1] = v >> 16, o[2] = v >> 8, o[3] = (uint8_t)v;
                else o[0] = (uint8_t)v, o[1] = v >> 8, o[2] = v >> 16, o[3] = v >> 24;
            } else {
                if (c->big_endian) o[0] = v >> 8, o[1] = (uint8_t)v;
                else o[0] = (uint8_t)v, o[1] = v >> 8;
            }
        }
        rc = send_all(c->fd, out, row);
    }
    free(out);
    return rc;
}

/* X11 keysym -> Linux input code (what virtio-input carries). */
static int keysym_code(uint32_t k) {
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    if (k >= 'A' && k <= 'Z') k += 32;
    for (int i = 0; row1[i]; i++) if (k == (uint32_t)row1[i]) return 16 + i;
    for (int i = 0; row2[i]; i++) if (k == (uint32_t)row2[i]) return 30 + i;
    for (int i = 0; row3[i]; i++) if (k == (uint32_t)row3[i]) return 44 + i;
    if (k >= '1' && k <= '9') return 2 + (int)(k - '1');
    switch (k) {
    case '0': case ')': return 11;
    case '!': return 2; case '@': return 3; case '#': return 4; case '$': return 5; case '%': return 6;
    case '^': return 7; case '&': return 8; case '*': return 9; case '(': return 10;
    case '-': case '_': return 12; case '=': case '+': return 13;
    case '[': case '{': return 26; case ']': case '}': return 27;
    case ';': case ':': return 39; case '\'': case '"': return 40; case '`': case '~': return 41;
    case '\\': case '|': return 43; case ',': case '<': return 51; case '.': case '>': return 52;
    case '/': case '?': return 53; case ' ': return 57;
    case 0xff08: return 14;   /* BackSpace */
    case 0xff09: return 15;   /* Tab */
    case 0xff0d: return 28;   /* Return */
    case 0xff1b: return 1;    /* Escape */
    case 0xffff: return 111;  /* Delete */
    case 0xff50: return 102;  /* Home */
    case 0xff57: return 107;  /* End */
    case 0xff55: return 104;  /* Page Up */
    case 0xff56: return 109;  /* Page Down */
    case 0xff51: return 105;  /* Left */
    case 0xff52: return 103;  /* Up */
    case 0xff53: return 106;  /* Right */
    case 0xff54: return 108;  /* Down */
    case 0xffe1: return 42;   /* Shift_L */
    case 0xffe2: return 54;   /* Shift_R */
    case 0xffe3: return 29;   /* Control_L */
    case 0xffe4: return 97;   /* Control_R */
    case 0xffe9: case 0xff7e: return 56;   /* Alt_L / Mode_switch: Option */
    case 0xffea: return 100;  /* Alt_R */
    case 0xffe7: case 0xffeb: return 125;  /* Meta_L / Super_L: Command */
    case 0xffe8: case 0xffec: return 126;  /* Meta_R / Super_R */
    case 0xffe5: return 58;   /* Caps Lock */
    }
    if (k >= 0xffbe && k <= 0xffc7) return 59 + (int)(k - 0xffbe); /* F1-F10 */
    if (k == 0xffc8) return 87;
    if (k == 0xffc9) return 88;
    return -1;
}

/* ---- The pointer ----
   The Mac's mouse is relative, and macOS accelerates it; the viewer's pointer is a place on the screen.
   One thread walks the Mac's pointer to that place: it sends a step, waits until the guest's cursor has
   answered it (hv_vm_pointer: the display's own cursor position), and only then measures again. Sending
   the whole remaining distance at every viewer event, as this did, applied it several times over before
   the guest had moved once, so the pointer overshot, was pulled back, and shook around the finger. */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int x, y, have;  /* where the viewer wants the pointer */
    int settled;     /* the Mac's pointer is there (or as near as it gets) */
    int started;
    unsigned relative; /* counts a mouse's own moves: where the pointer was last sent blind no longer holds */
} P = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void pointer_settled(int x, int y) {
    pthread_mutex_lock(&P.mu);
    if (P.x == x && P.y == y) P.settled = 1; /* else the viewer moved on meanwhile */
    pthread_mutex_unlock(&P.mu);
}

#define POINTER_NEAR 6      /* within this the step is the distance itself */
#define POINTER_STEP 300    /* the longest step */
#define POINTER_ANSWER_MS 500 /* a slow guest answers late: a second step on top of an unanswered one is the shake */
#define POINTER_BLIND_MS 3000
#define POINTER_QUIET_MS 40 /* the guest's cursor has stopped: over two of its frames without a move */

static void *pointer_thread(void *unused) {
    (void)unused;
    int ox = 0, oy = 0, have_o = 0;      /* the last place sent blind */
    int waiting = 0, gx0 = 0, gy0 = 0;   /* a step is out: where the guest was when it left */
    int sx = 0, sy = 0, silent = 0;
    int lx = 0, ly = 0, moving = 0;      /* the guest's cursor as last seen moving, for the quiet wait */
    uint64_t moved_at = 0, said_at = 0;
    unsigned steps = 0, answers = 0, silences = 0;
    uint64_t sent_at = 0, blind_until = 0;
    unsigned relative = 0;
    double gain = 1;                     /* guest pixels per unit sent, as last seen */
    for (;;) {
        pthread_mutex_lock(&P.mu);
        while (!P.have || P.settled) pthread_cond_wait(&P.cv, &P.mu);
        int tx = P.x, ty = P.y;
        if (P.relative != relative) relative = P.relative, have_o = 0, waiting = 0;
        pthread_mutex_unlock(&P.mu);
        uint64_t now = now_ms();
        int gx, gy;
        if (now < blind_until || hv_vm_pointer(&gx, &gy) != 0) {
            /* No cursor to measure against (the boot screen, or a guest that is not answering):
               the viewer's own movement, as it is. */
            if (have_o) hv_vm_mouse_move(tx - ox, ty - oy);
            ox = tx, oy = ty, have_o = 1, waiting = 0;
            pointer_settled(tx, ty);
            continue;
        }
        if (waiting) {
            int answered = gx != gx0 || gy != gy0;
            if (!answered && now - sent_at < POINTER_ANSWER_MS) { usleep(2000); continue; }
            /* The guest may answer one step in pieces (X, then Y; or over a few of its frames): the step is
               answered once its cursor has stood still for a moment, not at the first move. Measuring at the
               first piece sent the rest again on top of it, which is the shake. */
            if (answered) {
                if (gx != lx || gy != ly || !moving) lx = gx, ly = gy, moved_at = now, moving = 1;
                if (now - moved_at < POINTER_QUIET_MS && now - sent_at < POINTER_ANSWER_MS) { usleep(2000); continue; }
            }
            moving = 0;
            if (answered) {
                silent = 0, answers++;
                double sent = sqrt((double)sx * sx + (double)sy * sy);
                double went = sqrt((double)(gx - gx0) * (gx - gx0) + (double)(gy - gy0) * (gy - gy0));
                if (sent >= 6) {
                    double g = went / sent;
                    g = g < 0.3 ? 0.3 : g > 8 ? 8 : g;
                    gain = 0.3 * gain + 0.7 * g;
                }
            } else if (silences++, ++silent >= 3) {
                /* Steps go out and the cursor never moves: stop steering by it for a while. */
                silent = 0, blind_until = now + POINTER_BLIND_MS;
                ox = tx, oy = ty, have_o = 1, waiting = 0;
                pointer_settled(tx, ty);
                continue;
            }
            waiting = 0;
        }
        int ex = tx - gx, ey = ty - gy;
        if (abs(ex) <= 1 && abs(ey) <= 1) {
            ox = tx, oy = ty, have_o = 1;
            pointer_settled(tx, ty);
            continue;
        }
        int big = abs(ex) > abs(ey) ? abs(ex) : abs(ey);
        double scale;
        if (big <= POINTER_NEAR) scale = gain > 1 ? 1 / gain : 1;
        else {
            scale = 0.75 / gain;
            if (scale > 2) scale = 2;
            if (big * scale > POINTER_STEP) scale = (double)POINTER_STEP / big;
        }
        sx = (int)lround(ex * scale), sy = (int)lround(ey * scale);
        if (!sx && abs(ex) > 1) sx = ex > 0 ? 1 : -1;
        if (!sy && abs(ey) > 1) sy = ey > 0 ? 1 : -1;
        hv_vm_mouse_move(sx, sy);
        waiting = 1, moving = 0, gx0 = gx, gy0 = gy, sent_at = now, steps++;
        /* What the pointer is doing, every two seconds while it is being walked: for the log. */
        if (now - said_at >= 2000) {
            said_at = now;
            fprintf(stderr, "hengevm: pointer: to %d,%d from %d,%d, step %d,%d; %u steps, %u answered, %u unanswered, gain %.2f\n",
                    tx, ty, gx, gy, sx, sy, steps, answers, silences, gain);
        }
    }
    return NULL;
}

static void pointer_target(int x, int y) {
    pthread_mutex_lock(&P.mu);
    if (!P.started) {
        pthread_t t;
        if (pthread_create(&t, NULL, pointer_thread, NULL) == 0) pthread_detach(t), P.started = 1;
    }
    if (!P.have || P.x != x || P.y != y) P.x = x, P.y = y, P.have = 1, P.settled = 0;
    pthread_cond_signal(&P.cv);
    pthread_mutex_unlock(&P.mu);
}

/* A button changes where the pointer is, not where it was heading: wait (briefly) for it to arrive. */
static void pointer_wait(int ms) {
    for (uint64_t until = now_ms() + (uint64_t)ms; now_ms() < until; usleep(2000)) {
        pthread_mutex_lock(&P.mu);
        int there = P.settled || !P.started;
        pthread_mutex_unlock(&P.mu);
        if (there) return;
    }
}

/* The viewer's own message 250 (Henge's app, a captured mouse): the mouse's movement as it is, with the
   RFB button mask. Nothing steers: the pointer thread is told there is no place to walk to. */
static void buttons_event(Client *c, int mask);

static void relative_event(Client *c, int mask, int dx, int dy) {
    pthread_mutex_lock(&P.mu);
    P.have = 0, P.settled = 1, P.relative++;
    pthread_mutex_unlock(&P.mu);
    if (dx || dy) hv_vm_mouse_move(dx, dy);
    buttons_event(c, mask);
}

static void pointer_event(Client *c, int mask, int x, int y) {
    pointer_target(x, y);
    if ((mask ^ c->buttons) & 7) pointer_wait(800);
    buttons_event(c, mask);
}

static void buttons_event(Client *c, int mask) {
    static const int map[3] = {0, 2, 1}; /* RFB: 1 left, 2 middle, 4 right */
    if ((mask ^ c->buttons) & 7) {
        /* macOS misses a press let go at once: a tap holds for a moment. */
        uint64_t held = now_ms() - c->pressed_at;
        if ((c->buttons & 7 & ~mask) && held < 60) usleep((useconds_t)(60 - held) * 1000);
        if (mask & 7 & ~c->buttons) c->pressed_at = now_ms();
    }
    for (int b = 0; b < 3; b++) {
        int was = c->buttons >> b & 1, now = mask >> b & 1;
        if (was != now) hv_vm_mouse_button(map[b], now);
    }
    if (mask & 8) hv_vm_mouse_wheel(1);
    if (mask & 16) hv_vm_mouse_wheel(-1);
    if (mask & 32) hv_vm_mouse_hwheel(-1);
    if (mask & 64) hv_vm_mouse_hwheel(1);
    c->buttons = mask & 7;
}

static int handshake(Client *c) {
    char ver[12];
    if (send_all(c->fd, "RFB 003.008\n", 12) || recv_all(c->fd, ver, 12)) return -1;
    int minor = atoi(ver + 8);
    if (minor >= 7) {
        uint8_t types[2] = {1, 1}, chosen; /* one type: None */
        if (send_all(c->fd, types, 2) || recv_all(c->fd, &chosen, 1) || chosen != 1) return -1;
    } else {
        uint8_t t[4];
        put32(t, 1);
        if (send_all(c->fd, t, 4)) return -1;
    }
    if (minor >= 8) {
        uint8_t ok[4] = {0};
        if (send_all(c->fd, ok, 4)) return -1;
    }
    uint8_t shared;
    if (recv_all(c->fd, &shared, 1)) return -1;
    const uint8_t *px;
    uint32_t w = BLANK_W, h = BLANK_H, stride;
    current_frame(&px, &w, &h, &stride);
    static const char name[] = "Henge";
    uint8_t init[24 + sizeof name - 1] = {0};
    put16(init, (uint16_t)w), put16(init + 2, (uint16_t)h);
    init[4] = 32, init[5] = 24, init[6] = 0, init[7] = 1;             /* bpp, depth, little endian, true colour */
    put16(init + 8, 255), put16(init + 10, 255), put16(init + 12, 255);
    init[14] = 16, init[15] = 8, init[16] = 0;                        /* BGRA in memory */
    put32(init + 20, sizeof name - 1);
    memcpy(init + 24, name, sizeof name - 1);
    c->w = w, c->h = h, c->bpp = 32, c->big_endian = 0;
    c->rmax = c->gmax = c->bmax = 255, c->rshift = 16, c->gshift = 8, c->bshift = 0;
    return send_all(c->fd, init, sizeof init);
}

static void serve(Client *c) {
    if (handshake(c)) return;
    struct timespec last = {0};
    for (;;) {
        struct pollfd pf = {.fd = c->fd, .events = POLLIN};
        int r = poll(&pf, 1, 50);
        if (r < 0) return;
        if (r > 0) {
            uint8_t type;
            if (recv_all(c->fd, &type, 1)) return;
            uint8_t b[20];
            switch (type) {
            case 0: /* SetPixelFormat */
                if (recv_all(c->fd, b, 19)) return;
                if (b[3] != 32 && b[3] != 16) return; /* 8-bit colour maps: not supported */
                c->bpp = b[3], c->big_endian = b[5];
                c->rmax = get16(b + 7), c->gmax = get16(b + 9), c->bmax = get16(b + 11);
                c->rshift = b[13], c->gshift = b[14], c->bshift = b[15];
                break;
            case 2: { /* SetEncodings: we always send raw (+ DesktopSize) */
                if (recv_all(c->fd, b, 3)) return;
                for (unsigned n = get16(b + 1); n; n--)
                    if (recv_all(c->fd, b, 4)) return;
                break;
            }
            case 3: /* FramebufferUpdateRequest */
                if (recv_all(c->fd, b, 9)) return;
                c->want_update = 1, c->incremental = b[0];
                break;
            case 4: /* KeyEvent */
                if (recv_all(c->fd, b, 7)) return;
                { int code = keysym_code(get32(b + 3)); if (code >= 0) hv_vm_key((uint16_t)code, b[0]); }
                break;
            case 5: /* PointerEvent */
                if (recv_all(c->fd, b, 5)) return;
                pointer_event(c, b[0], get16(b + 1), get16(b + 3));
                break;
            case 250: /* Henge's app: a captured mouse's movement (relative_event) */
                if (recv_all(c->fd, b, 5)) return;
                relative_event(c, b[0], (int16_t)get16(b + 1), (int16_t)get16(b + 3));
                break;
            case 6: /* ClientCutText */
                if (recv_all(c->fd, b, 7)) return;
                for (uint32_t n = get32(b + 3); n; n--)
                    if (recv_all(c->fd, b, 1)) return;
                break;
            default:
                return;
            }
        }
        if (!c->want_update) continue;
        /* Incremental requests: about 10 frames a second; raw full frames are what this sends. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
        if (c->incremental && ms < 100) continue;
        if (send_frame(c)) return;
        c->want_update = 0, last = now;
    }
}

static void *vnc_thread(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fprintf(stderr, "hengevm: vnc viewer connected\n");
        Client c = {.fd = fd};
        serve(&c);
        close(fd);
        fprintf(stderr, "hengevm: vnc viewer left\n");
    }
    return NULL;
}

int hv_vnc_start(int port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(getenv("HV_VNC_ANY") ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) {
        close(ls);
        return -1;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, vnc_thread, (void *)(intptr_t)ls)) {
        close(ls);
        return -1;
    }
    pthread_detach(t);
    return 0;
}
