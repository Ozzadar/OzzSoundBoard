#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

#include "audio/Clip.hpp"
#include "audio/SpscRing.hpp"

namespace ozzsb {

enum class Target : std::uint8_t { Mic = 0, Monitor = 1 };
inline constexpr std::size_t kTargetCount = 2;

using TargetMask = std::uint8_t;
inline constexpr TargetMask kToMic = 1u << static_cast<unsigned>(Target::Mic);
inline constexpr TargetMask kToMonitor = 1u << static_cast<unsigned>(Target::Monitor);
inline constexpr TargetMask kToAll = kToMic | kToMonitor;

// Mixes clip voices into the virtual mic and the speaker monitor.
//
// Control side (play/stop/collectFinished/isPlaying) may be called from any non-audio thread.
// render() and setTargetActive() are called from audio callbacks; render never allocates or frees.
// Clips stay owned by the control side until the audio side reports their voice finished.
class Mixer {
public:
    using VoiceId = std::uint64_t;
    static constexpr std::size_t kMaxVoices = 32;

    Mixer();

    std::expected<VoiceId, std::string> play(ClipPtr clip, float gain = 1.0f, TargetMask targets = kToAll);
    // Stops every voice of `clip`; nullptr stops everything. isPlaying() reflects it immediately.
    void stop(const Clip* clip);
    // Releases clips whose voices have ended.
    void collectFinished();
    bool isPlaying(const Clip* clip);
    std::size_t liveVoices();

    // A target only holds voices back while it is active (its stream is running).
    void setTargetActive(Target target, bool active);
    void setTargetGain(Target target, float gain);

    // Adds the voices routed to `target` into `out` (interleaved, `channels` wide).
    // Mono output gets a stereo downmix; extra channels beyond two are left untouched.
    void render(Target target, std::span<float> out, std::uint32_t channels);

private:
    struct Command {
        enum class Kind : std::uint8_t { Play, Stop, StopAll } kind;
        const Clip* clip;
        VoiceId id;
        float gain;
        TargetMask targets;
    };

    struct Voice {
        const Clip* clip = nullptr;
        VoiceId id = 0;
        float gain = 1.0f;
        TargetMask targets = 0;
        std::array<std::size_t, kTargetCount> position{};
    };

    void drainCommands();
    void retire(std::size_t index);
    bool finished(const Voice& voice) const;

    SpscRing<Command> commands_{256};
    SpscRing<VoiceId> finished_{512};

    // Audio side, guarded by spin_ because the mic and monitor callbacks may run on different threads.
    std::atomic_flag spin_;
    std::array<Voice, kMaxVoices> voices_{};
    std::size_t voiceCount_ = 0;
    std::array<std::atomic<bool>, kTargetCount> targetActive_{};
    std::array<std::atomic<float>, kTargetCount> targetGain_{};

    // Control side. A voice stays live (owning its clip) until the audio side reports it finished;
    // `stopping` hides it from isPlaying() as soon as a stop is requested.
    struct LiveVoice {
        ClipPtr clip;
        bool stopping = false;
    };
    std::mutex control_;
    std::unordered_map<VoiceId, LiveVoice> live_;
    VoiceId nextId_ = 1;
};

}  // namespace ozzsb
