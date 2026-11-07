#include "core/Paths.hpp"

#include <cstdlib>
#include <format>
#include <unistd.h>

namespace ozzsb {

namespace {

std::filesystem::path envPath(const char* name) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::filesystem::path(value) : std::filesystem::path();
}

}  // namespace

std::filesystem::path defaultConfigPath() {
    if (auto overridden = envPath("OZZSOUNDBOARD_CONFIG"); !overridden.empty()) {
        return overridden;
    }
    auto base = envPath("XDG_CONFIG_HOME");
    if (base.empty()) {
        base = envPath("HOME") / ".config";
    }
    return base / "ozzsoundboard" / "sounds.json";
}

std::filesystem::path defaultSocketPath() {
    if (auto overridden = envPath("OZZSOUNDBOARD_SOCKET"); !overridden.empty()) {
        return overridden;
    }
    if (auto runtime = envPath("XDG_RUNTIME_DIR"); !runtime.empty()) {
        return runtime / "ozzsoundboard.sock";
    }
    return std::filesystem::temp_directory_path() / std::format("ozzsoundboard-{}.sock", getuid());
}

}  // namespace ozzsb
