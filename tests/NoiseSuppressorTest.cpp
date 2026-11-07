#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "audio/NoiseSuppressor.hpp"

using ozzsb::NoiseSuppressor;

namespace {

constexpr std::size_t kRate = 48000;

std::vector<float> whiteNoise(std::size_t count, float amplitude, unsigned seed = 1234) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> distribution(-amplitude, amplitude);
    std::vector<float> samples(count);
    for (float& sample : samples) {
        sample = distribution(random);
    }
    return samples;
}

double rms(std::span<const float> samples) {
    double sum = 0.0;
    for (float sample : samples) {
        sum += static_cast<double>(sample) * sample;
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

std::vector<float> run(std::vector<float> samples, std::size_t chunk) {
    NoiseSuppressor suppressor;
    suppressor.setEnabled(true);
    for (std::size_t offset = 0; offset < samples.size(); offset += chunk) {
        suppressor.process(std::span(samples).subspan(offset, std::min(chunk, samples.size() - offset)));
    }
    return samples;
}

}  // namespace

TEST(NoiseSuppressor, DisabledPassesAudioThrough) {
    NoiseSuppressor suppressor;
    auto samples = whiteNoise(2000, 0.3f);
    const auto original = samples;
    suppressor.process(samples);
    EXPECT_EQ(samples, original);
    EXPECT_EQ(suppressor.voiceProbability(), 0.0f);
}

TEST(NoiseSuppressor, EnabledDelaysByOneFrame) {
    const auto out = run(whiteNoise(NoiseSuppressor::kFrameSize * 3, 0.3f), 100);
    for (std::size_t i = 0; i < NoiseSuppressor::kFrameSize; ++i) {
        ASSERT_EQ(out[i], 0.0f) << "sample " << i;
    }
}

TEST(NoiseSuppressor, ChunkSizeDoesNotChangeOutput) {
    const auto input = whiteNoise(kRate / 2, 0.1f);
    const auto reference = run(input, NoiseSuppressor::kFrameSize);
    for (std::size_t chunk : {1u, 7u, 256u, 1000u, 24000u}) {
        EXPECT_EQ(run(input, chunk), reference) << "chunk " << chunk;
    }
}

TEST(NoiseSuppressor, StronglyAttenuatesSteadyNoise) {
    const auto input = whiteNoise(kRate * 2, 0.05f);
    const auto out = run(input, 512);
    const std::span tail(out.data() + kRate, kRate);  // after the model has adapted
    EXPECT_LT(rms(tail), 0.1 * rms(std::span(input).subspan(kRate, kRate)));
}

TEST(NoiseSuppressor, OutputStaysFiniteAndInRangeOnLoudInput) {
    std::vector<float> input(kRate);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = std::sin(2.0f * std::numbers::pi_v<float> * 220.0f * static_cast<float>(i) / kRate);
    }
    for (float sample : run(input, 333)) {
        ASSERT_TRUE(std::isfinite(sample));
        ASSERT_LE(std::abs(sample), 1.5f);
    }
}

TEST(NoiseSuppressor, ReenablingStartsClean) {
    NoiseSuppressor suppressor;
    suppressor.setEnabled(true);
    auto first = whiteNoise(NoiseSuppressor::kFrameSize * 4, 0.3f);
    suppressor.process(first);

    suppressor.setEnabled(false);
    auto bypassed = whiteNoise(100, 0.3f, 99);
    const auto original = bypassed;
    suppressor.process(bypassed);
    EXPECT_EQ(bypassed, original);
    EXPECT_EQ(suppressor.voiceProbability(), 0.0f);

    suppressor.setEnabled(true);
    auto again = whiteNoise(NoiseSuppressor::kFrameSize, 0.3f, 7);
    suppressor.process(again);
    for (float sample : again) {
        ASSERT_EQ(sample, 0.0f) << "no audio from before the bypass may leak out";
    }
}
