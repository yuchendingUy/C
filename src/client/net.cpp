#include "net.h"
#include <atomic>
#include <mutex>
#include <random>
#include <string.h>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#endif

namespace mcc::client {

static sock_t g_fd = BAD_SOCK;
static std::thread g_reader;
static std::atomic<bool> g_up{ false };
static std::mutex g_qlock, g_slock;
static std::vector<Message> g_queue;
static std::atomic<size_t> g_bytes{ 0 };

static bool send_all(const void *p, size_t n) {
    const char *c = (const char *)p;
    while (n) {
        long k = (long)send(g_fd, c, (int)n, 0);
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

static bool recv_all(void *p, size_t n) {
    char *c = (char *)p;
    while (n) {
        long k = (long)recv(g_fd, c, (int)n, 0);
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
        g_bytes += (size_t)k;
    }
    return true;
}

/* 服务端发来的帧不带掩码 */
static void reader() {
    Message msg;
    while (g_up) {
        uint8_t h[2];
        if (!recv_all(h, 2)) break;
        uint64_t len = h[1] & 127;
        if (len == 126) {
            uint8_t e[2];
            if (!recv_all(e, 2)) break;
            len = (uint64_t)e[0] << 8 | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (!recv_all(e, 8)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | e[i];
        }
        if (h[1] & 128) {
            uint8_t m[4];
            if (!recv_all(m, 4)) break;
        }
        msg.resize((size_t)len);
        if (len && !recv_all(msg.data(), (size_t)len)) break;
        uint8_t op = h[0] & 15;
        if (op == 8) break;
        if (op != 2) continue;
        std::lock_guard<std::mutex> lk(g_qlock);
        g_queue.push_back(std::move(msg));
        msg = Message();
    }
    g_up = false;
}

bool net_connect(const char *host, int port) {
#ifdef _WIN32
    static bool wsa;
    if (!wsa) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        wsa = true;
    }
#endif
    net_close();
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) || !res) return false;
    g_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = g_fd != BAD_SOCK && connect(g_fd, res->ai_addr, (int)res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        if (g_fd != BAD_SOCK) sock_close(g_fd);
        g_fd = BAD_SOCK;
        return false;
    }
    int one = 1;
    setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    char req[256];
    int n = snprintf(req, sizeof req,
                     "GET /ws HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: bWNjLWNsaWVudC1rZXkxMg==\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     host, port);
    if (!send_all(req, (size_t)n)) { net_close(); return false; }
    /* 读到响应头结束 */
    std::string head;
    char c;
    while (head.size() < 4096) {
        if (recv(g_fd, &c, 1, 0) != 1) { net_close(); return false; }
        head += c;
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    if (head.find(" 101 ") == std::string::npos) { net_close(); return false; }
    g_up = true;
    g_reader = std::thread(reader);
    return true;
}

void net_close() {
    g_up = false;
    if (g_fd != BAD_SOCK) {
#ifdef _WIN32
        shutdown(g_fd, SD_BOTH);
#else
        shutdown(g_fd, SHUT_RDWR);
#endif
        sock_close(g_fd);
        g_fd = BAD_SOCK;
    }
    if (g_reader.joinable()) g_reader.join();
    std::lock_guard<std::mutex> lk(g_qlock);
    g_queue.clear();
}

bool net_connected() { return g_up; }

/* 客户端发的帧必须带掩码 */
void net_send(const std::string &text) {
    if (!g_up) return;
    static std::mt19937 rng(12345);
    std::lock_guard<std::mutex> lk(g_slock);
    std::vector<uint8_t> f;
    size_t n = text.size();
    f.push_back(0x81);
    if (n < 126) f.push_back((uint8_t)(0x80 | n));
    else { f.push_back(0x80 | 126); f.push_back((uint8_t)(n >> 8)); f.push_back((uint8_t)n); }
    uint32_t m = rng();
    uint8_t mk[4] = { (uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16), (uint8_t)(m >> 24) };
    f.insert(f.end(), mk, mk + 4);
    for (size_t i = 0; i < n; i++) f.push_back((uint8_t)text[i] ^ mk[i & 3]);
    if (!send_all(f.data(), f.size())) g_up = false;
}

void net_poll(std::vector<Message> &out) {
    std::lock_guard<std::mutex> lk(g_qlock);
    for (auto &m : g_queue) out.push_back(std::move(m));
    g_queue.clear();
}

size_t net_bytes_received() { return g_bytes; }

/* Windows 上连本机没监听的端口要等约 2 秒才失败，放到后台线程连，画面不卡 */
static std::thread g_connector;
static std::atomic<bool> g_connecting{ false };

void net_connect_async(const char *host, int port) {
    if (g_connecting) return;
    if (g_connector.joinable()) g_connector.join();
    g_connecting = true;
    std::string h = host;
    g_connector = std::thread([h, port] {
        net_connect(h.c_str(), port);
        g_connecting = false;
    });
}

bool net_connecting() { return g_connecting; }

void net_shutdown() {
    if (g_connector.joinable()) g_connector.join();
    net_close();
}

} // namespace mcc::client
