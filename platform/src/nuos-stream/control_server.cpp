#include "control_server.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cstdio>

#include "nuos_proto.h"

namespace nuos {

static bool read_full(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return true;
}

bool ControlServer::start(uint16_t port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof addr) < 0 ||
        listen(listen_fd_, 1) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    return true;
}

void ControlServer::stop() {
    if (conn_ >= 0) close(conn_);
    if (listen_fd_ >= 0) close(listen_fd_);
    conn_ = listen_fd_ = -1;
}

bool ControlServer::poll() {
    if (listen_fd_ < 0) return false;

    if (conn_ < 0) {
        pollfd pf{listen_fd_, POLLIN, 0};
        if (::poll(&pf, 1, 0) <= 0) return false;
        sockaddr_in peer{};
        socklen_t sl = sizeof peer;
        conn_ = accept(listen_fd_, (sockaddr*)&peer, &sl);
        if (conn_ < 0) return false;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        peer_ip_ = ip;
        hello_received_ = false;
        fprintf(stderr, "nuos-stream: control connect from %s\n", ip);
        // board HELLO first
        nuos_hdr_t n; (void)n;
        uint8_t frame[4 + sizeof(nuos_hello_t)];
        frame[0] = NUOS_CTL_HELLO;
        frame[1] = sizeof(nuos_hello_t); frame[2] = 0;
        nuos_hello_t h{};
        h.proto_version = NUOS_PROTO_VERSION;
        h.device = 0;
        h.caps = NUOS_CAP_HEVC | NUOS_CAP_FEC | NUOS_CAP_HANDS | NUOS_CAP_PASSTHROUGH;
        memcpy(frame + 3, &h, sizeof h);
        write_full(conn_, frame, sizeof frame);
        return true;
    }

    // read one TLV frame: [type u8][len u16 LE][payload]
    uint8_t hdr3[3];
    if (!read_full(conn_, hdr3, 3)) {
        fprintf(stderr, "nuos-stream: control peer closed\n");
        if (on_stop_) on_stop_();
        close(conn_);
        conn_ = -1;
        sess_.active = false;
        return false;
    }
    uint8_t type = hdr3[0];
    uint16_t len = (uint16_t)(hdr3[1] | (hdr3[2] << 8));
    uint8_t payload[256];
    if (len > sizeof payload) len = sizeof payload;
    if (len && !read_full(conn_, payload, len)) return false;

    if (!hello_received_ && type != NUOS_CTL_HELLO) {
        // per PROTOCOL.md: HELLO must be first frame
        close(conn_);
        conn_ = -1;
        return true;
    }
    return process_frame(type, payload, len);
}

bool ControlServer::process_frame(uint8_t type, const uint8_t* payload, uint16_t len) {
    switch (type) {
    case NUOS_CTL_HELLO: {
        if (len < sizeof(nuos_hello_t)) return true;
        nuos_hello_t h;
        memcpy(&h, payload, sizeof h);
        if (h.proto_version != NUOS_PROTO_VERSION) {
            fprintf(stderr, "nuos-stream: proto version mismatch (%u), dropping session\n",
                    h.proto_version);
            close(conn_);
            conn_ = -1;
            return true;
        }
        hello_received_ = true;
        sess_.active = true;
        break;
    }
    case NUOS_CTL_SESSION: {
        if (len < sizeof(nuos_session_t)) return true;
        nuos_session_t s;
        memcpy(&s, payload, sizeof s);
        sess_.fps = s.fps ? s.fps : sess_.fps;
        sess_.codec = s.codec;
        sess_.passthrough = s.passthrough;
        sess_.width = s.width ? s.width : sess_.width;
        sess_.height = s.height ? s.height : sess_.height;
        if (s.bitrate_kbps) sess_.bitrate_kbps = s.bitrate_kbps;
        sess_.active = true;
        fprintf(stderr, "nuos-stream: SESSION %ux%u fps=%u kbps=%u\n",
                sess_.width, sess_.height, sess_.fps, sess_.bitrate_kbps);
        if (on_session_) on_session_(sess_);
        break;
    }
    case NUOS_CTL_STOP:
        fprintf(stderr, "nuos-stream: STOP\n");
        if (on_stop_) on_stop_();
        close(conn_);
        conn_ = -1;
        sess_.active = false;
        break;
    case NUOS_CTL_PING: {
        // echo payload back as PONG
        uint8_t out[3 + 256];
        out[0] = NUOS_CTL_PONG;
        out[1] = (uint8_t)(len & 0xFF);
        out[2] = (uint8_t)(len >> 8);
        memcpy(out + 3, payload, len);
        write_full(conn_, out, (size_t)len + 3);
        break;
    }
    case NUOS_CTL_STAT_REQ: {
        nuos_stat_t s{};
        if (stat_cb_) stat_cb_(s);
        uint8_t out[3 + sizeof s];
        out[0] = NUOS_CTL_PONG; // STAT delivered as PONG-encapsulated for v1?
        // Per PROTOCOL.md STAT goes over the telemetry socket; over TCP we
        // reply with the raw nuos_stat_t as a STAT frame for convenience.
        out[0] = NUOS_CTL_STAT_REQ + 16; // reserved local marker; phone uses UDP STAT
        out[1] = sizeof s; out[2] = 0;
        memcpy(out + 3, &s, sizeof s);
        write_full(conn_, out, 3 + sizeof s);
        break;
    }
    default:
        break;
    }
    return true;
}

void ControlServer::send_stat(const nuos_stat_t& s) {
    // STAT goes out over the board's telemetry socket at ~1 Hz (PROTOCOL.md);
    // this hook is kept for future TCP-STAT use.
    (void)s;
}

} // namespace nuos
