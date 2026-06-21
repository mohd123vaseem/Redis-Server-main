#include "../include/RedisServer.h"
#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

#include <iostream>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <atomic>
#include <vector>
#include <chrono>

// Set by the SIGINT handler, read by the event loop. A signal handler may only
// touch async-signal-safe state, so this is ALL it does. The real cleanup runs
// on the main thread once epoll_wait() returns with EINTR.
// (A later step may replace this with a signalfd integrated into the loop.)
static std::atomic<bool> g_shutdown{false};

static void signalHandler(int /*signum*/) {
    g_shutdown = true;
}

// Defensive cap: never let a single client's not-yet-framed input grow without
// bound (a misbehaving or malicious client could otherwise exhaust memory).
static constexpr size_t MAX_INBUF = 64 * 1024 * 1024;   // 64 MB

// Slow-loris protection: drop a client that has sent no data for this long.
// Replaces the old blocking SO_RCVTIMEO (same 300s threshold).
static constexpr std::chrono::seconds IDLE_TIMEOUT{300};

void RedisServer::setupSignalHandler() {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART: we WANT epoll_wait() to return with EINTR when SIGINT
    // arrives, so the loop can notice g_shutdown instead of blocking forever.
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

RedisServer::RedisServer(int port)
    : port(port), server_socket(-1), epoll_fd(-1) {
    setupSignalHandler();
}

bool RedisServer::makeNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

void RedisServer::run() {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) { std::cerr << "Error Creating Server Socket\n"; return; }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Error Binding Server Socket\n"; return;
    }
    if (listen(server_socket, SOMAXCONN) < 0) {
        std::cerr << "Error Listening On Server Socket\n"; return;
    }

    // The listening socket must be non-blocking too: in the accept loop we keep
    // accepting until accept() returns EAGAIN (the backlog is drained).
    if (!makeNonBlocking(server_socket)) {
        std::cerr << "Error Setting Server Socket Non-Blocking\n"; return;
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { std::cerr << "Error Creating epoll Instance\n"; return; }

    // Watch the listening socket for "readable" = a new connection is waiting.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_socket;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev);

    std::cout << "Redis Server Listening On Port " << port << " (epoll)\n";

    constexpr int MAX_EVENTS = 64;
    // Finite timeout (not -1): the loop wakes at least once a second to re-check
    // g_shutdown. This makes shutdown prompt AND closes the race where a SIGINT
    // arriving just before epoll_wait() would otherwise block forever.
    constexpr int LOOP_TIMEOUT_MS = 1000;
    epoll_event events[MAX_EVENTS];
    auto lastSweep = std::chrono::steady_clock::now();

    while (!g_shutdown) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, LOOP_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) continue;   // SIGINT etc. -> loop re-checks g_shutdown
            std::cerr << "epoll_wait error: " << std::strerror(errno) << "\n";
            break;
        }
        // n == 0 -> timeout, no events; loop falls through and re-checks g_shutdown.

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t e = events[i].events;

            if (fd == server_socket) {
                handleAccept();
            } else if (e & (EPOLLHUP | EPOLLERR)) {
                closeClient(fd);
            } else {
                if (e & EPOLLIN) handleRead(fd);
                // handleRead may have closed the client; guard the write side.
                if ((e & EPOLLOUT) && clients.count(fd)) flushOutput(fd);
            }
        }

        // Slow-loris protection: ~once a second, drop connections idle past
        // IDLE_TIMEOUT. The 1s epoll_wait timeout guarantees we reach here even
        // when there are no events at all.
        auto now = std::chrono::steady_clock::now();
        if (now - lastSweep >= std::chrono::seconds(1)) {
            sweepIdleClients();
            lastSweep = now;
        }
    }

    shutdown();
}

void RedisServer::handleAccept() {
    // Level-triggered: drain the whole backlog until accept() says "no more".
    while (true) {
        int client_fd = accept(server_socket, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // backlog drained
            if (errno == EINTR) continue;
            std::cerr << "accept error: " << std::strerror(errno) << "\n";
            break;
        }

        if (!makeNonBlocking(client_fd)) { close(client_fd); continue; }

        // Detect silently-dead clients (network failure / crash) via OS probes.
        int keepalive = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

        epoll_event ev{};
        ev.events = EPOLLIN;             // start by watching for incoming data
        ev.data.fd = client_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            close(client_fd);
            continue;
        }
        ClientState st;
        st.lastActivity = std::chrono::steady_clock::now();   // start the idle clock
        clients.emplace(client_fd, std::move(st));
    }
}

