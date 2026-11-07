#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ozzsb {

// A microphone the daemon can capture from (a PipeWire Audio/Source node other than our own).
struct InputSource {
    std::string name;         // node.name, stable across restarts
    std::string description;  // node.description, for humans
    std::int32_t priority = 0;

    bool operator==(const InputSource&) const = default;
};

struct InputState {
    std::string preferred;       // configured node.name, empty = automatic
    std::string systemDefault;   // current default.audio.source, may be our own node
    std::string lastRealDefault; // most recent system default that was not our own node
    std::string self;            // our virtual microphone's node.name
    std::vector<InputSource> sources;
};

// Picks the node.name to capture from, or nullopt when no usable microphone exists.
// Order: the preferred input if present, else the system default unless it is us, else the last real
// default, else the highest-priority source. Never returns `self`.
std::optional<std::string> chooseInput(const InputState& state);

// Extracts "name" from a default-node metadata value such as {"name":"alsa_input.foo"}.
std::optional<std::string> parseDefaultNodeName(std::string_view json);

}  // namespace ozzsb
