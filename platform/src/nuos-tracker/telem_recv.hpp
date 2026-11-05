// Single-owner UDP receiver on 48011 (phone -> board: POSE/HANDS/STAT).
// The tracker owns this socket exclusively (see proto/BRIDGE_API.md).
#ifndef NUOS_TELEM_RECV_HPP
#define NUOS_TELEM_RECV_HPP

#include <cstdint>
#include <functional>
#include "nuos_proto.h"

namespace nuos {

struct TelemHandlers {
    std::function<void(const nuos_pose_t&)>       on_pose;
    std::function<void(const uint8_t* /*raw*/, size_t /*len*/,
                       const nuos_hands_t&)>      on_hands; // raw for verbatim fwd
    std::function<void(const nuos_stat_t&)>       on_stat;
};

class TelemRecv {
public:
    bool start(uint16_t port = NUOS_PORT_TELEMETRY);
    void stop();

    // Poll the socket once; dispatches at most one packet to handlers.
    // Returns true if a packet was dispatched.
    bool poll(const TelemHandlers& h);

    bool arkit_alive(uint64_t now_ns) const;   // seen POSE within 500 ms
    uint64_t last_pose_ns() const { return last_pose_ns_; }
    const nuos_stat_t& last_stat() const { return stat_; }

private:
    int fd_ = -1;
    uint64_t last_pose_ns_ = 0;
    bool ever_pose_ = false;
    nuos_stat_t stat_{};
};

} // namespace nuos
#endif
