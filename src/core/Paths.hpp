#pragma once

#include <filesystem>

namespace ozzsb {

// $XDG_CONFIG_HOME/ozzsoundboard/sounds.json, overridable with OZZSOUNDBOARD_CONFIG.
std::filesystem::path defaultConfigPath();

// $XDG_RUNTIME_DIR/ozzsoundboard.sock, overridable with OZZSOUNDBOARD_SOCKET.
std::filesystem::path defaultSocketPath();

}  // namespace ozzsb