void RedisServer::handleRead(int fd) {
    auto it = clients.find(fd);
    if (it == clients.end()) return;
    ClientState& c = it->second;
    c.lastActivity = std::chrono::steady_clock::now();   // activity -> reset idle clock

    // Drain everything the kernel has for us right now into the input buffer.
    char buf[4096];
    while (true) {
        ssize_t bytes = recv(fd, buf, sizeof(buf), 0);
        if (bytes > 0) { c.inbuf.append(buf, bytes); continue; }
        if (bytes == 0) { closeClient(fd); return; }          // client closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // nothing more for now
        if (errno == EINTR) continue;
        closeClient(fd); return;                              // real error
    }

    // Defensive: drop a client that floods us with un-parseable input instead of
    // buffering forever.
    if (c.inbuf.size() > MAX_INBUF) { closeClient(fd); return; }

    // Pull out every complete command (pipelining) and queue its reply.
    while (true) {
        ssize_t flen = respFrameLength(c.inbuf);
        if (flen == 0) break;                 // command not fully arrived yet
        if (flen < 0) { closeClient(fd); return; }  // malformed framing -> drop client

        std::string frame = c.inbuf.substr(0, static_cast<size_t>(flen));
        c.inbuf.erase(0, static_cast<size_t>(flen));
        c.outbuf += cmdHandler.processCommand(frame);
    }

    flushOutput(fd);
}

void RedisServer::flushOutput(int fd) {
    auto it = clients.find(fd);
    if (it == clients.end()) return;
    ClientState& c = it->second;

    while (!c.outbuf.empty()) {
        // MSG_NOSIGNAL: writing to a peer that hung up returns EPIPE instead of
        // killing us with SIGPIPE.
        ssize_t sent = send(fd, c.outbuf.data(), c.outbuf.size(), MSG_NOSIGNAL);
        if (sent > 0) { c.outbuf.erase(0, static_cast<size_t>(sent)); continue; }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // outbox full
        if (sent < 0 && errno == EINTR) continue;
        closeClient(fd); return;              // real write error
    }

    // If bytes remain, ask epoll to tell us when the socket is writable again;
    // otherwise stop watching for writability so we aren't woken needlessly.
    updateEpollOut(fd, !c.outbuf.empty());
}

void RedisServer::updateEpollOut(int fd, bool wantWrite) {
    auto it = clients.find(fd);
    if (it == clients.end()) return;
    if (it->second.writeRegistered == wantWrite) return;  // nothing to change

    epoll_event ev{};
    ev.events = EPOLLIN | (wantWrite ? EPOLLOUT : 0);
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0)
        it->second.writeRegistered = wantWrite;
}

void RedisServer::closeClient(int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    clients.erase(fd);
}

void RedisServer::sweepIdleClients() {
    auto now = std::chrono::steady_clock::now();
    // Collect first, THEN close: closeClient() erases from `clients`, so we must
    // not mutate the map while iterating it.
    std::vector<int> stale;
    for (const auto& kv : clients) {
        if (now - kv.second.lastActivity > IDLE_TIMEOUT)
            stale.push_back(kv.first);
    }
    for (int fd : stale)
        closeClient(fd);
}

void RedisServer::shutdown() {
    // Persist before we go, so Ctrl+C never loses data.
    if (RedisDatabase::getInstance().dump("dump.my_rdb"))
        std::cout << "Database Dumped to dump.my_rdb\n";
    else
        std::cerr << "Error dumping database\n";

    if (!clients.empty())
        std::cout << "Closing " << clients.size() << " active client connection(s)...\n";

    for (auto& kv : clients)
        close(kv.first);
    clients.clear();

    if (server_socket != -1) { close(server_socket); server_socket = -1; }
    if (epoll_fd != -1) { close(epoll_fd); epoll_fd = -1; }

    std::cout << "Server Shutdown Complete!\n";
}
