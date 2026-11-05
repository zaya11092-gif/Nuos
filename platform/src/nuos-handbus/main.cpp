// nuos-handbus — listens on 127.0.0.1:48101 for NUOS_PKT_HANDS datagrams
// (forwarded verbatim by nuos-tracker) and writes /dev/shm/nuos_hands.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <cstdio>

#include "nuos_proto.h"
#include "hands_shm.hpp"

using namespace nuos;

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main() {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    HandsShm shm;
    if (!shm.open()) {
        fprintf(stderr, "nuos-handbus: cannot create %s\n", NUOS_SHM_HANDS_NAME);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(NUOS_LOCAL_PORT_HANDS);
    if (bind(fd, (sockaddr*)&addr, sizeof addr) < 0) {
        perror("bind 48101");
        return 1;
    }

    fprintf(stderr, "nuos-handbus: listening 127.0.0.1:%d -> %s\n",
            NUOS_LOCAL_PORT_HANDS, NUOS_SHM_HANDS_NAME);

    uint8_t buf[2048];
    while (!g_stop) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (n <= (ssize_t)sizeof(nuos_hdr_t)) continue;
        nuos_hdr_t hdr;
        memcpy(&hdr, buf, sizeof hdr);
        if (!nuos_hdr_valid(&hdr) || hdr.type != NUOS_PKT_HANDS) continue;
        if (n - sizeof hdr < (ssize_t)sizeof(nuos_hands_t)) continue;
        nuos_hands_t hs;
        memcpy(&hs, buf + sizeof hdr, sizeof hs);
        shm.publish(hs, mono_ns());
    }

    close(fd);
    fprintf(stderr, "nuos-handbus: stopped\n");
    return 0;
}
