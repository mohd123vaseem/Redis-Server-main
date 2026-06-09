#include "../include/RedisServer.h"
#include "../include/RedisDatabase.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <pthread.h>

int main(int argc, char* argv[]) {
    int port = 6379; // default
    if (argc >=2) port = std::stoi(argv[1]);

    if (RedisDatabase::getInstance().load("dump.my_rdb"))
        std::cout << "Database Loaded From dump.my_rdb\n";
    else
        std::cout << "No dump found or load failed; starting with an empty database.\n";

    RedisServer server(port);

    // Background persistence: dump the database every 300 seconds (5 minutes).
    //
    // Previously this thread was detach()ed and looped forever on a plain
    // sleep_for — it could only ever be killed by the process exiting. Now it's
    // joinable and parks on a condition_variable, so Ctrl+C can wake it
    // immediately (instead of waiting out the rest of a 300s sleep) and we join
    // it cleanly before returning from main().
    std::mutex persistMtx;
    std::condition_variable persistCv;
    bool persistStop = false;

    std::thread persistanceThread([&](){
        // Block SIGINT here too — only the main thread should field Ctrl+C.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);

        std::unique_lock<std::mutex> lock(persistMtx);
        while (!persistStop) {
            // Wait up to 300s; return early (true) if shutdown was requested.
            if (persistCv.wait_for(lock, std::chrono::seconds(300),
                                   [&]{ return persistStop; }))
                break;
            // Timed out → periodic dump. Drop the lock while we write so a
            // concurrent shutdown can still flip persistStop.
            lock.unlock();
            if (!RedisDatabase::getInstance().dump("dump.my_rdb"))
                std::cerr << "Error Dumping Database\n";
            else
                std::cout << "Database Dumped to dump.my_rdb\n";
            lock.lock();
        }
    });

    server.run();   // blocks until Ctrl+C; performs the final dump + joins clients

    // Tell the persistence thread to stop, wake it, and join it.
    {
        std::lock_guard<std::mutex> lock(persistMtx);
        persistStop = true;
    }
    persistCv.notify_one();
    persistanceThread.join();

    return 0;
}
