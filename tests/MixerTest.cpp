#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "audio/Mixer.hpp"

using namespace ozzsb;

namespace {

// Stereo clip whose left channel counts up from `left` and right channel is constant `right`.
ClipPtr makeClip(std::size_t frames, float left = 0.5f, float right = 0.25f) {
    auto clip = std::make_shared<Clip>();
    for (std::size_t i = 0; i < frames; ++i) {
        clip->samples.push_back(left);
        clip->samples.push_back(right);
    }
    return clip;
}

class MixerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mixer.setTargetActive(Target::Mic, true);
        mixer.setTargetActive(Target::Monitor, true);
    }

    std::vector<float> render(Target target, std::size_t frames, std::uint32_t channels) {
        std::vector<float> out(frames * channels, 0.0f);
        mixer.render(target, out, channels);
        return out;
    }

    Mixer mixer;
};

}  // namespace

TEST_F(MixerTest, MonoTargetGetsDownmix) {
    mixer.play(makeClip(4), 1.0f, kToMic);
    const auto out = render(Target::Mic, 4, 1);
    for (float sample : out) {
        EXPECT_FLOAT_EQ(sample, 0.375f);
    }
}

TEST_F(MixerTest, StereoTargetKeepsChannels) {
    mixer.play(makeClip(2), 1.0f, kToMonitor);
    const auto out = render(Target::Monitor, 2, 2);
    EXPECT_FLOAT_EQ(out[0], 0.5f);
    EXPECT_FLOAT_EQ(out[1], 0.25f);
    EXPECT_FLOAT_EQ(out[2], 0.5f);
    EXPECT_FLOAT_EQ(out[3], 0.25f);
}

TEST_F(MixerTest, MixesIntoExistingSignal) {
    mixer.play(makeClip(2), 1.0f, kToMic);
    std::vector<float> out{0.1f, 0.1f};
    mixer.render(Target::Mic, out, 1);
    EXPECT_FLOAT_EQ(out[0], 0.475f);
}

TEST_F(MixerTest, PreviewNeverReachesMic) {
    mixer.play(makeClip(4), 1.0f, kToMonitor);
    const auto mic = render(Target::Mic, 4, 1);
    for (float sample : mic) {
        EXPECT_EQ(sample, 0.0f);
    }
    const auto monitor = render(Target::Monitor, 4, 2);
    EXPECT_FLOAT_EQ(monitor[0], 0.5f);
}

TEST_F(MixerTest, TargetsAdvanceIndependently) {
    mixer.play(makeClip(4), 1.0f, kToAll);
    render(Target::Mic, 4, 1);
    EXPECT_EQ(mixer.liveVoices(), 1u) << "monitor has not played it yet";
    const auto monitor = render(Target::Monitor, 4, 2);
    EXPECT_FLOAT_EQ(monitor[6], 0.5f) << "monitor starts from the beginning";
    render(Target::Mic, 1, 1);
    EXPECT_EQ(mixer.liveVoices(), 0u);
}

TEST_F(MixerTest, InactiveTargetDoesNotHoldVoices) {
    mixer.setTargetActive(Target::Monitor, false);
    mixer.play(makeClip(2), 1.0f, kToAll);
    render(Target::Mic, 2, 1);
    render(Target::Mic, 1, 1);
    EXPECT_EQ(mixer.liveVoices(), 0u);
}

TEST_F(MixerTest, FinishedClipIsReleased) {
    auto clip = makeClip(2);
    std::weak_ptr<const Clip> watch = clip;
    mixer.play(std::move(clip), 1.0f, kToMic);
    render(Target::Mic, 8, 1);
    mixer.collectFinished();
    EXPECT_TRUE(watch.expired());
}

TEST_F(MixerTest, ClipEndsMidBufferWithSilenceAfter) {
    mixer.play(makeClip(3), 1.0f, kToMic);
    const auto out = render(Target::Mic, 5, 1);
    EXPECT_FLOAT_EQ(out[2], 0.375f);
    EXPECT_EQ(out[3], 0.0f);
    EXPECT_EQ(out[4], 0.0f);
}

TEST_F(MixerTest, GainsMultiply) {
    mixer.setTargetGain(Target::Mic, 0.5f);
    mixer.play(makeClip(1, 1.0f, 1.0f), 0.5f, kToMic);
    const auto out = render(Target::Mic, 1, 1);
    EXPECT_FLOAT_EQ(out[0], 0.25f);
}

TEST_F(MixerTest, OverlappingVoicesSum) {
    auto clip = makeClip(4, 0.25f, 0.25f);
    mixer.play(clip, 1.0f, kToMic);
    mixer.play(clip, 1.0f, kToMic);
    const auto out = render(Target::Mic, 1, 1);
    EXPECT_FLOAT_EQ(out[0], 0.5f);
}

TEST_F(MixerTest, StopClipOnlyStopsThatClip) {
    auto a = makeClip(100, 0.25f, 0.25f);
    auto b = makeClip(100, 0.5f, 0.5f);
    mixer.play(a, 1.0f, kToMic);
    mixer.play(b, 1.0f, kToMic);
    render(Target::Mic, 1, 1);
    mixer.stop(a.get());
    const auto out = render(Target::Mic, 1, 1);
    EXPECT_FLOAT_EQ(out[0], 0.5f);
    EXPECT_FALSE(mixer.isPlaying(a.get()));
    EXPECT_TRUE(mixer.isPlaying(b.get()));
}

TEST_F(MixerTest, StopAllSilences) {
    mixer.play(makeClip(100), 1.0f, kToAll);
    mixer.play(makeClip(100), 1.0f, kToAll);
    render(Target::Mic, 1, 1);
    mixer.stop(nullptr);
    const auto out = render(Target::Mic, 4, 1);
    for (float sample : out) {
        EXPECT_EQ(sample, 0.0f);
    }
    EXPECT_EQ(mixer.liveVoices(), 0u);
}

TEST_F(MixerTest, StopIsVisibleBeforeAudioThreadRuns) {
    auto clip = makeClip(1000);
    mixer.play(clip, 1.0f, kToMic);
    mixer.stop(clip.get());
    EXPECT_FALSE(mixer.isPlaying(clip.get())) << "a quick second toggle must start the sound again";
    mixer.play(clip, 1.0f, kToMic);
    EXPECT_TRUE(mixer.isPlaying(clip.get()));
}

TEST_F(MixerTest, IsPlayingTracksLifetime) {
    auto clip = makeClip(2);
    mixer.play(clip, 1.0f, kToMic);
    EXPECT_TRUE(mixer.isPlaying(clip.get()));
    render(Target::Mic, 4, 1);
    EXPECT_FALSE(mixer.isPlaying(clip.get()));
}

TEST_F(MixerTest, VoicesBeyondLimitAreDroppedAndReleased) {
    auto clip = makeClip(1000);
    for (std::size_t i = 0; i < Mixer::kMaxVoices + 5; ++i) {
        ASSERT_TRUE(mixer.play(clip, 1.0f, kToMic));
    }
    render(Target::Mic, 1, 1);
    EXPECT_EQ(mixer.liveVoices(), Mixer::kMaxVoices);
}

TEST_F(MixerTest, NullClipIsRejected) {
    EXPECT_FALSE(mixer.play(nullptr));
}
