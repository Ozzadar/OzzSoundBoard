#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ozzsb {

inline constexpr std::uint32_t kSampleRate = 48000;

// Decoded audio, interleaved stereo float at kSampleRate.
struct Clip {
    std::vector<float> samples;

    std::size_t frames() const { return samples.size() / 2; }
};

using ClipPtr = std::shared_ptr<const Clip>;

// Decodes anything libsndfile reads (WAV, FLAC, Ogg/Opus, MP3, ...), converts to stereo and resamples.
std::expected<Clip, std::string> decodeClip(const std::filesystem::path& path, std::uint32_t sampleRate = kSampleRate);

}  // namespace ozzsb
