// Reads /dev/shm/nuos_frames (nuos_ring.h): scans per-slot seqlocks and
// returns the newest stable BGRA slot each tick.
#ifndef NUOS_RING_READER_HPP
#define NUOS_RING_READER_HPP

#include <cstddef>
#include <cstdint>
#include "nuos_ring.h"
#include "nuos_shm.h"

namespace nuos {

struct RgbaView {          // BGRA8 pixel view into the mmap'd slot
    uint8_t* data;
    int width, height;
    size_t stride;
};

class RingReader {
public:
    ~RingReader();

    // Opens existing shm created by NuosBridge (O_RDWR so we can also create
    // it for standalone testing). Returns false if absent (stream idles).
    bool open(bool create_if_missing);

    // Newest stable slot as a BGRA view (wraps mmap region, no copy).
    // Returns false if none stable yet.
    bool latest(RgbaView& view, uint64_t& slot_ns);

    const nuos_ring_hdr_t* header() const { return hdr_; }

private:
    int fd_ = -1;
    void* map_ = nullptr;
    size_t map_len_ = 0;
    nuos_ring_hdr_t* hdr_ = nullptr;
};

} // namespace nuos
#endif
