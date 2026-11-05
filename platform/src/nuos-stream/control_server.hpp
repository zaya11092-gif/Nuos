// TCP control server :48012 — HELLO/SESSION/STOP/PING/STAT_REQ TLV frames
// per proto/PROTOCOL.md. The phone's IP is remembered as the video target.
#ifndef NUOS_CONTROL_SERVER_HPP
#define NUOS_CONTROL_SERVER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <atomic>
#include <thread>

#include "nuos_proto.h"

namespace nuos {

struct SessionParams {
    uint8_t fps = 60;
    uint8_t codec = 0;          // 0 = HEVC
    uint8_t passthrough = 0;
    uint16_t width = 1280;
    uint16_t height = 720;
    uint32_t bitrate_kbps = 15000;
    bool active = false;
};

class ControlServer {
public:
    using StatProvider = std::function<void(nuos_stat_t&)>;

    bool start(uint16_t port = NUOS_PORT_CONTROL);
    void stop();

    // Poll for one frame; dispatches to callbacks. Call from stream loop.
    // Returns true if a control frame was processed.
    bool poll();

    // Target IP of the phone (TCP peer), empty until HELLO.
    std::string peer_ip() const { return peer_ip_; }

    // Applied session params (fps/bitrate changed by SESSION frames).
    SessionParams session() const { return sess_; }

    void set_stat_provider(StatProvider p) { stat_cb_ = std::move(p); }
    void set_on_session(std::function<void(const SessionParams&)> f) {
        on_session_ = std::move(f);
    }
    void set_on_stop(std::function<void()> f) { on_stop_ = std::move(f); }
    bool peer_connected() const { return conn_ >= 0; }
    void send_stat(const nuos_stat_t& s);   // responds to STAT_REQ at 1 Hz

private:
    bool process_frame(uint8_t type, const uint8_t* payload, uint16_t len);

    int listen_fd_ = -1;
    int conn_ = -1;
    std::string peer_ip_;
    SessionParams sess_;
    StatProvider stat_cb_;
    std::function<void(const SessionParams&)> on_session_;
    std::function<void()> on_stop_;
    std::atomic<bool> hello_received_{false};
};

} // namespace nuos
#endif
