#ifndef REDIS_SERVER_H
#define REDIS_SERVER_H

#include <string>
#include <unordered_map>
#include <chrono>

#include "RedisCommandHandler.h"

// Single-threaded, non-blocking epoll event loop (Phase 3 — the Reactor pattern).
// One thread owns one epoll instance and multiplexes the listening socket plus
// every client socket. Replaces the old thread-per-client model.
class RedisServer {
public:
    RedisServer(int port);
    void run();
    void shutdown();

private:
    int port;
    int server_socket;   // listening socket fd
    int epoll_fd;        // epoll instance fd
    int signal_fd;       // signalfd delivering SIGINT into the event loop

    // Per-client state — replaces the old ClientWorker{thread, done, socket}.
    // No thread; just the buffers the event loop needs:
    //   inbuf:  bytes read but not yet a complete command (handles partial reads)
    //   outbuf: reply bytes not yet written (handles partial writes / backpressure)
    struct ClientState {
        std::string inbuf;
        std::string outbuf;
        bool writeRegistered = false;   // are we currently watching EPOLLOUT for this fd?
        // Last time we received data from this client — drives the idle timeout
        // (slow-loris protection) that replaces the old blocking SO_RCVTIMEO.
        std::chrono::steady_clock::time_point lastActivity;
    };
    std::unordered_map<int /*fd*/, ClientState> clients;

    RedisCommandHandler cmdHandler;

    // Event handlers.
    void handleAccept();          // listening socket readable -> accept new client(s)
    void handleRead(int fd);      // client readable -> recv, frame, process, queue replies
    void flushOutput(int fd);     // try to drain a client's outbuf; arm EPOLLOUT if needed
    void closeClient(int fd);     // remove from epoll + close + erase state
    void updateEpollOut(int fd, bool wantWrite);
    void sweepIdleClients();      // close clients idle longer than the timeout (slow-loris)

    static bool makeNonBlocking(int fd);
};

#endif
