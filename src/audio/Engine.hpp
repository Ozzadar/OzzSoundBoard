#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

#include "audio/InputSelection.hpp"
#include "audio/Mixer.hpp"
#include "audio/NoiseSuppressor.hpp"
#include "audio/SpscRing.hpp"

struct pw_context;
struct pw_core;
struct pw_metadata;
struct pw_registry;
struct pw_thread_loop;

namespace ozzsb {

struct EngineOptions {
    std::string nodeName = "ozzsoundboard_mic";
    std::string nodeDescription = "OzzSoundBoard Microphone";
    bool captureMic = true;  // mix a real microphone into the virtual mic
    bool monitor = true;     // play clips through the default speakers
};

// PipeWire side: captures a real microphone, publishes the virtual microphone (mic + clips) and a
// monitor playback stream (clips only). The captured microphone is the configured one, or the system
// default; it is never the virtual microphone itself, so that can safely be the system default.
class Engine {
public:
    struct Stats {
        bool capture = false;
        bool virtualMic = false;
        bool monitor = false;
        std::uint64_t micUnderruns = 0;
        std::string input;  // description of the captured microphone, empty when none
        bool noiseSuppression = false;
        float voiceProbability = 0.0f;
    };

    struct Inputs {
        std::vector<InputSource> sources;
        std::string systemDefault;  // node.name, may be our own virtual microphone
        std::string active;         // node.name being captured, empty when none
        std::string preferred;      // configured node.name, empty = automatic
    };

    Engine(Mixer& mixer, EngineOptions options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::expected<void, std::string> start();
    void stop();

    void setMicGain(float gain) { micGain_.store(gain, std::memory_order_relaxed); }
    // RNNoise on the captured microphone only; clips are mixed in afterwards and never denoised.
    void setNoiseSuppression(bool enabled) { denoiser_.setEnabled(enabled); }
    // node.name of the microphone to capture; empty follows the system default.
    void setPreferredInput(std::string name);
    Inputs inputs() const;
    Stats stats() const;
    const EngineOptions& options() const { return options_; }

    struct Slot;
    struct Graph;

private:
    void processCapture();
    void processVirtualMic();
    void processMonitor();
    void stateChanged(Slot& slot, int state, const char* error);

    // PipeWire loop thread only.
    void graphReady();
    void sourceAdded(std::uint32_t id, InputSource source);
    void sourceRemoved(std::uint32_t id);
    void defaultChanged(std::string name);
    void reevaluateInput();
    void retargetCapture(const std::string& target);

    friend struct EngineCallbacks;

    Mixer& mixer_;
    EngineOptions options_;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    Graph* graph_ = nullptr;
    Slot* capture_ = nullptr;
    Slot* virtualMic_ = nullptr;
    Slot* monitor_ = nullptr;

    // Guarded by the PipeWire thread loop lock.
    std::map<std::uint32_t, InputSource> sources_;
    std::string preferred_;
    std::string systemDefault_;
    std::string lastRealDefault_;
    std::string activeInput_;
    bool captureConnected_ = false;
    bool ready_ = false;

    SpscRing<float> micRing_{1 << 16};
    NoiseSuppressor denoiser_;
    std::array<float, 4096> captureScratch_{};
    std::atomic<float> micGain_{1.0f};
    std::atomic<std::uint64_t> micUnderruns_{0};
};

}  // namespace ozzsb
