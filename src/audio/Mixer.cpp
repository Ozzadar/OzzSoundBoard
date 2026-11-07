#include "audio/Mixer.hpp"

#include <algorithm>

namespace ozzsb {

namespace {

class SpinGuard {
public:
    explicit SpinGuard(std::atomic_flag& flag) : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            flag_.wait(true, std::memory_order_relaxed);
        }
    }
    ~SpinGuard() {
        flag_.clear(std::memory_order_release);
        flag_.notify_one();
    }
    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;

private:
    std::atomic_flag& flag_;
};

}  // namespace

Mixer::Mixer() {
    for (auto& gain : targetGain_) {
        gain.store(1.0f, std::memory_order_relaxed);
    }
}

std::expected<Mixer::VoiceId, std::string> Mixer::play(ClipPtr clip, float gain, TargetMask targets) {
    if (!clip) {
        return std::unexpected("no clip");
    }
    std::lock_guard lock(control_);
    const VoiceId id = nextId_++;
    if (!commands_.push(Command{Command::Kind::Play, clip.get(), id, gain, targets})) {
        return std::unexpected("too many queued sounds");
    }
    live_.emplace(id, LiveVoice{std::move(clip)});
    return id;
}

void Mixer::stop(const Clip* clip) {
    std::lock_guard lock(control_);
    commands_.push(Command{clip ? Command::Kind::Stop : Command::Kind::StopAll, clip, 0, 0.0f, 0});
    for (auto& [id, voice] : live_) {
        if (!clip || voice.clip.get() == clip) {
            voice.stopping = true;
        }
    }
}

void Mixer::collectFinished() {
    std::lock_guard lock(control_);
    VoiceId id = 0;
    while (finished_.pop(id)) {
        live_.erase(id);
    }
}

bool Mixer::isPlaying(const Clip* clip) {
    collectFinished();
    std::lock_guard lock(control_);
    return std::ranges::any_of(live_, [clip](const auto& entry) {
        return entry.second.clip.get() == clip && !entry.second.stopping;
    });
}

std::size_t Mixer::liveVoices() {
    collectFinished();
    std::lock_guard lock(control_);
    return live_.size();
}

void Mixer::setTargetActive(Target target, bool active) {
    targetActive_[static_cast<std::size_t>(target)].store(active, std::memory_order_release);
}

void Mixer::setTargetGain(Target target, float gain) {
    targetGain_[static_cast<std::size_t>(target)].store(gain, std::memory_order_relaxed);
}

void Mixer::render(Target target, std::span<float> out, std::uint32_t channels) {
    if (channels == 0) {
        return;
    }
    SpinGuard guard(spin_);
    drainCommands();

    const auto t = static_cast<std::size_t>(target);
    const TargetMask bit = static_cast<TargetMask>(1u << t);
    const float targetGain = targetGain_[t].load(std::memory_order_relaxed);
    const std::size_t outFrames = out.size() / channels;

    for (std::size_t v = 0; v < voiceCount_; ++v) {
        Voice& voice = voices_[v];
        if (!(voice.targets & bit)) {
            continue;
        }
        const std::size_t start = voice.position[t];
        const std::size_t count = std::min(outFrames, voice.clip->frames() - std::min(start, voice.clip->frames()));
        const float gain = voice.gain * targetGain;
        const float* in = voice.clip->samples.data() + start * 2;
        for (std::size_t frame = 0; frame < count; ++frame) {
            const float left = in[frame * 2] * gain;
            const float right = in[frame * 2 + 1] * gain;
            if (channels == 1) {
                out[frame] += 0.5f * (left + right);
            } else {
                out[frame * channels] += left;
                out[frame * channels + 1] += right;
            }
        }
        voice.position[t] = start + count;
    }

    for (std::size_t v = voiceCount_; v-- > 0;) {
        if (finished(voices_[v])) {
            retire(v);
        }
    }
}

void Mixer::drainCommands() {
    Command command{};
    while (commands_.pop(command)) {
        switch (command.kind) {
        case Command::Kind::Play:
            if (voiceCount_ == kMaxVoices) {
                finished_.push(command.id);
                break;
            }
            voices_[voiceCount_++] = Voice{command.clip, command.id, command.gain, command.targets, {}};
            break;
        case Command::Kind::Stop:
            for (std::size_t v = voiceCount_; v-- > 0;) {
                if (voices_[v].clip == command.clip) {
                    retire(v);
                }
            }
            break;
        case Command::Kind::StopAll:
            while (voiceCount_ > 0) {
                retire(voiceCount_ - 1);
            }
            break;
        }
    }
}

void Mixer::retire(std::size_t index) {
    finished_.push(voices_[index].id);
    voices_[index] = voices_[--voiceCount_];
}

bool Mixer::finished(const Voice& voice) const {
    for (std::size_t t = 0; t < kTargetCount; ++t) {
        const bool routed = voice.targets & (1u << t);
        if (routed && targetActive_[t].load(std::memory_order_acquire) && voice.position[t] < voice.clip->frames()) {
            return false;
        }
    }
    return true;
}

}  // namespace ozzsb
