#include "audio/NoiseSuppressor.hpp"

#include <stdexcept>

#include <rnnoise.h>

namespace ozzsb {

namespace {

// RNNoise works on 16-bit sample magnitudes.
constexpr float kScale = 32768.0f;

}  // namespace

NoiseSuppressor::NoiseSuppressor() : state_(rnnoise_create(nullptr)) {
    if (!state_) {
        throw std::runtime_error("cannot create RNNoise state");
    }
    if (static_cast<std::size_t>(rnnoise_get_frame_size()) != kFrameSize) {
        rnnoise_destroy(state_);
        throw std::runtime_error("unexpected RNNoise frame size");
    }
}

NoiseSuppressor::~NoiseSuppressor() { rnnoise_destroy(state_); }

void NoiseSuppressor::process(std::span<float> samples) {
    if (!enabled()) {
        if (running_) {
            running_ = false;
            voiceProbability_.store(0.0f, std::memory_order_relaxed);
        }
        return;
    }
    if (!running_) {
        // Start clean so a re-enable never replays audio from before it was switched off.
        input_.fill(0.0f);
        output_.fill(0.0f);
        position_ = 0;
        running_ = true;
    }

    for (float& sample : samples) {
        input_[position_] = sample * kScale;
        sample = output_[position_] / kScale;
        if (++position_ == kFrameSize) {
            const float probability = rnnoise_process_frame(state_, output_.data(), input_.data());
            voiceProbability_.store(probability, std::memory_order_relaxed);
            position_ = 0;
        }
    }
}

}  // namespace ozzsb
