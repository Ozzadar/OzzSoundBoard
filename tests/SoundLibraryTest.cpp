#include <filesystem>
#include <fstream>
#include <random>

#include <gtest/gtest.h>

#include "core/SoundLibrary.hpp"

using namespace ozzsb;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() / ("ozzsb-test-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(SoundLibrary, MissingFileIsEmptyLibrary) {
    TempDir dir;
    auto library = SoundLibrary::load(dir.path() / "nope.json");
    ASSERT_TRUE(library);
    EXPECT_TRUE(library->sounds().empty());
    EXPECT_EQ(library->gains(), Gains{});
}

TEST(SoundLibrary, SaveLoadRoundTrip) {
    TempDir dir;
    const auto file = dir.path() / "nested" / "sounds.json";
    SoundLibrary library;
    ASSERT_TRUE(library.set("air horn", "/sounds/air horn.wav"));
    ASSERT_TRUE(library.set("boo", "/sounds/boo.ogg"));
    library.gains() = {0.8f, 0.5f, 1.25f};
    library.setInput("alsa_input.usb-mic");
    library.setNoiseSuppression(true);
    ASSERT_TRUE(library.save(file));

    auto loaded = SoundLibrary::load(file);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->sounds(), library.sounds());
    EXPECT_EQ(loaded->gains(), library.gains());
    EXPECT_EQ(loaded->input(), "alsa_input.usb-mic");
    EXPECT_TRUE(loaded->noiseSuppression());
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp"));
}

TEST(SoundLibrary, ParsesPlainNameToPathMap) {
    auto library = SoundLibrary::parse(R"({"sounds": {"boo": "/b.wav"}})");
    ASSERT_TRUE(library);
    ASSERT_NE(library->find("boo"), nullptr);
    EXPECT_EQ(*library->find("boo"), "/b.wav");
    EXPECT_EQ(library->find("nope"), nullptr);
    EXPECT_EQ(library->gains(), Gains{});
    EXPECT_EQ(library->input(), "") << "missing input means automatic";
    EXPECT_FALSE(library->noiseSuppression()) << "noise suppression is opt-in";
}

TEST(SoundLibrary, PartialGainsKeepDefaults) {
    auto library = SoundLibrary::parse(R"({"gains": {"mic": 0.5}})");
    ASSERT_TRUE(library);
    EXPECT_FLOAT_EQ(library->gains().mic, 0.5f);
    EXPECT_FLOAT_EQ(library->gains().clipsToMic, 1.0f);
}

TEST(SoundLibrary, RejectsMalformedConfig) {
    EXPECT_FALSE(SoundLibrary::parse("{"));
    EXPECT_FALSE(SoundLibrary::parse("[]"));
    EXPECT_FALSE(SoundLibrary::parse(R"({"sounds": []})"));
    EXPECT_FALSE(SoundLibrary::parse(R"({"sounds": {"boo": 3}})"));
    EXPECT_FALSE(SoundLibrary::parse(R"({"sounds": {"": "/b.wav"}})"));
    EXPECT_FALSE(SoundLibrary::parse(R"({"sounds": {"boo": ""}})"));
    EXPECT_FALSE(SoundLibrary::parse(R"({"input": 3})"));
    EXPECT_FALSE(SoundLibrary::parse(R"({"noiseSuppression": "yes"})"));
}

TEST(SoundLibrary, ValidatesNames) {
    EXPECT_TRUE(SoundLibrary::validateName("air horn"));
    EXPECT_FALSE(SoundLibrary::validateName(""));
    EXPECT_FALSE(SoundLibrary::validateName("a\nb"));
    EXPECT_FALSE(SoundLibrary::validateName(" padded"));
    EXPECT_FALSE(SoundLibrary::validateName("padded "));
}

TEST(SoundLibrary, SetReplacesAndRemoveDeletes) {
    SoundLibrary library;
    ASSERT_TRUE(library.set("boo", "/a.wav"));
    ASSERT_TRUE(library.set("boo", "/b.wav"));
    EXPECT_EQ(library.sounds().size(), 1u);
    EXPECT_EQ(*library.find("boo"), "/b.wav");
    EXPECT_TRUE(library.remove("boo"));
    EXPECT_FALSE(library.remove("boo"));
}

TEST(SoundLibrary, UnreadableExistingFileIsError) {
    TempDir dir;
    const auto file = dir.path() / "sounds.json";
    std::ofstream(file) << "not json";
    EXPECT_FALSE(SoundLibrary::load(file));
}
