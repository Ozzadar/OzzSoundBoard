#include "audio/InputSelection.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace ozzsb {

std::optional<std::string> chooseInput(const InputState& state) {
    auto usable = [&](const std::string& name) {
        return !name.empty() && name != state.self &&
               std::ranges::any_of(state.sources, [&](const InputSource& source) { return source.name == name; });
    };

    for (const std::string* candidate : {&state.preferred, &state.systemDefault, &state.lastRealDefault}) {
        if (usable(*candidate)) {
            return *candidate;
        }
    }

    const InputSource* best = nullptr;
    for (const InputSource& source : state.sources) {
        if (source.name != state.self && (!best || source.priority > best->priority)) {
            best = &source;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return best->name;
}

std::optional<std::string> parseDefaultNodeName(std::string_view json) {
    const auto root = nlohmann::json::parse(json, nullptr, false);
    if (!root.is_object()) {
        return std::nullopt;
    }
    const auto name = root.find("name");
    if (name == root.end() || !name->is_string() || name->get<std::string>().empty()) {
        return std::nullopt;
    }
    return name->get<std::string>();
}

}  // namespace ozzsb
