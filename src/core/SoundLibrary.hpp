#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace ozzsb {

struct Gains {
    float mic = 1.0f;            // live microphone into the virtual mic
    float clipsToMic = 1.0f;     // sound clips into the virtual mic
    float clipsToMonitor = 1.0f; // sound clips into the speakers

    bool operator==(const Gains&) const = default;
};

// Name -> audio file path store, persisted as JSON:
//   { "sounds": { "airhorn": "/path/airhorn.wav" },
//     "gains": { "mic": 1.0, "clipsToMic": 1.0, "clipsToMonitor": 1.0 },
//     "input": "" }   microphone node.name to capture, empty = automatic
class SoundLibrary {
public:
    static std::expected<SoundLibrary, std::string> load(const std::filesystem::path& path);
    static std::expected<SoundLibrary, std::string> parse(std::string_view json);

    std::expected<void, std::string> save(const std::filesystem::path& path) const;
    std::string serialize() const;

    static std::expected<void, std::string> validateName(std::string_view name);

    std::expected<void, std::string> set(std::string_view name, std::filesystem::path path);
    bool remove(std::string_view name);
    const std::filesystem::path* find(std::string_view name) const;

    const std::map<std::string, std::filesystem::path, std::less<>>& sounds() const { return sounds_; }
    const std::string& input() const { return input_; }
    void setInput(std::string input) { input_ = std::move(input); }

    Gains& gains() { return gains_; }
    const Gains& gains() const { return gains_; }

private:
    std::map<std::string, std::filesystem::path, std::less<>> sounds_;
    Gains gains_;
    std::string input_;
};

}  // namespace ozzsb
