// Ring shm producer/consumer seqlock-consistency test (threads).
#include "nuos_ring.h"
#include "nuos_shm.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <vector>

static nuos_ring_hdr_t* hdr = nullptr;
static uint8_t* slots_base = nullptr;

static void* producer(void* arg) {
    (void)arg;
    unsigned seed = 1234;
    for (uint64_t i = 1; i <= 2000; ++i) {
        int slot = (int)(hdr->write_seq % NUOS_RING_SLOTS);
        volatile uint32_t* seq = &hdr->slot_seq[slot];
        nuos_seqlock_begin_write(seq);
        uint8_t* base = slots_base + NUOS_RING_SLOT_OFF(slot);
        uint32_t val = (uint32_t)i;
        for (size_t b = 0; b < 4096; b += 4)
            memcpy(base + b, &val, 4);
        hdr->slot_ns[slot] = i; // timestamp == payload value (consistency check)
        nuos_seqlock_end_write(seq);
        hdr->write_seq++;
        if ((seed = seed * 1103515245 + 12345) % 4 == 0)
            usleep(100); // occasional jitter
    }
    return nullptr;
}

int main() {
    const char* name = "/nuos_ring_test";
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, NUOS_RING_SHM_SIZE) < 0) return 1;
    void* m = mmap(nullptr, NUOS_RING_SHM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return 1;
    hdr = (nuos_ring_hdr_t*)m;
    memset(hdr, 0, sizeof *hdr);
    hdr->magic = NUOS_SHM_RING_MAGIC;
    hdr->version = 1;
    hdr->width = 64; hdr->height = 32; hdr->stride = 256;
    hdr->format = NUOS_RING_FORMAT_BGRA8;
    slots_base = (uint8_t*)m;

    pthread_t prod;
    pthread_create(&prod, nullptr, producer, nullptr);

    std::atomic<int> errors{0};
    std::atomic<int> reads{0};
    // consumer thread: scan newest stable slot, verify slot_ns == payload
    for (int tick = 0; tick < 5000; ++tick) {
        int best = -1; uint64_t best_ns = 0;
        for (int i = 0; i < NUOS_RING_SLOTS; ++i) {
            uint32_t s1 = nuos_seqlock_begin_read(&hdr->slot_seq[i]);
            uint64_t ns = hdr->slot_ns[i];
            if (nuos_seqlock_end_read(&hdr->slot_seq[i], s1) == 0 && ns > best_ns) {
                best_ns = ns; best = i;
            }
        }
        if (best >= 0) {
            uint32_t s1 = nuos_seqlock_begin_read(&hdr->slot_seq[best]);
            uint8_t* base = slots_base + NUOS_RING_SLOT_OFF(best);
            uint32_t val;
            memcpy(&val, base, 4);
            bool ok = nuos_seqlock_end_read(&hdr->slot_seq[best], s1) == 0;
            if (ok) {
                ++reads;
                if (val != (uint32_t)best_ns) ++errors; // torn read of payload
            }
        }
        usleep(200);
    }
    pthread_join(prod, nullptr);

    if (reads.load() < 100) { fprintf(stderr, "too few stable reads\n"); return 1; }
    if (errors.load() != 0) { fprintf(stderr, "%d torn reads\n", errors.load()); return 1; }

    printf("ring_roundtrip_test: PASS (%d consistent reads)\n", reads.load());
    shm_unlink(name);
    return 0;
}
