// NuosBridge — GDExtension per proto/BRIDGE_API.md.
//  - _ready(): maps /dev/shm/nuos_pose + /dev/shm/nuos_hands (read-only, no
//    failure if absent: flat/debug keeps working), starts a 10 Hz poll timer.
//  - push_frame(image): creates /dev/shm/nuos_frames on first call (ring
//    creator per BRIDGE_API.md), converts RGBA8 -> BGRA8, fills newest slot.
#ifndef NUOS_BRIDGE_HPP
#define NUOS_BRIDGE_HPP

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "nuos_shm.h"
#include "nuos_ring.h"

namespace godot {

class NuosBridge : public Node {
    GDCLASS(NuosBridge, Node)

private:
    int pose_fd_ = -1;
    nuos_pose_shm_t* pose_shm_ = nullptr;
    int hands_fd_ = -1;
    nuos_hands_shm_t* hands_shm_ = nullptr;
    int ring_fd_ = -1;
    void* ring_map_ = nullptr;

    bool stats_known_ = false;
    float stat_fps_ = 0;
    int stat_bitrate_kbps_ = 0;
    int stat_loss_ppm_ = 0;
    float stat_rtt_ms_ = -1;
    int stat_battery_ = 255;
    float stat_temp_c_ = -1;

    bool ring_warned_ = false;
    bool size_warned_ = false;

    int open_shm(const char* name, size_t size, bool create, void** map);
    void teardown_shm();

protected:
    static void _bind_methods();

public:
    NuosBridge() = default;
    ~NuosBridge() override;

    void _ready() override;
    void _exit_tree() override;

    Dictionary get_head_pose();
    Dictionary get_hands();
    Dictionary get_stats();
    bool push_frame(const Ref<class Image>& image);

    // 10 Hz poll timer callback: refreshes stats from merged STAT snapshot
    // (tracker stats arrive via the 48100 state packet consumer upstream;
    // here we just keep shm mappings warm).
    void poll_tick();
};

} // namespace godot
#endif
