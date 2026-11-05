#include "hands_shm.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace nuos {

HandsShm::~HandsShm() {
    if (shm_) munmap(shm_, sizeof(nuos_hands_shm_t));
    if (fd_ >= 0) close(fd_);
}

bool HandsShm::open() {
    fd_ = shm_open(NUOS_SHM_HANDS_NAME, O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) return false;
    if (ftruncate(fd_, sizeof(nuos_hands_shm_t)) < 0) return false;
    shm_ = (nuos_hands_shm_t*)mmap(nullptr, sizeof(nuos_hands_shm_t),
                                   PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (shm_ == MAP_FAILED) { shm_ = nullptr; return false; }
    memset(shm_, 0, sizeof *shm_);
    shm_->magic = NUOS_SHM_MAGIC_HANDS;
    shm_->version = 1;
    return true;
}

void HandsShm::publish(const nuos_hands_t& pkt, uint64_t board_mono_ns) {
    if (!shm_) return;
    nuos_hands_shm_t* p = shm_;
    nuos_seqlock_begin_write(&p->seq);
    p->present = pkt.present;
    p->t_mono_ns = board_mono_ns;
    memcpy(p->left, pkt.left, sizeof p->left);
    memcpy(p->right, pkt.right, sizeof p->right);
    nuos_seqlock_end_write(&p->seq);
}

} // namespace nuos
