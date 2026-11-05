/*
 * NuOS board-side shared memory contracts (see proto/BRIDGE_API.md).
 *
 * Two objects in /dev/shm (created via shm_open, mode 0666 so Godot's
 * GDExtension can read them as a non-root service if needed):
 *   NUOS_SHM_POSE_NAME  — latest fused head pose (seqlock, latest value)
 *   NUOS_SHM_HANDS_NAME — latest hand joints (seqlock, latest value)
 *
 * Seqlock: writer increments seq (odd) before writing, increments again
 * (even) after. Readers retry while seq changed or is odd.
 */
#ifndef NUOS_SHM_H
#define NUOS_SHM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUOS_SHM_POSE_NAME  "/nuos_pose"
#define NUOS_SHM_HANDS_NAME "/nuos_hands"
#define NUOS_SHM_RING_NAME  "/nuos_frames"

#define NUOS_SHM_MAGIC_POSE  0x4E505331u /* 'NPS1' */
#define NUOS_SHM_MAGIC_HANDS 0x4E505332u /* 'NPS2' */

#define NUOS_HAND_JOINTS 21 /* keep in sync with proto/nuos_proto.h */

typedef struct {
    uint32_t magic;            /* NUOS_SHM_MAGIC_POSE */
    uint32_t version;          /* 1 */
    volatile uint32_t seq;     /* seqlock */
    uint8_t  tracking;         /* 0 none, 1 limited, 2 normal */
    uint8_t  kiyo_ok;          /* Kiyo odometry alive */
    uint8_t  arkit_ok;         /* ARKit packets arriving */
    uint8_t  reserved;
    uint64_t t_mono_ns;        /* board CLOCK_MONOTONIC at fusion */
    float    pos[3];
    float    quat[4];          /* w x y z */
    float    vel[3];
    float    ang_vel[3];
} nuos_pose_shm_t; /* 64 bytes */

typedef struct {
    uint32_t magic;            /* NUOS_SHM_MAGIC_HANDS */
    uint32_t version;          /* 1 */
    volatile uint32_t seq;
    uint8_t  present;          /* bit0 left, bit1 right (Vision order) */
    uint8_t  reserved[3];
    uint64_t t_mono_ns;
    float    left[NUOS_HAND_JOINTS][3];  /* normalized image coords */
    float    right[NUOS_HAND_JOINTS][3];
} nuos_hands_shm_t;

/* ---------------- seqlock helpers ---------------- */
static inline void nuos_seqlock_begin_write(volatile uint32_t *seq) {
    *seq += 1; /* -> odd */
}
static inline void nuos_seqlock_end_write(volatile uint32_t *seq) {
    *seq += 1; /* -> even */
}
static inline uint32_t nuos_seqlock_begin_read(const volatile uint32_t *seq) {
    uint32_t s1;
    do { s1 = *seq; } while (s1 & 1u);
    return s1;
}
static inline int nuos_seqlock_end_read(const volatile uint32_t *seq,
                                        uint32_t s1) {
    uint32_t s2 = *seq;
    return (s2 == s1) ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* NUOS_SHM_H */
