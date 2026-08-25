#ifndef REDIS_COMMAND_HANDLER_H
#define REDIS_COMMAND_HANDLER_H

#include <string>
#include <sys/types.h>   // ssize_t

class RedisCommandHandler {
public:
    RedisCommandHandler();
    // Process a command from a client and return a RESP-formatted response.
    std::string processCommand(const std::string& commandLine);
};

// Phase 3 (epoll) framing helper. Given the bytes accumulated so far in a
// client's input buffer, report how many bytes the FIRST complete command at
// the front occupies. Pure function — no sockets — so it is unit-testable on
// its own and reusable by the event loop.
//
// Returns:
//    > 0 : the first complete command occupies exactly this many bytes
//   == 0 : incomplete — not all bytes have arrived yet; wait for more
//    < 0 : malformed framing — caller should reply an error / drop the client
ssize_t respFrameLength(const std::string& buf);

#endif
