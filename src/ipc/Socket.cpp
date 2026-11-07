#include "ipc/Socket.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ozzsb {

namespace {

constexpr std::size_t kMaxRequestBytes = 4096;
constexpr std::size_t kMaxResponseBytes = 1 << 20;

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }

private:
    int fd_;
};

std::expected<sockaddr_un, std::string> makeAddress(const std::filesystem::path& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string text = path.string();
    if (text.size() >= sizeof(address.sun_path)) {
        return std::unexpected(std::format("socket path too long: {}", text));
    }
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    return address;
}

bool writeAll(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t written = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

// Reads until EOF, `stopAtNewline`, the size cap or the timeout.
std::expected<std::string, std::string> readUntil(int fd, bool stopAtNewline, std::size_t limit,
                                                  std::chrono::milliseconds timeout) {
    std::string data;
    std::array<char, 1024> chunk{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (data.size() < limit) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return std::unexpected("timed out");
        }
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return std::unexpected("timed out");
        }
        const ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::strerror(errno));
        }
        if (got == 0) {
            break;
        }
        data.append(chunk.data(), static_cast<std::size_t>(got));
        if (stopAtNewline && data.find('\n') != std::string::npos) {
            break;
        }
    }
    return data;
}

}  // namespace

std::expected<Response, std::string> sendRequest(const std::filesystem::path& socketPath, const Request& request,
                                                 std::chrono::milliseconds timeout) {
    auto address = makeAddress(socketPath);
    if (!address) {
        return std::unexpected(address.error());
    }
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (fd.get() < 0) {
        return std::unexpected(std::strerror(errno));
    }
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0) {
        return std::unexpected(std::format("daemon not reachable at {}: {}", socketPath.string(), std::strerror(errno)));
    }
    if (!writeAll(fd.get(), formatRequest(request))) {
        return std::unexpected(std::format("send failed: {}", std::strerror(errno)));
    }
    ::shutdown(fd.get(), SHUT_WR);
    auto reply = readUntil(fd.get(), false, kMaxResponseBytes, timeout);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    return parseResponse(*reply);
}

ControlServer::~ControlServer() { stop(); }

std::expected<void, std::string> ControlServer::start(const std::filesystem::path& socketPath, Handler handler,
                                                      std::function<void()> idle,
                                                      std::chrono::milliseconds idleInterval) {
    auto address = makeAddress(socketPath);
    if (!address) {
        return std::unexpected(address.error());
    }
    if (sendRequest(socketPath, Request{Verb::Ping, {}}, std::chrono::milliseconds(500))) {
        return std::unexpected(std::format("another daemon is already listening on {}", socketPath.string()));
    }
    std::error_code ignored;
    std::filesystem::remove(socketPath, ignored);
    std::filesystem::create_directories(socketPath.parent_path(), ignored);

    Fd listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (listener.get() < 0) {
        return std::unexpected(std::strerror(errno));
    }
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0) {
        return std::unexpected(std::format("cannot bind {}: {}", socketPath.string(), std::strerror(errno)));
    }
    if (::listen(listener.get(), 16) != 0) {
        return std::unexpected(std::format("cannot listen on {}: {}", socketPath.string(), std::strerror(errno)));
    }
    Fd wake(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (wake.get() < 0) {
        return std::unexpected(std::strerror(errno));
    }

    path_ = socketPath;
    handler_ = std::move(handler);
    idle_ = std::move(idle);
    idleInterval_ = idleInterval;
    listenFd_ = listener.release();
    wakeFd_ = wake.release();
    thread_ = std::thread([this] { run(); });
    return {};
}

void ControlServer::stop() {
    if (!thread_.joinable()) {
        return;
    }
    const std::uint64_t one = 1;
    [[maybe_unused]] auto written = ::write(wakeFd_, &one, sizeof(one));
    thread_.join();
    ::close(listenFd_);
    ::close(wakeFd_);
    listenFd_ = wakeFd_ = -1;
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
}

void ControlServer::run() {
    while (true) {
        std::array<pollfd, 2> fds{pollfd{listenFd_, POLLIN, 0}, pollfd{wakeFd_, POLLIN, 0}};
        const int ready = ::poll(fds.data(), fds.size(), static_cast<int>(idleInterval_.count()));
        if (fds[1].revents & POLLIN) {
            return;
        }
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            const int client = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client >= 0) {
                serve(client);
            }
        }
        if (idle_) {
            idle_();
        }
    }
}

void ControlServer::serve(int client) {
    Fd fd(client);
    auto line = readUntil(fd.get(), true, kMaxRequestBytes, std::chrono::seconds(2));
    Response response;
    if (!line) {
        response = Response::failure(line.error());
    } else if (auto request = parseRequest(line->substr(0, line->find('\n'))); !request) {
        response = Response::failure(request.error());
    } else {
        response = handler_(*request);
    }
    writeAll(fd.get(), formatResponse(response));
}

}  // namespace ozzsb
