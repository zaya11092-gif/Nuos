// Wire-struct size checks + encode/decode roundtrips for nuos_proto.h.
#include "nuos_proto.h"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

// static_assert wire struct sizes
static_assert(sizeof(nuos_hdr_t)   == 8,  "hdr");
static_assert(sizeof(nuos_pose_t)  == 64, "pose");
static_assert(sizeof(nuos_hands_t) == 516,"hands");
static_assert(sizeof(nuos_video_t) == 48, "video");
static_assert(sizeof(nuos_fec_t)   == 12, "fec");
static_assert(sizeof(nuos_stat_t)  == 28, "stat");
static_assert(sizeof(nuos_hello_t) == 8,  "hello");
static_assert(sizeof(nuos_session_t) == 12, "session");
static_assert(NUOS_VIDEO_PAYLOAD_MAX == 1444, "payload max");

static std::vector<uint8_t> encode(uint8_t type, const void* payload, size_t n, uint16_t seq) {
    nuos_hdr_t h;
    nuos_hdr_init(&h, type);
    h.seq = seq;
    h.len = (uint16_t)n;
    std::vector<uint8_t> buf(sizeof h + n);
    memcpy(buf.data(), &h, sizeof h);
    memcpy(buf.data() + sizeof h, payload, n);
    return buf;
}

int main() {
    // POSE roundtrip
    nuos_pose_t p{};
    p.t_mono_ns = 0x1122334455667788ull;
    p.pos[0] = 1.25f; p.pos[1] = -2.5f; p.pos[2] = 3.75f;
    p.quat[0] = 1.0f; p.quat[3] = 0.5f;
    p.vel[1] = 0.5f; p.ang_vel[2] = 0.01f;
    p.tracking = 2; p.flags = 1;
    auto buf = encode(NUOS_PKT_POSE, &p, sizeof p, 42);
    assert(buf.size() == sizeof(nuos_hdr_t) + 64);
    nuos_hdr_t h; memcpy(&h, buf.data(), sizeof h);
    assert(nuos_hdr_valid(&h) && h.type == NUOS_PKT_POSE && h.seq == 42 && h.len == 64);
    nuos_pose_t q; memcpy(&q, buf.data() + sizeof h, sizeof q);
    assert(!memcmp(&p, &q, sizeof p));

    // HANDS roundtrip (516 bytes)
    nuos_hands_t hs{};
    hs.t_mono_ns = 99; hs.present = 3;
    hs.left[0][0] = 0.1f; hs.right[20][2] = 0.9f;
    auto hbuf = encode(NUOS_PKT_HANDS, &hs, sizeof hs, 7);
    assert(hbuf.size() == 8 + 516);
    nuos_hands_t hs2; memcpy(&hs2, hbuf.data() + 8, sizeof hs2);
    assert(hs2.present == 3 && hs2.left[0][0] == 0.1f && hs2.right[20][2] == 0.9f);

    // VIDEO fragment roundtrip
    nuos_video_t v{};
    v.frame_seq = 0xDEADBEEF; v.frag_idx = 1; v.frag_count = 23;
    v.payload_len = 100; v.flags = 1;
    v.render_quat[0] = 1.0f;
    std::vector<uint8_t> payload(100, 0xAB);
    auto vbuf = encode(NUOS_PKT_VIDEO, &v, sizeof v, 1);
    vbuf.insert(vbuf.end(), payload.begin(), payload.end());
    nuos_video_t v2; memcpy(&v2, vbuf.data() + 8, sizeof v2);
    assert(v2.frame_seq == 0xDEADBEEF && v2.frag_idx == 1 && v2.frag_count == 23);
    assert(v2.payload_len == 100 && vbuf.size() == 8 + 56 + 100);
    assert(vbuf[8 + 56] == 0xAB);

    // STAT / HELLO / SESSION roundtrips
    nuos_stat_t st{}; st.fps_x10 = 600; st.bitrate_kbps = 15000; st.rtt_ms_x10 = -1;
    auto sbuf = encode(NUOS_PKT_STAT, &st, sizeof st, 0);
    nuos_stat_t st2; memcpy(&st2, sbuf.data() + 8, sizeof st2);
    assert(st2.fps_x10 == 600 && st2.bitrate_kbps == 15000 && st2.rtt_ms_x10 == -1);

    nuos_hello_t hl{}; hl.proto_version = NUOS_PROTO_VERSION; hl.device = 1;
    hl.caps = NUOS_CAP_HEVC | NUOS_CAP_FEC;
    auto hlbuf = encode(0, &hl, sizeof hl, 0); // TCP frame: type byte is separate
    nuos_hello_t hl2; memcpy(&hl2, hlbuf.data() + 8, sizeof hl2);
    assert(hl2.proto_version == NUOS_PROTO_VERSION && hl2.device == 1);
    assert(hl2.caps == (NUOS_CAP_HEVC | NUOS_CAP_FEC));

    nuos_session_t se{}; se.fps = 60; se.width = 1280; se.height = 720;
    se.bitrate_kbps = 15000;
    auto sebuf = encode(0, &se, sizeof se, 0);
    nuos_session_t se2; memcpy(&se2, sebuf.data() + 8, sizeof se2);
    assert(se2.fps == 60 && se2.width == 1280 && se2.bitrate_kbps == 15000);

    // invalid header rejected
    nuos_hdr_t bad{}; bad.magic[0]='X';
    assert(!nuos_hdr_valid(&bad));

    printf("proto_roundtrip_test: PASS\n");
    return 0;
}
