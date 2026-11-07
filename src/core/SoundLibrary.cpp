#include "core/SoundLibrary.hpp"

#include <format>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace ozzsb {

namespace {

float readGain(const nlohmann::json& gains, const char* key, float fallback) {
    if (auto it = gains.find(key); it != gains.end() && it->is_number()) {
        return it->get<float>();
    }
    return fallback;
}

}  // namespace

std::expected<SoundLibrary, std::string> SoundLibrary::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        if (!std::filesystem::exists(path)) {
            return SoundLibrary{};
        }
        return std::unexpected(std::format("cannot read {}", path.string()));
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    auto library = parse(buffer.str());
    if (!library) {
        return std::unexpected(std::format("{}: {}", path.string(), library.error()));
    }
    return library;
}

std::expected<SoundLibrary, std::string> SoundLibrary::parse(std::string_view json) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(std::format("invalid JSON: {}", error.what()));
    }
    if (!root.is_object()) {
        return std::unexpected("top level must be an object");
    }

    SoundLibrary library;
    if (auto sounds = root.find("sounds"); sounds != root.end()) {
        if (!sounds->is_object()) {
            return std::unexpected("\"sounds\" must be an object of name -> path");
        }
        for (const auto& [name, path] : sounds->items()) {
            if (!path.is_string()) {
                return std::unexpected(std::format("path for \"{}\" must be a string", name));
            }
            if (auto added = library.set(name, path.get<std::string>()); !added) {
                return std::unexpected(added.error());
            }
        }
    }
    if (auto input = root.find("input"); input != root.end()) {
        if (!input->is_string()) {
            return std::unexpected("\"input\" must be a string");
        }
        library.input_ = input->get<std::string>();
    }
    if (auto denoise = root.find("noiseSuppression"); denoise != root.end()) {
        if (!denoise->is_boolean()) {
            return std::unexpected("\"noiseSuppression\" must be true or false");
        }
        library.noiseSuppression_ = denoise->get<bool>();
    }
    if (auto gains = root.find("gains"); gains != root.end() && gains->is_object()) {
        library.gains_.mic = readGain(*gains, "mic", 1.0f);
        library.gains_.clipsToMic = readGain(*gains, "clipsToMic", 1.0f);
        library.gains_.clipsToMonitor = readGain(*gains, "clipsToMonitor", 1.0f);
    }
    return library;
}

std::string SoundLibrary::serialize() const {
    nlohmann::json root;
    root["sounds"] = nlohmann::json::object();
    for (const auto& [name, path] : sounds_) {
        root["sounds"][name] = path.string();
    }
    root["gains"] = {
        {"mic", gains_.mic},
        {"clipsToMic", gains_.clipsToMic},
        {"clipsToMonitor", gains_.clipsToMonitor},
    };
    root["input"] = input_;
    root["noiseSuppression"] = noiseSuppression_;
    return root.dump(2) + "\n";
}

std::expected<void, std::string> SoundLibrary::save(const std::filesystem::path& path) const {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return std::unexpected(std::format("cannot create {}: {}", path.parent_path().string(), error.message()));
    }

    auto temp = path;
    temp += std::format(".tmp.{}", getpid());
    {
        std::ofstream file(temp, std::ios::trunc);
        if (!file) {
            return std::unexpected(std::format("cannot write {}", temp.string()));
        }
        file << serialize();
        if (!file.flush()) {
            return std::unexpected(std::format("cannot write {}", temp.string()));
        }
    }
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::filesystem::remove(temp);
        return std::unexpected(std::format("cannot replace {}: {}", path.string(), error.message()));
    }
    return {};
}

std::expected<void, std::string> SoundLibrary::validateName(std::string_view name) {
    if (name.empty()) {
        return std::unexpected("sound name must not be empty");
    }
    if (name.find_first_of("\r\n") != std::string_view::npos) {
        return std::unexpected("sound name must not contain line breaks");
    }
    if (name.front() == ' ' || name.back() == ' ') {
        return std::unexpected("sound name must not start or end with a space");
    }
    return {};
}

std::expected<void, std::string> SoundLibrary::set(std::string_view name, std::filesystem::path path) {
    if (auto valid = validateName(name); !valid) {
        return valid;
    }
    if (path.empty()) {
        return std::unexpected(std::format("path for \"{}\" must not be empty", name));
    }
    sounds_.insert_or_assign(std::string(name), std::move(path));
    return {};
}

bool SoundLibrary::remove(std::string_view name) {
    auto it = sounds_.find(name);
    if (it == sounds_.end()) {
        return false;
    }
    sounds_.erase(it);
    return true;
}

const std::filesystem::path* SoundLibrary::find(std::string_view name) const {
    auto it = sounds_.find(name);
    return it == sounds_.end() ? nullptr : &it->second;
}

}  // namespace ozzsb
