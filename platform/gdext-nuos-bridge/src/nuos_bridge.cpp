#include "nuos_bridge.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/core/math.hpp>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <ctime>

using namespace godot;

static uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void NuosBridge::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_head_pose"), &NuosBridge::get_head_pose);
    ClassDB::bind_method(D_METHOD("get_hands"), &NuosBridge::get_hands);
    ClassDB::bind_method(D_METHOD("get_stats"), &NuosBridge::get_stats);
    ClassDB::bind_method(D_METHOD("push_frame", "image"), &NuosBridge::push_frame);
    ClassDB::bind_method(D_METHOD("poll_tick"), &NuosBridge::poll_tick);
}

int NuosBridge::open_shm(const char* name, size_t size, bool create, void** map) {
    int fd = shm_open(name, create ? (O_CREAT | O_RDWR) : O_RDONLY,
                      create ? 0666 : 0);
    if (fd < 0) return -1;
    if (create && ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    void* m = mmap(nullptr, size, create ? (PROT_READ | PROT_WRITE) : PROT_READ,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    *map = m;
    return fd;
}

void NuosBridge::teardown_shm() {
    auto un = [](int fd, void* m, size_t sz) {
        if (m) munmap(m, sz);
        if (fd >= 0) close(fd);
    };
    un(pose_fd_, pose_shm_, sizeof(nuos_pose_shm_t));
    un(hands_fd_, hands_shm_, sizeof(nuos_hands_shm_t));
    if (ring_map_) munmap(ring_map_, NUOS_RING_SHM_SIZE);
    pose_fd_ = hands_fd_ = ring_fd_ = -1;
    pose_shm_ = nullptr; hands_shm_ = nullptr; ring_map_ = nullptr;
}

NuosBridge::~NuosBridge() { teardown_shm(); }

void NuosBridge::_ready() {
    // read-only mappings; both may be absent (flat/debug mode keeps working)
    pose_fd_ = open_shm(NUOS_SHM_POSE_NAME, sizeof(nuos_pose_shm_t), false,
                        (void**)&pose_shm_);
    hands_fd_ = open_shm(NUOS_SHM_HANDS_NAME, sizeof(nuos_hands_shm_t), false,
                         (void**)&hands_shm_);

    auto* t = memnew(Timer);
    t->set_wait_time(0.1); // 10 Hz
    t->connect("timeout", Callable(this, "poll_tick"));
    add_child(t);
    t->start();
}

void NuosBridge::_exit_tree() { teardown_shm(); }

void NuosBridge::poll_tick() {
    // stats snapshot is merged into the state packet upstream; nothing to do
    // per tick beyond keeping mappings alive (kept minimal for v1).
}

Dictionary NuosBridge::get_head_pose() {
    Dictionary d;
    Vector3 pos;
    Quaternion quat;
    uint64_t t_ns = 0;
    int tracking = 0;
    bool kiyo_ok = false, arkit_ok = false;

    if (pose_shm_) {
        for (int tries = 0; tries < 8; ++tries) {
            uint32_t s1 = nuos_seqlock_begin_read(&pose_shm_->seq);
            if (pose_shm_->magic != NUOS_SHM_MAGIC_POSE) break;
            t_ns = pose_shm_->t_mono_ns;
            tracking = pose_shm_->tracking;
            kiyo_ok = pose_shm_->kiyo_ok != 0;
            arkit_ok = pose_shm_->arkit_ok != 0;
            pos = Vector3(pose_shm_->pos[0], pose_shm_->pos[1], pose_shm_->pos[2]);
            quat = Quaternion(pose_shm_->quat[1], pose_shm_->quat[2],
                              pose_shm_->quat[3], pose_shm_->quat[0]); // xyzw ctor
            if (nuos_seqlock_end_read(&pose_shm_->seq, s1) == 0) break;
        }
    }
    // zero values if shm absent — flat/debug keeps working (BRIDGE_API.md)
    d["pos"] = pos;
    d["quat"] = quat;
    d["t_ns"] = (int64_t)t_ns;
    d["tracking"] = tracking;
    d["kiyo_ok"] = kiyo_ok;
    d["arkit_ok"] = arkit_ok;
    return d;
}

Dictionary NuosBridge::get_hands() {
    Dictionary d;
    PackedVector3Array left, right;
    int present = 0;
    uint64_t t_ns = 0;
    if (hands_shm_) {
        for (int tries = 0; tries < 8; ++tries) {
            uint32_t s1 = nuos_seqlock_begin_read(&hands_shm_->seq);
            if (hands_shm_->magic != NUOS_SHM_MAGIC_HANDS) break;
            present = hands_shm_->present;
            t_ns = hands_shm_->t_mono_ns;
            left.resize(NUOS_HAND_JOINTS);
            right.resize(NUOS_HAND_JOINTS);
            for (int i = 0; i < NUOS_HAND_JOINTS; ++i) {
                left.set(i, Vector3(hands_shm_->left[i][0], hands_shm_->left[i][1],
                                    hands_shm_->left[i][2]));
                right.set(i, Vector3(hands_shm_->right[i][0], hands_shm_->right[i][1],
                                     hands_shm_->right[i][2]));
            }
            if (nuos_seqlock_end_read(&hands_shm_->seq, s1) == 0) break;
        }
    }
    d["present"] = present;
    d["left"] = left;
    d["right"] = right;
    d["t_ns"] = (int64_t)t_ns;
    return d;
}

Dictionary NuosBridge::get_stats() {
    // v1: board stats come via the tracker's 48100 state packets consumed by
    // a helper; until that merge lands in-process, report zeroed stats.
    Dictionary d;
    d["fps"] = 0.0f;
    d["bitrate_kbps"] = 0;
    d["loss_ppm"] = 0;
    d["rtt_ms"] = -1.0f;
    d["battery_pct"] = 255;
    d["temp_c"] = -1.0f;
    return d;
}

bool NuosBridge::push_frame(const Ref<Image>& image) {
    if (image.is_null()) return false;

    Ref<Image> img = image;
    if (img->get_format() != Image::FORMAT_RGBA8)
        img = img->duplicate();
    if (img->get_format() != Image::FORMAT_RGBA8)
        img->convert(Image::FORMAT_RGBA8);

    int w = img->get_width(), h = img->get_height();
    if (w <= 0 || h <= 0 || w > NUOS_RING_MAX_W || h > NUOS_RING_MAX_H) {
        if (!size_warned_) {
            ERR_PRINT("NuosBridge: image size invalid for nuos_frames ring");
            size_warned_ = true;
        }
        return false;
    }

    if (!ring_map_) {
        // ring creator on first push_frame (BRIDGE_API.md ownership table)
        ring_fd_ = open_shm(NUOS_SHM_RING_NAME, NUOS_RING_SHM_SIZE, true,
                            &ring_map_);
        if (ring_fd_ < 0 || !ring_map_) {
            ring_map_ = nullptr;
            if (!ring_warned_) {
                ERR_PRINT("NuosBridge: cannot create /dev/shm/nuos_frames");
                ring_warned_ = true;
            }
            return false;
        }
        auto* hdr = (nuos_ring_hdr_t*)ring_map_;
        if (hdr->magic != NUOS_SHM_RING_MAGIC) {
            memset(hdr, 0, sizeof *hdr);
            hdr->magic = NUOS_SHM_RING_MAGIC;
            hdr->version = 1;
        }
    }
    auto* hdr = (nuos_ring_hdr_t*)ring_map_;
    hdr->width = (uint32_t)w;
    hdr->height = (uint32_t)h;
    hdr->stride = (uint32_t)(w * 4);
    hdr->format = NUOS_RING_FORMAT_BGRA8;

    // RGBA8 -> BGRA8 in place (channel swap), per BRIDGE_API.md
    PackedByteArray px = img->get_data();
    uint8_t* p = px.ptrw();
    for (ssize_t i = 0; i + 3 < (ssize_t)px.size(); i += 4) {
        uint8_t t = p[i];
        p[i] = p[i + 2];
        p[i + 2] = t;
    }

    int slot = (int)(hdr->write_seq % NUOS_RING_SLOTS);
    volatile uint32_t* seq = &hdr->slot_seq[slot];
    nuos_seqlock_begin_write(seq);
    uint8_t* base = (uint8_t*)ring_map_ + NUOS_RING_SLOT_OFF(slot);
    memcpy(base, p, px.size());
    hdr->slot_ns[slot] = mono_ns();
    nuos_seqlock_end_write(seq);
    hdr->write_seq++;
    return true;
}
