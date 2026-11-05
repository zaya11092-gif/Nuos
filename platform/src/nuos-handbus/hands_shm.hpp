// /dev/shm/nuos_hands seqlock writer (creator per proto/BRIDGE_API.md).
#ifndef NUOS_HANDS_SHM_HPP
#define NUOS_HANDS_SHM_HPP

#include <cstdint>
#include <ctime>

#include "nuos_proto.h"
#include "nuos_shm.h"

namespace nuos {

class HandsShm {
public:
    ~HandsShm();
    bool open();  // shm_open /nuos_hands, create if missing
    void publish(const nuos_hands_t& pkt, uint64_t board_mono_ns);

private:
    int fd_ = -1;
    nuos_hands_shm_t* shm_ = nullptr;
};

} // namespace nuos
#endif
