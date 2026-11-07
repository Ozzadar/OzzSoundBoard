#include "audio/Engine.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

namespace ozzsb {

struct Engine::Slot {
    enum class Kind { Capture, VirtualMic, Monitor } kind;
    Engine* engine = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    std::atomic<bool> streaming{false};
};

// Registry, default-node metadata and startup sync state. Loop thread only.
struct Engine::Graph {
    Engine* engine = nullptr;
    pw_registry* registry = nullptr;
    spa_hook coreListener{};
    spa_hook registryListener{};
    pw_metadata* metadata = nullptr;
    std::uint32_t metadataId = SPA_ID_INVALID;
    spa_hook metadataListener{};
    int syncSeq = 0;
    int syncRound = 0;
};

struct EngineCallbacks {
    static void stateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error) {
        auto* slot = static_cast<Engine::Slot*>(data);
        slot->engine->stateChanged(*slot, state, error);
    }
    static void captureProcess(void* data) { static_cast<Engine::Slot*>(data)->engine->processCapture(); }
    static void virtualMicProcess(void* data) { static_cast<Engine::Slot*>(data)->engine->processVirtualMic(); }
    static void monitorProcess(void* data) { static_cast<Engine::Slot*>(data)->engine->processMonitor(); }

    static void coreDone(void* data, std::uint32_t id, int seq);
    static void coreError(void* data, std::uint32_t id, int seq, int res, const char* message);
    static void registryGlobal(void* data, std::uint32_t id, std::uint32_t permissions, const char* type,
                               std::uint32_t version, const spa_dict* props);
    static void registryGlobalRemove(void* data, std::uint32_t id);
    static int metadataProperty(void* data, std::uint32_t subject, const char* key, const char* type,
                                const char* value);
};

namespace {

constexpr const char* kLatency = "256/48000";
// Keep at most this many quanta of microphone audio queued before dropping the oldest.
constexpr std::size_t kMaxQueuedQuanta = 3;
// Startup syncs: the first returns after every global is announced, the second after the default
// metadata bound during the first has delivered its properties.
constexpr int kStartupSyncRounds = 2;

pw_stream_events makeStreamEvents(void (*process)(void*)) {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = &EngineCallbacks::stateChanged;
    events.process = process;
    return events;
}

const pw_stream_events kCaptureEvents = makeStreamEvents(&EngineCallbacks::captureProcess);
const pw_stream_events kVirtualMicEvents = makeStreamEvents(&EngineCallbacks::virtualMicProcess);
const pw_stream_events kMonitorEvents = makeStreamEvents(&EngineCallbacks::monitorProcess);

const pw_core_events kCoreEvents = [] {
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.done = &EngineCallbacks::coreDone;
    events.error = &EngineCallbacks::coreError;
    return events;
}();

const pw_registry_events kRegistryEvents = [] {
    pw_registry_events events{};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = &EngineCallbacks::registryGlobal;
    events.global_remove = &EngineCallbacks::registryGlobalRemove;
    return events;
}();

const pw_metadata_events kMetadataEvents = [] {
    pw_metadata_events events{};
    events.version = PW_VERSION_METADATA_EVENTS;
    events.property = &EngineCallbacks::metadataProperty;
    return events;
}();

const spa_pod* buildFormat(spa_pod_builder* builder, std::uint32_t channels) {
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kSampleRate;
    info.channels = channels;
    if (channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    return spa_format_audio_raw_build(builder, SPA_PARAM_EnumFormat, &info);
}

std::expected<void, std::string> createStream(Engine::Slot& slot, pw_core* core, const char* name,
                                              pw_properties* props, const pw_stream_events* events) {
    slot.stream = pw_stream_new(core, name, props);
    if (!slot.stream) {
        return std::unexpected(std::format("cannot create stream \"{}\"", name));
    }
    pw_stream_add_listener(slot.stream, &slot.listener, events, &slot);
    return {};
}

std::expected<void, std::string> connectStream(Engine::Slot& slot, pw_direction direction, std::uint32_t channels) {
    std::array<std::uint8_t, 1024> buffer{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, buffer.data(), buffer.size());
    const spa_pod* params[] = {buildFormat(&builder, channels)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                    PW_STREAM_FLAG_RT_PROCESS);
    if (int result = pw_stream_connect(slot.stream, direction, PW_ID_ANY, flags, params, 1); result < 0) {
        return std::unexpected(std::format("cannot connect stream: {}", spa_strerror(result)));
    }
    return {};
}

// Returns the writable float area of a dequeued buffer, sized to the frames PipeWire asked for.
std::span<float> outputArea(pw_buffer* buffer, std::uint32_t channels) {
    spa_data& data = buffer->buffer->datas[0];
    if (!data.data) {
        return {};
    }
    const std::uint32_t maxFrames = data.maxsize / (sizeof(float) * channels);
    std::uint32_t frames = maxFrames;
    if (buffer->requested > 0) {
        frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(buffer->requested, maxFrames));
    }
    return {static_cast<float*>(data.data), static_cast<std::size_t>(frames) * channels};
}

void finishOutput(pw_buffer* buffer, std::span<float> samples, std::uint32_t channels) {
    for (float& sample : samples) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }
    spa_data& data = buffer->buffer->datas[0];
    data.chunk->offset = 0;
    data.chunk->stride = static_cast<std::int32_t>(sizeof(float) * channels);
    data.chunk->size = static_cast<std::uint32_t>(samples.size_bytes());
}

