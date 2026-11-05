#include "udp_send.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace nuos {

UdpSend::~UdpSend() { if (fd_ >= 0) close(fd_); }

bool UdpSend::open() {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    return fd_ >= 0;
}

void UdpSend::set_target(const std::string& ip, uint16_t port) {
    ip_ = ip;
    port_ = port;
}

bool UdpSend::send(const void* data, size_t len) {
    if (fd_ < 0 && !open()) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) return false;
    ssize_t n = sendto(fd_, data, len, 0, (sockaddr*)&addr, sizeof addr);
    if (n >= 0) { ++sent_; return true; }
    return false;
}

} // namespace nuos
