#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "ipc/Protocol.hpp"

namespace ozzsb {

// Sends one request to the daemon and waits for its reply.
std::expected<Response, std::string> sendRequest(const std::filesystem::path& socketPath, const Request& request,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Unix-socket server that answers one request per connection on its own thread.
class ControlServer {
public:
    using Handler = std::function<Response(const Request&)>;

    ControlServer() = default;
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Fails if another live server already owns the socket. A stale socket file is replaced.
    // `idle` runs on the server thread between connections, at least every `idleInterval`.
    std::expected<void, std::string> start(const std::filesystem::path& socketPath, Handler handler,
                                           std::function<void()> idle = {},
                                           std::chrono::milliseconds idleInterval = std::chrono::milliseconds(250));
    void stop();

private:
    void run();
    void serve(int client);

    std::filesystem::path path_;
    Handler handler_;
    std::function<void()> idle_;
    std::chrono::milliseconds idleInterval_{250};
    int listenFd_ = -1;
    int wakeFd_ = -1;
    std::thread thread_;
};

}  // namespace ozzsb
