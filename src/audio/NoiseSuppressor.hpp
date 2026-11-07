#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

struct DenoiseState;

namespace ozzsb {

// Streaming RNNoise voice denoiser for 48 kHz mono audio in [-1, 1].
// process() accepts any chunk size. While enabled the output lags the input by kFrameSize samples;
// while disabled it passes audio through untouched with no delay. Realtime safe after construction.
class NoiseSuppressor {
public:
    static constexpr std::size_t kFrameSize = 480;  // 10 ms at 48 kHz

    NoiseSuppressor();
    ~NoiseSuppressor();
    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Probability (0-1) that the most recent frame contained speech; 0 while disabled.
    float voiceProbability() const { return voiceProbability_.load(std::memory_order_relaxed); }

    // Denoises `samples` in place.
    void process(std::span<float> samples);

private:
    DenoiseState* state_;
    std::array<float, kFrameSize> input_{};
    std::array<float, kFrameSize> output_{};
    std::size_t position_ = 0;
    bool running_ = false;
    std::atomic<bool> enabled_{false};
    std::atomic<float> voiceProbability_{0.0f};
};

}  // namespace ozzsb
