#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>
#include <vector>

#include <gtest/gtest.h>
#include <sndfile.h>

#include "audio/Clip.hpp"

using namespace ozzsb;

namespace {

std::filesystem::path tempFile(const char* extension) {
    std::random_device random;
    return std::filesystem::temp_directory_path() / ("ozzsb-clip-" + std::to_string(random()) + extension);
}

void writeWav(const std::filesystem::path& path, int rate, int channels, const std::vector<float>& samples) {
    SF_INFO info{};
    info.samplerate = rate;
    info.channels = channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
    ASSERT_NE(file, nullptr) << sf_strerror(nullptr);
    sf_writef_float(file, samples.data(), static_cast<sf_count_t>(samples.size() / static_cast<std::size_t>(channels)));
    sf_close(file);
}

}  // namespace

TEST(Clip, StereoAtTargetRateIsUntouched) {
    const auto path = tempFile(".wav");
    writeWav(path, 48000, 2, {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f});
    auto clip = decodeClip(path);
    std::filesystem::remove(path);
    ASSERT_TRUE(clip) << clip.error();
    ASSERT_EQ(clip->frames(), 3u);
    EXPECT_FLOAT_EQ(clip->samples[0], 0.1f);
    EXPECT_FLOAT_EQ(clip->samples[1], -0.1f);
    EXPECT_FLOAT_EQ(clip->samples[5], -0.3f);
}

TEST(Clip, MonoIsDuplicatedToBothChannels) {
    const auto path = tempFile(".wav");
    writeWav(path, 48000, 1, {0.5f, -0.25f});
    auto clip = decodeClip(path);
    std::filesystem::remove(path);
    ASSERT_TRUE(clip) << clip.error();
    ASSERT_EQ(clip->frames(), 2u);
    EXPECT_FLOAT_EQ(clip->samples[0], 0.5f);
    EXPECT_FLOAT_EQ(clip->samples[1], 0.5f);
    EXPECT_FLOAT_EQ(clip->samples[2], -0.25f);
    EXPECT_FLOAT_EQ(clip->samples[3], -0.25f);
}

TEST(Clip, ExtraChannelsKeepFrontPair) {
    const auto path = tempFile(".wav");
    writeWav(path, 48000, 4, {0.1f, 0.2f, 0.9f, 0.9f});
    auto clip = decodeClip(path);
    std::filesystem::remove(path);
    ASSERT_TRUE(clip) << clip.error();
    ASSERT_EQ(clip->frames(), 1u);
    EXPECT_FLOAT_EQ(clip->samples[0], 0.1f);
    EXPECT_FLOAT_EQ(clip->samples[1], 0.2f);
}

TEST(Clip, ResamplesToTargetRateKeepingDurationAndPitch) {
    constexpr int kRate = 24000;
    constexpr double kHz = 440.0;
    std::vector<float> tone(kRate);  // one second
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * kHz * static_cast<double>(i) / kRate));
    }
    const auto path = tempFile(".wav");
    writeWav(path, kRate, 1, tone);
    auto clip = decodeClip(path);
    std::filesystem::remove(path);
    ASSERT_TRUE(clip) << clip.error();
    EXPECT_NEAR(static_cast<double>(clip->frames()), 48000.0, 64.0);

    // Count rising zero crossings over the middle of the clip to check the pitch survived.
    int crossings = 0;
    for (std::size_t frame = 12000; frame < 36000; ++frame) {
        if (clip->samples[(frame - 1) * 2] < 0.0f && clip->samples[frame * 2] >= 0.0f) {
            ++crossings;
        }
    }
    EXPECT_NEAR(crossings, 220, 2);  // 0.5 s of 440 Hz
}

TEST(Clip, MissingFileIsError) {
    auto clip = decodeClip("/definitely/not/here.wav");
    ASSERT_FALSE(clip);
    EXPECT_NE(clip.error().find("/definitely/not/here.wav"), std::string::npos);
}

TEST(Clip, NonAudioFileIsError) {
    const auto path = tempFile(".wav");
    { std::FILE* file = std::fopen(path.c_str(), "w"); std::fputs("hello", file); std::fclose(file); }
    auto clip = decodeClip(path);
    std::filesystem::remove(path);
    EXPECT_FALSE(clip);
}
