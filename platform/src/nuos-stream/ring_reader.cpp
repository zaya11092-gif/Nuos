#include "ring_reader.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

namespace nuos {

RingReader::~RingReader() {
    if (map_) munmap(map_, map_len_);
    if (fd_ >= 0) close(fd_);
}

bool RingReader::open(bool create_if_missing) {
    int flags = create_if_missing ? (O_CREAT | O_RDWR) : O_RDWR;
    fd_ = shm_open(NUOS_SHM_RING_NAME, flags, 0666);
    if (fd_ < 0) return false;
    if (ftruncate(fd_, NUOS_RING_SHM_SIZE) < 0) return false;
    map_len_ = NUOS_RING_SHM_SIZE;
    map_ = mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (map_ == MAP_FAILED) { map_ = nullptr; return false; }
    hdr_ = (nuos_ring_hdr_t*)map_;

    // If we created it, initialize a valid empty header.
    if (hdr_->magic != NUOS_SHM_RING_MAGIC) {
        memset(hdr_, 0, sizeof *hdr_);
        hdr_->magic = NUOS_SHM_RING_MAGIC;
        hdr_->version = 1;
    }
    return true;
}

bool RingReader::latest(RgbaView& view, uint64_t& slot_ns) {
    if (!hdr_ || hdr_->magic != NUOS_SHM_RING_MAGIC) return false;
    int best = -1;
    uint64_t best_ns = 0;
    for (int i = 0; i < NUOS_RING_SLOTS; ++i) {
        uint32_t s1 = nuos_seqlock_begin_read(&hdr_->slot_seq[i]);
        uint64_t ns = hdr_->slot_ns[i];
        if (nuos_seqlock_end_read(&hdr_->slot_seq[i], s1) == 0 &&
            ns > best_ns && ns != 0) {
            best_ns = ns;
            best = i;
        }
    }
    if (best < 0) return false;
    int w = (int)hdr_->width, h = (int)hdr_->height;
    size_t stride = hdr_->stride ? hdr_->stride : (size_t)w * 4;
    if (w <= 0 || h <= 0 || w > NUOS_RING_MAX_W || h > NUOS_RING_MAX_H) return false;
    view.data = (uint8_t*)map_ + NUOS_RING_SLOT_OFF(best);
    view.width = w;
    view.height = h;
    view.stride = stride;
    slot_ns = best_ns;
    return true;
}

} // namespace nuos
