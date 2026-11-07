#include "daemon/Daemon.hpp"

#include <algorithm>
#include <csignal>
#include <format>
#include <print>

#include <pthread.h>

namespace ozzsb {

Daemon::Daemon(DaemonOptions options) : options_(std::move(options)), engine_(mixer_, options_.engine) {}

int Daemon::run() {
    // Block the signals before any thread starts so only sigwait() below receives them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    if (auto loaded = reload(); !loaded) {
        std::println(stderr, "ozzsoundboard: {}", loaded.error());
    }
    if (auto started = engine_.start(); !started) {
        std::println(stderr, "ozzsoundboard: {}", started.error());
        return 1;
    }
    auto served = server_.start(
        options_.socketPath, [this](const Request& request) { return handle(request); },
        [this] { mixer_.collectFinished(); });
    if (!served) {
        std::println(stderr, "ozzsoundboard: {}", served.error());
        engine_.stop();
        return 1;
    }
    std::println("ozzsoundboard: virtual microphone \"{}\" ready, listening on {}", options_.engine.nodeDescription,
                 options_.socketPath.string());

    while (true) {
        int signal = 0;
        sigwait(&signals, &signal);
        if (signal != SIGHUP) {
            break;
        }
        std::lock_guard lock(mutex_);
        if (auto loaded = reload(); !loaded) {
            std::println(stderr, "ozzsoundboard: {}", loaded.error());
        }
    }

    server_.stop();
    engine_.stop();
    return 0;
}

Response Daemon::handle(const Request& request) {
    std::lock_guard lock(mutex_);
    switch (request.verb) {
    case Verb::Play:
        return start(request.argument, kToAll);
    case Verb::Preview:
        return start(request.argument, kToMonitor);
    case Verb::Toggle: {
        auto clip = clipFor(request.argument);
        if (!clip) {
            return Response::failure(clip.error());
        }
        if (mixer_.isPlaying(clip->get())) {
            mixer_.stop(clip->get());
            return Response::success("stopped");
        }
        return start(request.argument, kToAll);
    }
    case Verb::Stop:
        if (request.argument.empty()) {
            mixer_.stop(nullptr);
            return Response::success("stopped all");
        }
        if (!library_.find(request.argument)) {
            return Response::failure(std::format("unknown sound \"{}\"", request.argument));
        }
        if (auto clip = clipFor(request.argument)) {
            mixer_.stop(clip->get());
        }
        return Response::success("stopped");
    case Verb::List: {
        std::string body;
        for (const auto& [name, path] : library_.sounds()) {
            body += std::format("{}\t{}\n", name, path.string());
        }
        return Response::success(std::format("{} sounds", library_.sounds().size()), std::move(body));
    }
    case Verb::Reload:
        if (auto loaded = reload(); !loaded) {
            return Response::failure(loaded.error());
        }
        return Response::success(std::format("{} sounds", library_.sounds().size()));
    case Verb::Status:
        return status();
    case Verb::Inputs:
        return inputs();
    case Verb::Ping:
        return Response::success("pong");
    }
    return Response::failure("unhandled command");
}

Response Daemon::start(std::string_view name, TargetMask targets) {
    auto clip = clipFor(name);
    if (!clip) {
        return Response::failure(clip.error());
    }
    if (auto voice = mixer_.play(std::move(*clip), 1.0f, targets); !voice) {
        return Response::failure(voice.error());
    }
    return Response::success("playing");
}

Response Daemon::status() {
    const Engine::Stats stats = engine_.stats();
    auto state = [](bool on) { return on ? "running" : "idle"; };
    std::string capture = "disabled";
    if (options_.engine.captureMic) {
        capture = stats.input.empty() ? "no microphone" : std::format("{}\t{}", state(stats.capture), stats.input);
    }
    std::string body = std::format(
        "virtual_mic\t{}\t{}\nmic_capture\t{}\nmic_mode\t{}\nmonitor\t{}\nvoices\t{}\nsounds\t{}\n"
        "mic_underruns\t{}\nconfig\t{}\n",
        options_.engine.nodeDescription, state(stats.virtualMic), capture,
        library_.input().empty() ? "automatic" : "fixed",
        options_.engine.monitor ? state(stats.monitor) : "disabled", mixer_.liveVoices(),
        library_.sounds().size(), stats.micUnderruns, options_.configPath.string());
    return Response::success("running", std::move(body));
}

Response Daemon::inputs() {
    const Engine::Inputs inputs = engine_.inputs();
    std::string body;
    for (const InputSource& source : inputs.sources) {
        std::string flags;
        auto flag = [&flags](bool on, std::string_view name) {
            if (on) {
                flags += flags.empty() ? "" : ",";
                flags += name;
            }
        };
        flag(source.name == inputs.systemDefault, "default");
        flag(source.name == inputs.active, "active");
        flag(source.name == inputs.preferred, "preferred");
        body += std::format("{}\t{}\t{}\n", source.name, source.description, flags);
    }
    return Response::success(inputs.systemDefault == options_.engine.nodeName ? "default is the soundboard"
                                                                               : "",
                             std::move(body));
}

std::expected<void, std::string> Daemon::reload() {
    auto library = SoundLibrary::load(options_.configPath);
    if (!library) {
        return std::unexpected(library.error());
    }
    library_ = std::move(*library);
    engine_.setMicGain(library_.gains().mic);
    engine_.setPreferredInput(library_.input());
    mixer_.setTargetGain(Target::Mic, library_.gains().clipsToMic);
    mixer_.setTargetGain(Target::Monitor, library_.gains().clipsToMonitor);

    std::erase_if(cache_, [this](const auto& entry) {
        return !std::ranges::any_of(library_.sounds(),
                                    [&](const auto& sound) { return sound.second.string() == entry.first; });
    });
    // Decode up front so the first button press plays instantly.
    for (const auto& [name, path] : library_.sounds()) {
        if (auto clip = clipFor(name); !clip) {
            std::println(stderr, "ozzsoundboard: {}", clip.error());
        }
    }
    return {};
}

std::expected<ClipPtr, std::string> Daemon::clipFor(std::string_view name) {
    const std::filesystem::path* path = library_.find(name);
    if (!path) {
        return std::unexpected(std::format("unknown sound \"{}\"", name));
    }
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(*path, error);
    if (error) {
        return std::unexpected(std::format("{}: {}", path->string(), error.message()));
    }
    if (auto it = cache_.find(path->string()); it != cache_.end() && it->second.modified == modified) {
        return it->second.clip;
    }
    auto decoded = decodeClip(*path);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto clip = std::make_shared<const Clip>(std::move(*decoded));
    cache_.insert_or_assign(path->string(), CachedClip{modified, clip});
    return clip;
}

}  // namespace ozzsb