const char* lookup(const spa_dict* props, const char* key) { return props ? spa_dict_lookup(props, key) : nullptr; }

}  // namespace

void EngineCallbacks::coreDone(void* data, std::uint32_t id, int seq) {
    auto* graph = static_cast<Engine::Graph*>(data);
    if (id != PW_ID_CORE || seq != graph->syncSeq) {
        return;
    }
    if (++graph->syncRound < kStartupSyncRounds) {
        graph->syncSeq = pw_core_sync(graph->engine->core_, PW_ID_CORE, graph->syncSeq);
        return;
    }
    graph->engine->graphReady();
}

void EngineCallbacks::coreError(void*, std::uint32_t id, int, int res, const char* message) {
    std::fprintf(stderr, "ozzsoundboard: PipeWire error on object %u: %s (%s)\n", id, message ? message : "",
                 spa_strerror(res));
}

void EngineCallbacks::registryGlobal(void* data, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t,
                                     const spa_dict* props) {
    auto* graph = static_cast<Engine::Graph*>(data);
    Engine* engine = graph->engine;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* mediaClass = lookup(props, PW_KEY_MEDIA_CLASS);
        const char* name = lookup(props, PW_KEY_NODE_NAME);
        if (!mediaClass || !name ||
            (std::strcmp(mediaClass, "Audio/Source") != 0 && std::strcmp(mediaClass, "Audio/Source/Virtual") != 0) ||
            engine->options_.nodeName == name) {
            return;
        }
        const char* description = lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (!description) {
            description = lookup(props, PW_KEY_NODE_NICK);
        }
        const char* priority = lookup(props, PW_KEY_PRIORITY_SESSION);
        engine->sourceAdded(id, InputSource{name, description ? description : name,
                                            priority ? static_cast<std::int32_t>(std::atoi(priority)) : 0});
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !graph->metadata) {
        const char* metadataName = lookup(props, PW_KEY_METADATA_NAME);
        if (!metadataName || std::strcmp(metadataName, "default") != 0) {
            return;
        }
        graph->metadata =
            static_cast<pw_metadata*>(pw_registry_bind(graph->registry, id, type, PW_VERSION_METADATA, 0));
        if (graph->metadata) {
            graph->metadataId = id;
            pw_metadata_add_listener(graph->metadata, &graph->metadataListener, &kMetadataEvents, graph);
        }
    }
}

