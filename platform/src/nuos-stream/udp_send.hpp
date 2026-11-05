// UDP sender to the phone (video) — target IP from the TCP control peer.
#ifndef NUOS_UDP_SEND_HPP
#define NUOS_UDP_SEND_HPP

#include <cstdint>
#include <string>

#include "nuos_proto.h"

namespace nuos {

class UdpSend {
public:
    ~UdpSend();
    bool open();
    void set_target(const std::string& ip, uint16_t port);
    bool send(const void* data, size_t len);
    uint64_t sent_datagrams() const { return sent_; }

private:
    int fd_ = -1;
    std::string ip_ = "127.0.0.1";
    uint16_t port_ = NUOS_PORT_VIDEO;
    uint64_t sent_ = 0;
};

} // namespace nuos
#endif
