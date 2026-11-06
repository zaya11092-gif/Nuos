/* nuos-frameudp — bridges the XR home (Godot) to the frame ring.
 * Listens on TCP 127.0.0.1:48200 for a single client. Frame wire format:
 *   [u32 w][u32 h][u32 len][BGRA8 pixels, len == w*h*4]
 * Each complete frame is written into /dev/shm/nuos_frames (nuos_ring.h).
 * Build deps: none beyond POSIX. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "nuos_ring.h"
#include "nuos_shm.h"

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int ring_fd = -1;
static void *ring_map = NULL;
static nuos_ring_hdr_t *hdr = NULL;

static int ring_create(void) {
    ring_fd = shm_open(NUOS_SHM_RING_NAME, O_CREAT | O_RDWR, 0666);
    if (ring_fd < 0) { perror("shm_open"); return -1; }
    if (ftruncate(ring_fd, NUOS_RING_SHM_SIZE) < 0) { perror("ftruncate"); return -1; }
    ring_map = mmap(NULL, NUOS_RING_SHM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, ring_fd, 0);
    if (ring_map == MAP_FAILED) { perror("mmap"); return -1; }
    hdr = (nuos_ring_hdr_t *)ring_map;
    if (hdr->magic != NUOS_SHM_RING_MAGIC) {
        memset(hdr, 0, sizeof *hdr);
        hdr->magic = NUOS_SHM_RING_MAGIC;
        hdr->version = 1;
        hdr->format = NUOS_RING_FORMAT_BGRA8;
        hdr->stride = 0; /* per-frame */
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return -1; /* peer closed */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static void handle_client(int cfd) {
    uint32_t w, h, len;
    static uint32_t slot = 0;
    for (;;) {
        if (read_full(cfd, &w, 4) || read_full(cfd, &h, 4) || read_full(cfd, &len, 4))
            return;
        w = ntohl(w); h = ntohl(h); len = ntohl(len);
        if (w == 0 || h == 0 || w > NUOS_RING_MAX_W || h > NUOS_RING_MAX_H ||
            len != w * h * 4) {
            fprintf(stderr, "frameudp: bad frame %ux%u len %u\n", w, h, len);
            return;
        }
        uint32_t slot_i = slot++ % NUOS_RING_SLOTS;
        uint8_t *dst = (uint8_t *)ring_map + NUOS_RING_SLOT_OFF(slot_i);
        if (read_full(cfd, dst, len)) return;
        /* Godot sends RGBA8; ring contract is BGRA8 — swap R/B in place. */
        for (uint32_t i = 0; i < len; i += 4) {
            uint8_t t = dst[i + 0]; dst[i + 0] = dst[i + 2]; dst[i + 2] = t;
        }
        nuos_seqlock_begin_write(&hdr->slot_seq[slot_i]);
        hdr->width = w; hdr->height = h;
        hdr->stride = w * 4;
        hdr->format = NUOS_RING_FORMAT_BGRA8;
        hdr->slot_ns[slot_i] = now_ns();
        nuos_seqlock_end_write(&hdr->slot_seq[slot_i]);
    }
}

int main(void) {
    if (ring_create() < 0) return 1;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(48200);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    listen(lfd, 1);
    fprintf(stderr, "frameudp: listening on 127.0.0.1:48200\n");
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        int one2 = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one2, sizeof one2);
        fprintf(stderr, "frameudp: client connected\n");
        handle_client(cfd);
        close(cfd);
        fprintf(stderr, "frameudp: client disconnected\n");
    }
}
