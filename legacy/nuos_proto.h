/*
 * NuOS wire protocol v1 — single source of truth for all packet layouts.
 *
 * Conventions:
 *  - All multi-byte integers are LITTLE-ENDIAN on the wire.
 *  - All structs are byte-packed (1-byte alignment), no padding.
 *  - Every UDP datagram starts with nuos_hdr_t followed by exactly one payload
 *    struct (video datagrams additionally carry `payload_len` raw bytes).
 *  - `t_mono_ns` values are per-device monotonic clocks and are NOT comparable
 *    across devices. Cross-device timing is measured via control-channel PING.
 *  - Coordinate frame: ARKit gravity-aligned world (Y up), meters.
 *    Quaternions are (w, x, y, z).
 *
 * Layout in repo:
 *  proto/nuos_proto.h        this file
 *  proto/PROTOCOL.md         flows, MTU/FEC/jitter rules
 *  proto/BRIDGE_API.md       in-process (shm) contracts used on the board
 *  platform/include/*.h      board-side shm structures
 *  ios/NuosView/.../NuosProto.swift   Swift mirror (manual LE encode/decode)
 */

#ifndef NUOS_PROTO_H
#define NUOS_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUOS_PROTO_VERSION 1

/* ------------------------------------------------------------------ ports */
#define NUOS_PORT_VIDEO       48010 /* udp board -> phone */
#define NUOS_PORT_TELEMETRY   48011 /* udp phone -> board (pose, hands, stat) */
#define NUOS_PORT_CONTROL     48012 /* tcp board listens; phone connects */
#define NUOS_LOCAL_PORT_STATE 48100 /* udp localhost: tracker -> dashboard/xr-home */
#define NUOS_LOCAL_PORT_HANDS 48101 /* udp localhost: tracker -> handbus */

#define NUOS_MDNS_SERVICE_TYPE "_nuos-xr._tcp"

/* ----------------------------------------------------------- wire constants */
#define NUOS_MTU_DEFAULT 1500
#define NUOS_VIDEO_PAYLOAD_MAX \
    (NUOS_MTU_DEFAULT - (int)sizeof(nuos_hdr_t) - (int)sizeof(nuos_video_t))
#define NUOS_FEC_GROUP      8  /* data fragments per XOR recovery fragment */
#define NUOS_HAND_JOINTS    21
#define NUOS_MAX_HANDS      2

#pragma pack(push, 1)

/* Common header, prefix of every UDP datagram. 8 bytes. */
typedef struct {
    uint8_t  magic[3]; /* 'N', 'U', '1' */
    uint8_t  type;     /* nuos_pkt_type */
    uint16_t seq;      /* per-type rolling counter, wraps at 65535 */
    uint16_t len;      /* payload bytes following the header (excludes
                          trailing raw video bytes; see nuos_video_t) */
} nuos_hdr_t;

enum nuos_pkt_type {
    NUOS_PKT_POSE  = 1,
    NUOS_PKT_HANDS = 2,
    NUOS_PKT_VIDEO = 3,
    NUOS_PKT_FEC   = 4,
    NUOS_PKT_STAT  = 5,
};

/* phone -> board, at ARKit camera rate (~60 Hz). */
typedef struct {
    uint64_t t_mono_ns;  /* phone monotonic */
    float    pos[3];     /* meters */
    float    quat[4];    /* w x y z */
    float    vel[3];     /* m/s */
    float    ang_vel[3]; /* rad/s */
    uint8_t  tracking;   /* 0 none, 1 limited, 2 normal */
    uint8_t  flags;      /* bit0: ultrawide passthrough active */
    uint16_t reserved;
} nuos_pose_t; /* 64 bytes */

/* phone -> board, ~30 Hz. Joint positions in normalized camera space
 * (x, y in [0,1], z = relative depth proxy). The board maps screen-space
 * pointers from x/y; metric scale is NOT guaranteed. */
typedef struct {
    uint64_t t_mono_ns;
    uint8_t  present;    /* bit0: left hand, bit1: right hand */
    uint8_t  reserved[3];
    float    left[NUOS_HAND_JOINTS][3];
    float    right[NUOS_HAND_JOINTS][3];
} nuos_hands_t; /* 516 bytes */

/* board -> phone. Header struct followed by payload_len raw HEVC bytes
 * in the SAME datagram. Fragments reassemble in frag_idx order. */