void EngineCallbacks::registryGlobalRemove(void* data, std::uint32_t id) {
    auto* graph = static_cast<Engine::Graph*>(data);
    if (id == graph->metadataId && graph->metadata) {
        spa_hook_remove(&graph->metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(graph->metadata));
        graph->metadata = nullptr;
        graph->metadataId = SPA_ID_INVALID;
        graph->engine->defaultChanged({});
        return;
    }
    graph->engine->sourceRemoved(id);
}

int EngineCallbacks::metadataProperty(void* data, std::uint32_t subject, const char* key, const char*,
                                      const char* value) {
    auto* graph = static_cast<Engine::Graph*>(data);
    if (subject != PW_ID_CORE) {
        return 0;
    }
    if (!key) {
        graph->engine->defaultChanged({});
    } else if (std::strcmp(key, "default.audio.source") == 0) {
        graph->engine->defaultChanged(value ? parseDefaultNodeName(value).value_or("") : "");
    }
    return 0;
}

Engine::Engine(Mixer& mixer, EngineOptions options) : mixer_(mixer), options_(std::move(options)) {}

Engine::~Engine() { stop(); }

std::expected<void, std::string> Engine::start() {
    if (loop_) {
        return {};
    }
    loop_ = pw_thread_loop_new("ozzsoundboard", nullptr);
    if (!loop_) {
        return std::unexpected("cannot create PipeWire loop");
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        stop();
        return std::unexpected("cannot create PipeWire context");
    }
    if (pw_thread_loop_start(loop_) < 0) {
        stop();
        return std::unexpected("cannot start PipeWire loop");
    }

    // Every slot exists before any stream connects: realtime callbacks read these pointers unlocked.
    virtualMic_ = new Slot{Slot::Kind::VirtualMic, this};
    capture_ = options_.captureMic ? new Slot{Slot::Kind::Capture, this} : nullptr;
    monitor_ = options_.monitor ? new Slot{Slot::Kind::Monitor, this} : nullptr;

    pw_thread_loop_lock(loop_);
    auto result = [&]() -> std::expected<void, std::string> {
        core_ = pw_context_connect(context_, nullptr, 0);
        if (!core_) {
            return std::unexpected("cannot connect to PipeWire");
        }

        pw_properties* micProps = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_NAME,
            options_.nodeName.c_str(), PW_KEY_NODE_DESCRIPTION, options_.nodeDescription.c_str(),
            PW_KEY_APP_NAME, "OzzSoundBoard", PW_KEY_NODE_LATENCY, kLatency, PW_KEY_NODE_ALWAYS_PROCESS, "true",
            PW_KEY_NODE_WANT_DRIVER, "true", PW_KEY_PRIORITY_SESSION, "0", "device.icon-name",
            "audio-input-microphone", nullptr);
        if (auto ok = createStream(*virtualMic_, core_, options_.nodeDescription.c_str(), micProps,
                                   &kVirtualMicEvents);
            !ok) {
            return ok;
        }
        if (auto ok = connectStream(*virtualMic_, PW_DIRECTION_OUTPUT, 1); !ok) {
            return ok;
        }

        if (monitor_) {
            const std::string name = options_.nodeName + "_monitor";
            pw_properties* props = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Production",
                PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, "OzzSoundBoard monitor", PW_KEY_APP_NAME,
                "OzzSoundBoard", PW_KEY_NODE_LATENCY, kLatency, nullptr);
            if (auto ok = createStream(*monitor_, core_, "OzzSoundBoard monitor", props, &kMonitorEvents); !ok) {
                return ok;
            }
            if (auto ok = connectStream(*monitor_, PW_DIRECTION_OUTPUT, 2); !ok) {
                return ok;
            }
        }

        if (capture_) {
            // Connected once a microphone is chosen; target.object pins it so the session manager never
            // falls back to the default source, which may be our own virtual microphone.
            const std::string name = options_.nodeName + "_capture";
            pw_properties* props = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication",
                PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, "OzzSoundBoard microphone input",
                PW_KEY_APP_NAME, "OzzSoundBoard", PW_KEY_NODE_LATENCY, kLatency, PW_KEY_NODE_DONT_RECONNECT, "true",
                "node.dont-fallback", "true", "node.dont-move", "true", nullptr);
            if (auto ok = createStream(*capture_, core_, "OzzSoundBoard capture", props, &kCaptureEvents); !ok) {
                return ok;
            }
        }

        graph_ = new Graph{this};
        graph_->registry = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        if (!graph_->registry) {
            return std::unexpected("cannot read the PipeWire registry");
        }
        pw_core_add_listener(core_, &graph_->coreListener, &kCoreEvents, graph_);
        pw_registry_add_listener(graph_->registry, &graph_->registryListener, &kRegistryEvents, graph_);
        graph_->syncSeq = pw_core_sync(core_, PW_ID_CORE, 0);
        return {};
    }();
    pw_thread_loop_unlock(loop_);

    if (!result) {
        stop();
    }
    return result;
}

