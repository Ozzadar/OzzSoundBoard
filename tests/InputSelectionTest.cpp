#include <gtest/gtest.h>

#include "audio/InputSelection.hpp"

using namespace ozzsb;

namespace {

InputState state() {
    InputState s;
    s.self = "ozzsoundboard_mic";
    s.sources = {
        {"usb_mic", "USB Mic", 1000},
        {"webcam_mic", "Webcam", 2000},
        {"ozzsoundboard_mic", "OzzSoundBoard Microphone", 0},
    };
    return s;
}

}  // namespace

TEST(InputSelection, PreferredWins) {
    auto s = state();
    s.preferred = "usb_mic";
    s.systemDefault = "webcam_mic";
    EXPECT_EQ(chooseInput(s), "usb_mic");
}

TEST(InputSelection, MissingPreferredFallsBackToDefault) {
    auto s = state();
    s.preferred = "unplugged_mic";
    s.systemDefault = "usb_mic";
    EXPECT_EQ(chooseInput(s), "usb_mic");
}

TEST(InputSelection, AutomaticFollowsSystemDefault) {
    auto s = state();
    s.systemDefault = "usb_mic";
    EXPECT_EQ(chooseInput(s), "usb_mic");
}

TEST(InputSelection, SelfAsDefaultUsesLastRealDefault) {
    auto s = state();
    s.systemDefault = "ozzsoundboard_mic";
    s.lastRealDefault = "usb_mic";
    EXPECT_EQ(chooseInput(s), "usb_mic");
}

TEST(InputSelection, SelfAsDefaultWithoutHistoryUsesHighestPriority) {
    auto s = state();
    s.systemDefault = "ozzsoundboard_mic";
    EXPECT_EQ(chooseInput(s), "webcam_mic");
}

TEST(InputSelection, NoDefaultUsesHighestPriority) {
    EXPECT_EQ(chooseInput(state()), "webcam_mic");
}

TEST(InputSelection, NeverChoosesSelf) {
    InputState s;
    s.self = "ozzsoundboard_mic";
    s.sources = {{"ozzsoundboard_mic", "OzzSoundBoard Microphone", 5000}};
    s.preferred = "ozzsoundboard_mic";
    s.systemDefault = "ozzsoundboard_mic";
    s.lastRealDefault = "ozzsoundboard_mic";
    EXPECT_EQ(chooseInput(s), std::nullopt);
}

TEST(InputSelection, NoSourcesMeansNoInput) {
    InputState s;
    s.preferred = "usb_mic";
    s.systemDefault = "usb_mic";
    EXPECT_EQ(chooseInput(s), std::nullopt);
}

TEST(InputSelection, StaleLastDefaultIsSkipped) {
    auto s = state();
    s.systemDefault = "ozzsoundboard_mic";
    s.lastRealDefault = "gone_mic";
    EXPECT_EQ(chooseInput(s), "webcam_mic");
}

TEST(InputSelection, ParsesDefaultMetadataValue) {
    EXPECT_EQ(parseDefaultNodeName(R"({"name":"alsa_input.pci-0000_12_00.6.analog-stereo"})"),
              "alsa_input.pci-0000_12_00.6.analog-stereo");
    EXPECT_EQ(parseDefaultNodeName(R"({"name":""})"), std::nullopt);
    EXPECT_EQ(parseDefaultNodeName(R"({"other":"x"})"), std::nullopt);
    EXPECT_EQ(parseDefaultNodeName("not json"), std::nullopt);
    EXPECT_EQ(parseDefaultNodeName(""), std::nullopt);
}
