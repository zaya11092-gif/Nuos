#include "state_pub.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>

namespace nuos {

StatePub::~StatePub() {
    if (pose_shm_) munmap(pose_shm_, sizeof(nuos_pose_shm_t));
    if (pose_fd_ >= 0) close(pose_fd_);
    if (fd_state_ >= 0) close(fd_state_);
    if (fd_hands_ >= 0) close(fd_hands_);
}

bool StatePub::open() {
    pose_fd_ = shm_open(NUOS_SHM_POSE_NAME, O_CREAT | O_RDWR, 0666);
    if (pose_fd_ < 0) { perror("shm_open nuos_pose"); return false; }
    if (ftruncate(pose_fd_, sizeof(nuos_pose_shm_t)) < 0) {
        perror("ftruncate"); return false;
    }
    pose_shm_ = (nuos_pose_shm_t*)mmap(nullptr, sizeof(nuos_pose_shm_t),
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       pose_fd_, 0);
    if (pose_shm_ == MAP_FAILED) { pose_shm_ = nullptr; return false; }
    memset(pose_shm_, 0, sizeof *pose_shm_);
    pose_shm_->magic = NUOS_SHM_MAGIC_POSE;
    pose_shm_->version = 1;

    fd_state_ = socket(AF_INET, SOCK_DGRAM, 0);
    fd_hands_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_state_ < 0 || fd_hands_ < 0) return false;

    // widen seqlock window: store-release semantics via volatile + memcpy;
    // single-writer/many-reader per nuos_shm.h contract.
    return true;
}

void StatePub::publish_pose(const FusedState& s) {
    if (!pose_shm_) return;
    nuos_pose_shm_t* p = pose_shm_;
    nuos_seqlock_begin_write(&p->seq);
    p->tracking = s.tracking;
    p->kiyo_ok  = s.kiyo_ok;
    p->arkit_ok = s.arkit_ok;
    p->t_mono_ns = s.t_mono_ns;
    memcpy(p->pos, s.pos, sizeof p->pos);
    for (int i = 0; i < 4; ++i) p->quat[i] = (float)s.quat[i];
    for (int i = 0; i < 3; ++i) p->vel[i] = (float)s.vel[i];
    for (int i = 0; i < 3; ++i) p->ang_vel[i] = (float)s.ang_vel[i];
    nuos_seqlock_end_write(&p->seq);
}

static void send_udp(int fd, uint16_t port, const void* data, size_t len) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    sendto(fd, data, len, 0, (sockaddr*)&addr, sizeof addr);
}

void StatePub::send_state_10hz(const FusedState& s, uint64_t now_ns) {
    if (now_ns - last_state_ns_ < 100000000ull) return; // 10 Hz
    last_state_ns_ = now_ns;
    nuos_hdr_t hdr;
    nuos_hdr_init(&hdr, NUOS_PKT_POSE);
    nuos_pose_t pkt{};
    pkt.t_mono_ns = s.t_mono_ns;
    for (int i = 0; i < 3; ++i) pkt.pos[i] = (float)s.pos[i];
    for (int i = 0; i < 4; ++i) pkt.quat[i] = (float)s.quat[i];
    for (int i = 0; i < 3; ++i) pkt.vel[i] = (float)s.vel[i];
    for (int i = 0; i < 3; ++i) pkt.ang_vel[i] = (float)s.ang_vel[i];
    pkt.tracking = s.tracking;
    hdr.len = sizeof pkt;
    uint8_t buf[sizeof hdr + sizeof pkt];
    memcpy(buf, &hdr, sizeof hdr);
    memcpy(buf + sizeof hdr, &pkt, sizeof pkt);
    send_udp(fd_state_, NUOS_LOCAL_PORT_STATE, buf, sizeof buf);
}

void StatePub::forward_hands(const uint8_t* raw, size_t len, uint16_t seq) {
    if (len == 0) return;
    nuos_hdr_t hdr;
    nuos_hdr_init(&hdr, NUOS_PKT_HANDS);
    hdr.seq = seq;
    hdr.len = (uint16_t)len;
    uint8_t buf[sizeof hdr + 516];
    size_t total = sizeof hdr + len;
    if (total > sizeof buf) return;
    memcpy(buf, &hdr, sizeof hdr);
    memcpy(buf + sizeof hdr, raw, len);
    send_udp(fd_hands_, NUOS_LOCAL_PORT_HANDS, buf, total);
}

} // namespace nuos
