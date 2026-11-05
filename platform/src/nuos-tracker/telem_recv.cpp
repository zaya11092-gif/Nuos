#include "telem_recv.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <cstdio>

namespace nuos {

static uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool TelemRecv::start(uint16_t port) {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd_, (sockaddr*)&addr, sizeof addr) < 0) {
        close(fd_); fd_ = -1;
        return false;
    }
    // non-blocking poll model: 1 ms timeout keeps the main loop at ~90 Hz
    timeval tv{0, 1000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return true;
}

void TelemRecv::stop() {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

bool TelemRecv::poll(const TelemHandlers& h) {
    if (fd_ < 0) return false;
    uint8_t buf[2048];
    ssize_t n = recvfrom(fd_, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= (ssize_t)sizeof(nuos_hdr_t)) return false;
    nuos_hdr_t hdr;
    memcpy(&hdr, buf, sizeof hdr);
    if (!nuos_hdr_valid(&hdr)) return false;
    size_t plen = (size_t)std::min<uint16_t>(hdr.len, (uint16_t)(n - sizeof hdr));

    switch (hdr.type) {
    case NUOS_PKT_POSE:
        if (plen >= sizeof(nuos_pose_t) && h.on_pose) {
            nuos_pose_t p;
            memcpy(&p, buf + sizeof hdr, sizeof p);
            last_pose_ns_ = mono_ns();
            ever_pose_ = true;
            h.on_pose(p);
        }
        return true;
    case NUOS_PKT_HANDS:
        if (plen >= sizeof(nuos_hands_t) && h.on_hands) {
            nuos_hands_t hs;
            memcpy(&hs, buf + sizeof hdr, sizeof hs);
            h.on_hands(buf + sizeof hdr, plen, hs);
        }
        return true;
    case NUOS_PKT_STAT:
        if (plen >= sizeof(nuos_stat_t)) {
            memcpy(&stat_, buf + sizeof hdr, sizeof stat_);
            if (h.on_stat) h.on_stat(stat_);
        }
        return true;
    default:
        return false;
    }
}

bool TelemRecv::arkit_alive(uint64_t now_ns) const {
    return ever_pose_ && now_ns - last_pose_ns_ < 500000000ull;
}

} // namespace nuos
