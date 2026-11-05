// Frames one HEVC access unit into nuos_video_t fragments (<=
// NUOS_VIDEO_PAYLOAD_MAX payload) plus XOR FEC recovery fragments per
// PROTOCOL.md.
#ifndef NUOS_PACKETIZER_HPP
#define NUOS_PACKETIZER_HPP

#include <cstdint>
#include <vector>

namespace nuos {

struct VideoPacket {
    // complete datagram: nuos_hdr_t + nuos_video_t + payload bytes
    std::vector<uint8_t> bytes;
};

struct FecPacket {
    std::vector<uint8_t> bytes;  // nuos_hdr_t + nuos_fec_t + XOR payload
};

class Packetizer {
public:
    // Split one access unit into fragments + FEC groups.
    // render_pos/render_quat (w x y z) go into every fragment header.
    void frame_packets(uint64_t t_mono_ns, uint32_t frame_seq,
                       const uint8_t* au, size_t len, bool idr,
                       const float render_pos[3], const float render_quat[4],
                       std::vector<VideoPacket>& frags,
                       std::vector<FecPacket>& fec);

private:
    static void xor_into(std::vector<uint8_t>& acc, const uint8_t* p, size_t n);
};

} // namespace nuos
#endif