typedef struct {
    uint64_t t_mono_ns;   /* board compose time */
    uint32_t frame_seq;
    uint16_t frag_idx;    /* 0-based */
    uint16_t frag_count;
    uint16_t payload_len; /* raw bytes following this struct */
    uint8_t  flags;       /* bit0: this frame contains an IDR frame */
    uint8_t  reserved;
    float    render_pos[3];  /* pose used at compose time (for timewarp) */
    float    render_quat[4];
} nuos_video_t; /* 48 bytes */

/* board -> phone, sent right after the last fragment of each FEC group.
 * XOR of the payloads of fragments [group_start, group_start+group_count). */
typedef struct {
    uint32_t frame_seq;
    uint16_t group_start;
    uint16_t group_count;
    uint16_t payload_len; /* length of the XOR recovery payload */
    uint16_t reserved;
} nuos_fec_t; /* 12 bytes */

/* bidirectional, ~1 Hz. */
typedef struct {
    uint64_t t_mono_ns;
    uint32_t fps_x10;
    uint32_t bitrate_kbps;
    uint32_t loss_ppm;     /* estimated datagram loss, parts per million */
    int16_t  rtt_ms_x10;   /* -1 if unknown */
    int16_t  temp_c_x10;   /* -1 if unknown */
    uint8_t  battery_pct;  /* 0-100, 255 = unknown */
    uint8_t  flags;        /* bit0: thermal-throttle hint */
    uint8_t  reserved[2];
} nuos_stat_t; /* 28 bytes */

/* ------------------------------------------------- TCP control (TLV frames) */
/* Frame = [type u8][len u16 LE][payload `len` bytes]. No header magic;
 * the TCP connection is established only after UDP discovery, and HELLO
 * must be the first frame from each side. */

enum nuos_ctl_type {
    NUOS_CTL_HELLO    = 1,
    NUOS_CTL_SESSION  = 2,
    NUOS_CTL_STOP     = 3,
    NUOS_CTL_PING     = 4,
    NUOS_CTL_PONG     = 5,
    NUOS_CTL_STAT_REQ = 6,
};

typedef struct {
    uint16_t proto_version; /* NUOS_PROTO_VERSION */
    uint8_t  device;        /* 0 = board, 1 = phone */
    uint8_t  reserved;
    uint32_t caps;          /* nuos_caps */
} nuos_hello_t; /* 8 bytes */

enum nuos_caps {
    NUOS_CAP_HEVC        = 1u << 0,
    NUOS_CAP_FEC         = 1u << 1,
    NUOS_CAP_HANDS       = 1u << 2,
    NUOS_CAP_PASSTHROUGH = 1u << 3,
    NUOS_CAP_ULTRAWIDE   = 1u << 4,
};

/* phone -> board after HELLO (board may also send to reconfigure). */
typedef struct {
    uint8_t  fps;           /* 30, 60 */
    uint8_t  codec;         /* 0 = HEVC */
    uint8_t  passthrough;   /* 0 off, 1 on (phone composites locally) */
    uint8_t  reserved;
    uint16_t width;         /* per-eye */
    uint16_t height;        /* per-eye */
    uint32_t bitrate_kbps;
} nuos_session_t; /* 12 bytes */

#pragma pack(pop)

/* ---- size assertions (checked at build time on every side) ---- */
#if defined(__cplusplus)
#define NUOS_ASSERT_SIZE(t, n) static_assert(sizeof(t) == (n), "wire size")
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define NUOS_ASSERT_SIZE(t, n) _Static_assert(sizeof(t) == (n), "wire size")
#else
#define NUOS_ASSERT_SIZE(t, n)
#endif

NUOS_ASSERT_SIZE(nuos_hdr_t,   8);
NUOS_ASSERT_SIZE(nuos_pose_t,  64);
NUOS_ASSERT_SIZE(nuos_hands_t, 516);
NUOS_ASSERT_SIZE(nuos_video_t, 48);
NUOS_ASSERT_SIZE(nuos_fec_t,   12);
NUOS_ASSERT_SIZE(nuos_stat_t,  28);
NUOS_ASSERT_SIZE(nuos_hello_t, 8);
NUOS_ASSERT_SIZE(nuos_session_t, 12);

/* -------- helpers -------- */
static inline void nuos_hdr_init(nuos_hdr_t *h, uint8_t type) {
    h->magic[0] = 'N'; h->magic[1] = 'U'; h->magic[2] = '1';
    h->type = type;
}

static inline int nuos_hdr_valid(const nuos_hdr_t *h) {
    return h->magic[0] == 'N' && h->magic[1] == 'U' && h->magic[2] == '1'
        && h->type >= NUOS_PKT_POSE && h->type <= NUOS_PKT_STAT;
}

#ifdef __cplusplus
}
#endif

#endif /* NUOS_PROTO_H */
