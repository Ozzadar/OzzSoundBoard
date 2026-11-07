#pragma once

#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "audio/Engine.hpp"
#include "audio/Mixer.hpp"
#include "core/SoundLibrary.hpp"
#include "ipc/Socket.hpp"

namespace ozzsb {

struct DaemonOptions {
    std::filesystem::path configPath;
    std::filesystem::path socketPath;
    EngineOptions engine;
};

class Daemon {
public:
    explicit Daemon(DaemonOptions options);

    // Runs until SIGINT or SIGTERM. SIGHUP reloads the config. Returns the process exit code.
    int run();

private:
    Response handle(const Request& request);
    std::expected<void, std::string> reload();
    std::expected<ClipPtr, std::string> clipFor(std::string_view name);
    Response start(std::string_view name, TargetMask targets);
    Response status();
    Response inputs();

    struct CachedClip {
        std::filesystem::file_time_type modified;
        ClipPtr clip;
    };

    DaemonOptions options_;
    Mixer mixer_;
    Engine engine_;
    ControlServer server_;

    std::mutex mutex_;
    SoundLibrary library_;
    std::unordered_map<std::string, CachedClip> cache_;  // by path
};

}  // namespace ozzsb
