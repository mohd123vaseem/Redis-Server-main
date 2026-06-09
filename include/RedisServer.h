#ifndef REDIS_SERVER_H
#define REDIS_SERVER_H

#include <string>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class RedisServer {
public:
    RedisServer(int port);
    void run();
    void shutdown();

private:
    int port;
    int server_socket;

    // One entry per connected client.
    //  - thread: the worker handling that client
    //  - done:   set by the worker just before it returns, so the accept loop
    //            can reap (join + erase) it and the vector stays bounded
    //  - socket: kept so shutdown() can ::shutdown() a recv()-blocked worker
    //            and force it to return, instead of waiting out its timeout
    struct ClientWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
        int socket;
    };
    std::vector<ClientWorker> clients;

    // Setup signal handling for graceful shutdown (ctrl + c)
    void setupSignalHandler();
};

#endif
