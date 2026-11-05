/*
 * NuOS frame ring — the xr-home (Godot) renders composited frames and pushes
 * them to nuos-stream for HEVC encode + streaming. Lives in /dev/shm as
 * NUOS_SHM_RING_NAME ("nuos_frames").
 *
 * Layout: header page, then NUOS_RING_SLOTS fixed-size BGRA slots, each
 * 4096-aligned. Per-slot seqlock in slot_seq[]; write_seq reserves slots.
 *
 * Producer (GDExtension): pick slot = write_seq % NUOS_RING_SLOTS,
 *   nuos_seqlock_begin_write(&slot_seq[slot]); fill slot; set slot_ns;
 *   nuos_seqlock_end_write(&slot_seq[slot]); write_seq++;
 * Consumer (nuos-stream): scans for the newest stable slot each tick.
 */
#ifndef NUOS_RING_H
#define NUOS_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUOS_SHM_RING_NAME "/nuos_frames"
#define NUOS_SHM_RING_MAGIC 0x4E505231u /* 'NPR1' */

#define NUOS_RING_SLOTS   4
#define NUOS_RING_MAX_W   2560
#define NUOS_RING_MAX_H   800   /* side-by-side stereo 2560x720 + margin */
#define NUOS_RING_FORMAT_BGRA8 0

typedef struct {
    uint32_t magic;                 /* NUOS_SHM_RING_MAGIC */
    uint32_t version;               /* 1 */
    volatile uint32_t write_seq;    /* producer counter (never read back) */
    uint32_t width;                 /* producer fills each frame */
    uint32_t height;
    uint32_t stride;                /* bytes per row */
    uint32_t format;                /* NUOS_RING_FORMAT_* */
    uint64_t slot_ns[NUOS_RING_SLOTS];    /* board CLOCK_MONOTONIC */
    volatile uint32_t slot_seq[NUOS_RING_SLOTS];
    uint32_t reserved[8];
} nuos_ring_hdr_t; /* header only; slot data follows */

#define NUOS_RING_SLOT_BYTES ((size_t)NUOS_RING_MAX_W * NUOS_RING_MAX_H * 4)

/* Slot i starts at the first 4096 boundary after the header. */
#define NUOS_RING_SLOT_OFF(i) \
    (((sizeof(nuos_ring_hdr_t) + 4095u) & ~(size_t)4095u) \
     + (size_t)(i) * NUOS_RING_SLOT_BYTES)

/* Total shm size to ftruncate to. */
#define NUOS_RING_SHM_SIZE (NUOS_RING_SLOT_OFF(NUOS_RING_SLOTS))

#ifdef __cplusplus
}
#endif

#endif /* NUOS_RING_H */
