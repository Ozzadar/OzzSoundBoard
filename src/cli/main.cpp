#include <cstdio>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>

#include "core/Paths.hpp"
#include "core/SoundLibrary.hpp"
#include "daemon/Daemon.hpp"
#include "ipc/Socket.hpp"

namespace {

using namespace ozzsb;

constexpr std::string_view kUsage = R"(usage: ozzsoundboard [--config PATH] [--socket PATH] <command> [args]

Commands:
  daemon [--node-name NAME] [--description TEXT] [--no-mic] [--no-monitor]
                        run the audio engine and virtual microphone
  play <name>           play a sound into the virtual mic and the speakers
  preview <name>        play a sound through the speakers only
  toggle <name>         stop the sound if it is playing, otherwise play it
  stop [name]           stop one sound, or everything
  list                  list sounds
  add <name> <path>     add or replace a sound
  remove <name>         remove a sound
  inputs                list microphones the soundboard can capture
  input [name|auto]     show or set the microphone to capture (auto follows the system default)
  denoise [on|off]      show or set noise suppression on your microphone (sounds are never filtered)
  reload                make the daemon re-read the config
  status                show daemon state

Config:  $OZZSOUNDBOARD_CONFIG or $XDG_CONFIG_HOME/ozzsoundboard/sounds.json
Socket:  $OZZSOUNDBOARD_SOCKET or $XDG_RUNTIME_DIR/ozzsoundboard.sock
)";

struct Globals {
    std::filesystem::path config = defaultConfigPath();
    std::filesystem::path socket = defaultSocketPath();
};

std::string joinArgs(const std::vector<std::string_view>& args, std::size_t from) {
    std::string joined;
    for (std::size_t i = from; i < args.size(); ++i) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += args[i];
    }
    return joined;
}

int printResponse(const std::expected<Response, std::string>& response) {
    if (!response) {
        std::println(stderr, "ozzsoundboard: {}", response.error());
        std::println(stderr, "is the daemon running? start it with: systemctl --user start ozzsoundboard");
        return 1;
    }
    if (!response->ok) {
        std::println(stderr, "ozzsoundboard: {}", response->message);
        return 1;
    }
    if (!response->body.empty()) {
        std::print("{}", response->body);
    } else if (!response->message.empty()) {
        std::println("{}", response->message);
    }
    return 0;
}

int runDaemon(const Globals& globals, const std::vector<std::string_view>& args) {
    DaemonOptions options{globals.config, globals.socket, {}};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--no-mic") {
            options.engine.captureMic = false;
        } else if (arg == "--no-monitor") {
            options.engine.monitor = false;
        } else if (arg == "--node-name" && i + 1 < args.size()) {
            options.engine.nodeName = args[++i];
        } else if (arg == "--description" && i + 1 < args.size()) {
            options.engine.nodeDescription = args[++i];
        } else {
            std::print(stderr, "{}", kUsage);
            return 2;
        }
    }
    pw_init(nullptr, nullptr);
    const int code = Daemon(std::move(options)).run();
    pw_deinit();
    return code;
}

// Edits the config file directly so it works without the daemon, then asks a running daemon to reload.
int editLibrary(const Globals& globals, auto&& edit) {
    auto library = SoundLibrary::load(globals.config);
    if (!library) {
        std::println(stderr, "ozzsoundboard: {}", library.error());
        return 1;
    }
    if (auto edited = edit(*library); !edited) {
        std::println(stderr, "ozzsoundboard: {}", edited.error());
        return 1;
    }
    if (auto saved = library->save(globals.config); !saved) {
        std::println(stderr, "ozzsoundboard: {}", saved.error());
        return 1;
    }
    if (auto reloaded = sendRequest(globals.socket, Request{Verb::Reload, {}}); reloaded && !reloaded->ok) {
        std::println(stderr, "ozzsoundboard: saved, but the daemon could not reload: {}", reloaded->message);
        return 1;
    }
    return 0;
}

int listOffline(const Globals& globals) {
    auto library = SoundLibrary::load(globals.config);
    if (!library) {
        std::println(stderr, "ozzsoundboard: {}", library.error());
        return 1;
    }
    for (const auto& [name, path] : library->sounds()) {
        std::println("{}\t{}", name, path.string());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Globals globals;
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (args.empty() && arg == "--config" && i + 1 < argc) {
            globals.config = argv[++i];
        } else if (args.empty() && arg == "--socket" && i + 1 < argc) {
            globals.socket = argv[++i];
        } else if (args.empty() && (arg == "-h" || arg == "--help")) {
            std::print("{}", kUsage);
            return 0;
        } else {
            args.push_back(arg);
        }
    }
    if (args.empty()) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }

    const std::string_view command = args[0];
    const std::string name = joinArgs(args, 1);

    if (command == "daemon") {
        return runDaemon(globals, args);
    }
    if (command == "add") {
        if (args.size() != 3) {
            std::println(stderr, "usage: ozzsoundboard add <name> <path>  (quote names with spaces)");
            return 2;
        }
        std::error_code error;
        const auto path = std::filesystem::absolute(args[2], error);
        if (error || !std::filesystem::is_regular_file(path)) {
            std::println(stderr, "ozzsoundboard: {} is not a file", args[2]);
            return 1;
        }
        return editLibrary(globals, [&](SoundLibrary& library) { return library.set(args[1], path); });
    }
    if (command == "input") {
        if (name.empty()) {
            auto library = SoundLibrary::load(globals.config);
            if (!library) {
                std::println(stderr, "ozzsoundboard: {}", library.error());
                return 1;
            }
            std::println("{}", library->input().empty() ? "auto" : library->input());
            return 0;
        }
        return editLibrary(globals, [&](SoundLibrary& library) -> std::expected<void, std::string> {
            library.setInput(name == "auto" ? std::string() : name);
            return {};
        });
    }
    if (command == "denoise") {
        if (name.empty()) {
            auto library = SoundLibrary::load(globals.config);
            if (!library) {
                std::println(stderr, "ozzsoundboard: {}", library.error());
                return 1;
            }
            std::println("{}", library->noiseSuppression() ? "on" : "off");
            return 0;
        }
        if (name != "on" && name != "off") {
            std::println(stderr, "usage: ozzsoundboard denoise [on|off]");
            return 2;
        }
        return editLibrary(globals, [&](SoundLibrary& library) -> std::expected<void, std::string> {
            library.setNoiseSuppression(name == "on");
            return {};
        });
    }
    if (command == "remove") {
        if (name.empty()) {
            std::println(stderr, "usage: ozzsoundboard remove <name>");
            return 2;
        }
        return editLibrary(globals, [&](SoundLibrary& library) -> std::expected<void, std::string> {
            if (!library.remove(name)) {
                return std::unexpected(std::format("unknown sound \"{}\"", name));
            }
            return {};
        });
    }

    std::string line(command);
    if (!name.empty()) {
        line += ' ';
        line += name;
    }
    auto request = parseRequest(line);
    if (!request) {
        std::println(stderr, "ozzsoundboard: {}", request.error());
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    auto response = sendRequest(globals.socket, *request);
    if (!response && request->verb == Verb::List) {
        return listOffline(globals);
    }
    return printResponse(response);
}