void Engine::stop() {
    if (!loop_) {
        return;
    }
    pw_thread_loop_lock(loop_);
    for (Slot** slot : {&virtualMic_, &capture_, &monitor_}) {
        if (*slot) {
            if ((*slot)->stream) {
                pw_stream_destroy((*slot)->stream);
            }
            delete *slot;
            *slot = nullptr;
        }
    }
    if (graph_) {
        if (graph_->metadata) {
            spa_hook_remove(&graph_->metadataListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(graph_->metadata));
        }
        if (graph_->registry) {
            spa_hook_remove(&graph_->registryListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(graph_->registry));
        }
        spa_hook_remove(&graph_->coreListener);
        delete graph_;
        graph_ = nullptr;
    }
    if (core_) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    sources_.clear();
    activeInput_.clear();
    captureConnected_ = false;
    ready_ = false;
    pw_thread_loop_unlock(loop_);

    pw_thread_loop_stop(loop_);
    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    mixer_.setTargetActive(Target::Mic, false);
    mixer_.setTargetActive(Target::Monitor, false);
}

void Engine::setPreferredInput(std::string name) {
    if (!loop_) {
        preferred_ = std::move(name);
        return;
    }
    pw_thread_loop_lock(loop_);
    preferred_ = std::move(name);
    reevaluateInput();
    pw_thread_loop_unlock(loop_);
}

Engine::Inputs Engine::inputs() const {
    Inputs inputs;
    if (!loop_) {
        inputs.preferred = preferred_;
        return inputs;
    }
    pw_thread_loop_lock(loop_);
    for (const auto& [id, source] : sources_) {
        inputs.sources.push_back(source);
    }
    inputs.systemDefault = systemDefault_;
    inputs.active = activeInput_;
    inputs.preferred = preferred_;
    pw_thread_loop_unlock(loop_);
    std::ranges::sort(inputs.sources, {}, &InputSource::description);
    return inputs;
}

Engine::Stats Engine::stats() const {
    Stats stats;
    stats.capture = capture_ && capture_->streaming.load();
    stats.virtualMic = virtualMic_ && virtualMic_->streaming.load();
    stats.monitor = monitor_ && monitor_->streaming.load();
    stats.micUnderruns = micUnderruns_.load();
    const Inputs current = inputs();
    for (const InputSource& source : current.sources) {
        if (source.name == current.active) {
            stats.input = source.description;
        }
    }
    return stats;
}

void Engine::graphReady() {
    ready_ = true;
    reevaluateInput();
}

void Engine::sourceAdded(std::uint32_t id, InputSource source) {
    sources_.insert_or_assign(id, std::move(source));
    reevaluateInput();
}

void Engine::sourceRemoved(std::uint32_t id) {
    if (sources_.erase(id) > 0) {
        reevaluateInput();
    }
}

void Engine::defaultChanged(std::string name) {
    if (!name.empty() && name != options_.nodeName) {
        lastRealDefault_ = name;
    }
    systemDefault_ = std::move(name);
    reevaluateInput();
}

