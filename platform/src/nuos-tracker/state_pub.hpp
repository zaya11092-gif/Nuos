// Tracker outputs:
//  - seqlock write to /dev/shm/nuos_pose (every fused sample, ~90 Hz)
//  - POSE-format nuos_pose_t packet to 127.0.0.1:48100 at 10 Hz
//  - verbatim HANDS forwarding to 127.0.0.1:48101
#ifndef NUOS_STATE_PUB_HPP
#define NUOS_STATE_PUB_HPP

#include <cstdint>
#include "nuos_shm.h"
#include "nuos_proto.h"
#include "fusion.hpp"

namespace nuos {

class StatePub {
public:
    ~StatePub();

    bool open();  // shm_open+ftruncate+mmap pose shm, create UDP send sockets

    // Publish a fused sample to shm.
    void publish_pose(const FusedState& s);

    // 10 Hz state packet to localhost:48100 (nuos_pose_t payload).
    void send_state_10hz(const FusedState& s, uint64_t now_ns);

    // Forward a raw hands datagram (payload bytes after header) verbatim.
    void forward_hands(const uint8_t* raw, size_t len, uint16_t seq);

private:
    int pose_fd_ = -1;
    nuos_pose_shm_t* pose_shm_ = nullptr;
    int fd_state_ = -1;   // -> 48100
    int fd_hands_ = -1;   // -> 48101
    uint64_t last_state_ns_ = 0;
};

} // namespace nuos
#endif
