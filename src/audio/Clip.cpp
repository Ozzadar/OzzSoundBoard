#include "audio/Clip.hpp"

#include <cmath>
#include <format>
#include <memory>

#include <samplerate.h>
#include <sndfile.h>

namespace ozzsb {

namespace {

struct SndfileCloser {
    void operator()(SNDFILE* file) const { sf_close(file); }
};

std::vector<float> readStereo(SNDFILE* file, int channels) {
    constexpr sf_count_t kChunkFrames = 4096;
    std::vector<float> chunk(static_cast<std::size_t>(kChunkFrames * channels));
    std::vector<float> stereo;
    while (true) {
        const sf_count_t read = sf_readf_float(file, chunk.data(), kChunkFrames);
        if (read <= 0) {
            break;
        }
        for (sf_count_t frame = 0; frame < read; ++frame) {
            const float* in = &chunk[static_cast<std::size_t>(frame * channels)];
            stereo.push_back(in[0]);
            stereo.push_back(channels == 1 ? in[0] : in[1]);
        }
    }
    return stereo;
}

std::expected<std::vector<float>, std::string> resample(const std::vector<float>& stereo, double ratio) {
    const long inputFrames = static_cast<long>(stereo.size() / 2);
    const long outputFrames = static_cast<long>(std::ceil(static_cast<double>(inputFrames) * ratio)) + 64;
    std::vector<float> output(static_cast<std::size_t>(outputFrames) * 2);

    SRC_DATA data{};
    data.data_in = stereo.data();
    data.input_frames = inputFrames;
    data.data_out = output.data();
    data.output_frames = outputFrames;
    data.src_ratio = ratio;
    if (int error = src_simple(&data, SRC_SINC_MEDIUM_QUALITY, 2); error != 0) {
        return std::unexpected(std::format("resampling failed: {}", src_strerror(error)));
    }
    output.resize(static_cast<std::size_t>(data.output_frames_gen) * 2);
    return output;
}

}  // namespace

std::expected<Clip, std::string> decodeClip(const std::filesystem::path& path, std::uint32_t sampleRate) {
    SF_INFO info{};
    std::unique_ptr<SNDFILE, SndfileCloser> file(sf_open(path.c_str(), SFM_READ, &info));
    if (!file) {
        return std::unexpected(std::format("cannot open {}: {}", path.string(), sf_strerror(nullptr)));
    }
    if (info.channels < 1 || info.samplerate < 1) {
        return std::unexpected(std::format("{} has no audio", path.string()));
    }

    Clip clip;
    clip.samples = readStereo(file.get(), info.channels);
    if (clip.samples.empty()) {
        return std::unexpected(std::format("{} has no audio", path.string()));
    }

    if (static_cast<std::uint32_t>(info.samplerate) != sampleRate) {
        auto resampled = resample(clip.samples, static_cast<double>(sampleRate) / info.samplerate);
        if (!resampled) {
            return std::unexpected(std::format("{}: {}", path.string(), resampled.error()));
        }
        clip.samples = std::move(*resampled);
    }
    return clip;
}

}  // namespace ozzsb
