#include "../include/RedisServer.h"
#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <pthread.h>

// Set by the SIGINT handler, read by the accept loop. A signal handler may only
// touch async-signal-safe state, so this is ALL the handler does — no I/O, no
// close(), no dump(). The real cleanup runs on the main thread once accept()
// returns. (std::atomic<bool> is lock-free on every platform we target, so it
// is safe to store to from a handler.)
static std::atomic<bool> g_shutdown{false};

void signalHandler(int /*signum*/) {
    g_shutdown = true;
}

void RedisServer::setupSignalHandler() {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART: we WANT accept() to return with EINTR when the signal
    // arrives, so the loop can notice g_shutdown. With SA_RESTART the kernel
    // would silently restart accept() and we'd never see the flag.
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

RedisServer::RedisServer(int port) : port(port), server_socket(-1) {
    setupSignalHandler();
}

void RedisServer::shutdown() {
    // Persist before we go, so a Ctrl+C never loses data.
    if (RedisDatabase::getInstance().dump("dump.my_rdb"))
        std::cout << "Database Dumped to dump.my_rdb\n";
    else
        std::cerr << "Error dumping database\n";

    // Stop accepting new connections.
    if (server_socket != -1) {
        close(server_socket);
        server_socket = -1;
    }

    if (!clients.empty())
        std::cout << "Joining " << clients.size() << " active client connection(s)...\n";

    // Wake any worker parked in a blocking recv(): ::shutdown() makes its
    // pending recv() return 0 right away, so the worker breaks out of its loop
    // and becomes joinable — instead of us waiting out its 300s recv timeout.
    // (Skip ones already finished: their socket is closed; :: prefix avoids
    // calling this member function recursively.)
    for (auto& w : clients) {
        if (!w.done->load())
            ::shutdown(w.socket, SHUT_RDWR);
    }
    for (auto& w : clients) {
        if (w.thread.joinable())
            w.thread.join();
    }
    clients.clear();

    std::cout << "Server Shutdown Complete!\n";
}

void RedisServer::run() {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "Error Creating Server Socket\n";
        return;
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Error Binding Server Socket\n";
        return;
    }

    if (listen(server_socket, 10) < 0) {
        std::cerr << "Error Listening On Server Socket\n";
        return;
    }

    std::cout << "Redis Server Listening On Port " << port << "\n";

    RedisCommandHandler cmdHandler;

    while (!g_shutdown) {
        int client_socket = accept(server_socket, nullptr, nullptr);
        if (client_socket < 0) {
            // SIGINT interrupts accept() with EINTR (handler installed without
            // SA_RESTART). g_shutdown tells us it was a shutdown request.
            if (g_shutdown) break;
            if (errno == EINTR) continue;
            std::cerr << "Error Accepting Client Connection\n";
            break;
        }

        // Idle-client timeout: 300s — prevents slow loris (clients connecting and never sending data)
        struct timeval recv_timeout{};
        recv_timeout.tv_sec = 300;
        recv_timeout.tv_usec = 0;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // Detect silently-dead clients (network failure, crash) — OS probes idle sockets
        int keepalive = 1;
        setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

        // Reap workers that have already finished so `clients` stays bounded —
        // otherwise 10,000 served-and-gone clients leave 10,000 dead entries.
        for (auto it = clients.begin(); it != clients.end(); ) {
            if (it->done->load()) {
                it->thread.join();
                it = clients.erase(it);
            } else {
                ++it;
            }
        }

        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([client_socket, &cmdHandler, done](){
            // Block SIGINT here so a terminal Ctrl+C is always delivered to the
            // main thread (the one in accept()), never to a worker. Otherwise
            // the signal could land on a worker, set g_shutdown, and leave the
            // main thread blocked in accept() with nothing to wake it.
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGINT);
            pthread_sigmask(SIG_BLOCK, &mask, nullptr);

            char buffer[1024];
            while (true) {
                memset(buffer, 0, sizeof(buffer));
                int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
                if (bytes <= 0) break;
                std::string request(buffer, bytes);
                std::string response = cmdHandler.processCommand(request);
                send(client_socket, response.c_str(), response.size(), 0);
            }
            close(client_socket);
            done->store(true);   // set LAST: signals the accept loop it may reap us
        });
        clients.push_back(ClientWorker{std::move(worker), done, client_socket});
    }

    // Only reached after a shutdown request. Safe to clean up now: we're on the
    // main thread, not inside an async signal handler.
    shutdown();
}
