#include "packetizer.hpp"

#include <cstring>
#include "nuos_proto.h"

namespace nuos {

void Packetizer::xor_into(std::vector<uint8_t>& acc, const uint8_t* p, size_t n) {
    if (acc.size() < n) acc.resize(n, 0);
    for (size_t i = 0; i < n; ++i) acc[i] ^= p[i];
}

void Packetizer::frame_packets(uint64_t t_mono_ns, uint32_t frame_seq,
                               const uint8_t* au, size_t len, bool idr,
                               const float render_pos[3], const float render_quat[4],
                               std::vector<VideoPacket>& frags,
                               std::vector<FecPacket>& fec) {
    frags.clear();
    fec.clear();
    const size_t payload_max = (size_t)NUOS_VIDEO_PAYLOAD_MAX;
    size_t n_frags = (len + payload_max - 1) / payload_max;
    if (len == 0) n_frags = 1;

    for (size_t f = 0; f < n_frags; ++f) {
        const uint8_t* p = au + f * payload_max;
        size_t plen = (f == n_frags - 1) ? (len - f * payload_max) : payload_max;

        VideoPacket pkt;
        pkt.bytes.resize(sizeof(nuos_hdr_t) + sizeof(nuos_video_t) + plen);
        nuos_hdr_t hdr;
        nuos_hdr_init(&hdr, NUOS_PKT_VIDEO);
        hdr.seq = (uint16_t)(frame_seq & 0xFFFF);
        hdr.len = sizeof(nuos_video_t); // header-only struct; raw bytes follow
        memcpy(pkt.bytes.data(), &hdr, sizeof hdr);

        nuos_video_t v{};
        v.t_mono_ns = t_mono_ns;
        v.frame_seq = frame_seq;
        v.frag_idx = (uint16_t)f;
        v.frag_count = (uint16_t)n_frags;
        v.payload_len = (uint16_t)plen;
        v.flags = idr ? 1 : 0;
        memcpy(v.render_pos, render_pos, sizeof v.render_pos);
        memcpy(v.render_quat, render_quat, sizeof v.render_quat);
        memcpy(pkt.bytes.data() + sizeof hdr, &v, sizeof v);
        memcpy(pkt.bytes.data() + sizeof hdr + sizeof v, p, plen);
        frags.push_back(std::move(pkt));

        // FEC per PROTOCOL.md: XOR payload of each NUOS_FEC_GROUP fragments
        if ((f + 1) % NUOS_FEC_GROUP == 0 || f + 1 == n_frags) {
            size_t gstart = f + 1 - ((f + 1) % NUOS_FEC_GROUP == 0
                                     ? NUOS_FEC_GROUP : (f + 1) % NUOS_FEC_GROUP);
            size_t gcount = f + 1 - gstart;
            std::vector<uint8_t> x;
            for (size_t g = gstart; g <= f; ++g) {
                const uint8_t* gp = au + g * payload_max;
                size_t gplen = (g == n_frags - 1)
                    ? (len - g * payload_max) : payload_max;
                xor_into(x, gp, gplen);
            }
            FecPacket fp;
            fp.bytes.resize(sizeof(nuos_hdr_t) + sizeof(nuos_fec_t) + x.size());
            nuos_hdr_t fh;
            nuos_hdr_init(&fh, NUOS_PKT_FEC);
            fh.seq = (uint16_t)(frame_seq & 0xFFFF);
            fh.len = sizeof(nuos_fec_t);
            memcpy(fp.bytes.data(), &fh, sizeof fh);
            nuos_fec_t fx{};
            fx.frame_seq = frame_seq;
            fx.group_start = (uint16_t)gstart;
            fx.group_count = (uint16_t)gcount;
            fx.payload_len = (uint16_t)x.size();
            memcpy(fp.bytes.data() + sizeof fh, &fx, sizeof fx);
            memcpy(fp.bytes.data() + sizeof fh + sizeof fx, x.data(), x.size());
            fec.push_back(std::move(fp));
        }
    }
}

} // namespace nuos