void Engine::reevaluateInput() {
    if (!ready_ || !capture_) {
        return;
    }
    InputState state{preferred_, systemDefault_, lastRealDefault_, options_.nodeName, {}};
    for (const auto& [id, source] : sources_) {
        state.sources.push_back(source);
    }
    const std::string target = chooseInput(state).value_or("");
    if (target != activeInput_ || (!target.empty() && !captureConnected_)) {
        retargetCapture(target);
    }
}

void Engine::retargetCapture(const std::string& target) {
    if (captureConnected_) {
        pw_stream_disconnect(capture_->stream);
        captureConnected_ = false;
    }
    activeInput_ = target;
    if (target.empty()) {
        std::fprintf(stderr, "ozzsoundboard: no microphone available\n");
        return;
    }

    spa_dict_item items[] = {{PW_KEY_TARGET_OBJECT, target.c_str()}};
    spa_dict dict{0, 1, items};
    pw_stream_update_properties(capture_->stream, &dict);
    if (auto connected = connectStream(*capture_, PW_DIRECTION_INPUT, 1); !connected) {
        std::fprintf(stderr, "ozzsoundboard: %s\n", connected.error().c_str());
        return;
    }
    captureConnected_ = true;
    std::fprintf(stdout, "ozzsoundboard: capturing from %s\n", target.c_str());
    std::fflush(stdout);
}

void Engine::stateChanged(Slot& slot, int state, const char* error) {
    const bool streaming = state == PW_STREAM_STATE_STREAMING;
    slot.streaming.store(streaming);
    if (slot.kind == Slot::Kind::VirtualMic) {
        mixer_.setTargetActive(Target::Mic, streaming);
    } else if (slot.kind == Slot::Kind::Monitor) {
        mixer_.setTargetActive(Target::Monitor, streaming);
    }
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "ozzsoundboard: stream error: %s\n", error ? error : "unknown");
    }
}

void Engine::processCapture() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(capture_->stream);
    if (!buffer) {
        return;
    }
    const spa_data& data = buffer->buffer->datas[0];
    if (data.data && data.chunk) {
        const std::uint32_t offset = std::min(data.chunk->offset, data.maxsize);
        const std::uint32_t size = std::min(data.chunk->size, data.maxsize - offset);
        const auto* samples = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(data.data) + offset);
        micRing_.write(std::span(samples, size / sizeof(float)));
    }
    pw_stream_queue_buffer(capture_->stream, buffer);
}

void Engine::processVirtualMic() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(virtualMic_->stream);
    if (!buffer) {
        return;
    }
    std::span<float> out = outputArea(buffer, 1);
    if (out.empty()) {
        pw_stream_queue_buffer(virtualMic_->stream, buffer);
        return;
    }

    std::size_t got = 0;
    if (capture_) {
        if (const std::size_t queued = micRing_.size(); queued > out.size() * kMaxQueuedQuanta) {
            micRing_.discard(queued - out.size());
        }
        got = micRing_.read(out);
        if (got < out.size() && capture_->streaming.load(std::memory_order_relaxed)) {
            micUnderruns_.fetch_add(1, std::memory_order_relaxed);
        }
        const float gain = micGain_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < got; ++i) {
            out[i] *= gain;
        }
    }
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(got), out.end(), 0.0f);

    mixer_.render(Target::Mic, out, 1);
    finishOutput(buffer, out, 1);
    pw_stream_queue_buffer(virtualMic_->stream, buffer);
}

void Engine::processMonitor() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(monitor_->stream);
    if (!buffer) {
        return;
    }
    std::span<float> out = outputArea(buffer, 2);
    std::ranges::fill(out, 0.0f);
    mixer_.render(Target::Monitor, out, 2);
    finishOutput(buffer, out, 2);
    pw_stream_queue_buffer(monitor_->stream, buffer);
}

}  // namespace ozzsb
