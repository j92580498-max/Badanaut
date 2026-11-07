#include "control.h"
#include <cstdint>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
#endif

namespace {

void serve(int port, std::function<std::string(const std::string&)> exec) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) {
        printf("[ctl] socket() failed\n");
        return;
    }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
    if (bind(ls, (sockaddr*)&addr, sizeof addr) != 0) {
        printf("[ctl] bind 127.0.0.1:%d failed\n", port);
        CLOSESOCK(ls);
        return;
    }
    listen(ls, 1);
    printf("[ctl] control server on 127.0.0.1:%d\n", port);

    for (;;) {
        sock_t cs = accept(ls, nullptr, nullptr);
        if (cs == INVALID_SOCKET) {
            continue;
        }
        std::string buf;
        char tmp[2048];
        for (;;) {
            int n = (int)recv(cs, tmp, sizeof tmp, 0);
            if (n <= 0) {
                break;
            }
            buf.append(tmp, (size_t)n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::string reply = exec(line);
                reply += "\n";
                send(cs, reply.data(), (int)reply.size(), 0);
            }
        }
        CLOSESOCK(cs);
    }
}

} // namespace

void start_control_server(int port, std::function<std::string(const std::string&)> exec) {
    std::thread(serve, port, std::move(exec)).detach();
}
