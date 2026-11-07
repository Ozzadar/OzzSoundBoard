# OzzSoundBoard

A Linux soundboard with its own virtual microphone. A daemon captures a real microphone, mixes sound clips into it, and publishes the result as a PipeWire source called **OzzSoundBoard Microphone**. Pick
that device as the microphone in Discord, Slack, a browser or OBS. Clips also play through your speakers so you
hear what you send; your own voice is not echoed back to you.

Sounds are a plain name → audio file map. Anything libsndfile reads works (WAV, FLAC, Ogg/Opus, MP3, AIFF).
Clips are decoded once, resampled to 48 kHz and kept in memory, so playback starts on the next audio cycle.

## Build

Needs a C++23 compiler (GCC 14+ or Clang 18+), CMake 3.28+, Ninja and these packages (Ubuntu names):

```sh
sudo apt install libpipewire-0.3-dev libspa-0.2-dev libsndfile1-dev libsamplerate0-dev \
    nlohmann-json3-dev qt6-base-dev libgtest-dev
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build
ctest --test-dir build          # or ./build/ozzsb_tests
cmake --install build
```

`-DOZZSB_BUILD_GUI=OFF` and `-DOZZSB_BUILD_TESTS=OFF` drop the Qt GUI and the tests.

## Run

```sh
systemctl --user daemon-reload
systemctl --user enable --now ozzsoundboard   # start now and at every login
```

Then open **OzzSoundBoard** from the app menu (`ozzsoundboard-gui`), add sounds, and choose
**OzzSoundBoard Microphone** as the input device in your voice apps.

OzzSoundBoard Microphone can be your *system default* input, so every app gets your voice plus the soundboard
without per-app setup. The daemon never records itself: in automatic mode it follows the system default, and
when that is the soundboard it keeps using the last real microphone (or the highest-priority one at startup).
Pick a fixed microphone in the GUI or with `ozzsoundboard input <name>` to stop following the default. A fixed
microphone that is unplugged falls back to automatic until it returns.

## GUI

- **Microphone** picks what gets mixed into the virtual mic: *Automatic* or a specific device (● marks the one in use).
- **Add…** picks one or more audio files; the name defaults to the file name. Names and paths edit in place.
- **Preview** plays a sound through your speakers only. Nobody on a call hears it.
- **Play** plays it into the virtual microphone and your speakers.
- **Copy deck command** copies the Stream Deck command for the selected sound.
- The three volumes control your live microphone, sounds sent into the microphone, and sounds in your speakers.

Every edit is saved immediately and the daemon reloads.

## Command line

```text
ozzsoundboard play <name>        into the virtual mic and the speakers
ozzsoundboard preview <name>     speakers only
ozzsoundboard toggle <name>      stop it if it is playing, otherwise play it
ozzsoundboard stop [name]        stop one sound, or everything
ozzsoundboard list
ozzsoundboard add <name> <path>
ozzsoundboard remove <name>
ozzsoundboard inputs             microphones: node name, description, flags (default/active/preferred)
ozzsoundboard input [name|auto]  show or set the captured microphone
ozzsoundboard reload
ozzsoundboard status
ozzsoundboard daemon [--node-name NAME] [--description TEXT] [--no-mic] [--no-monitor]
```

Names may contain spaces: `ozzsoundboard play air horn` and `ozzsoundboard play 'air horn'` are the same.
`--config PATH` and `--socket PATH` (before the command) or `OZZSOUNDBOARD_CONFIG` / `OZZSOUNDBOARD_SOCKET`
point at another instance.

## Stream Deck (OpenDeck)

Use the **Run Command** action with the key-down command set to:

```sh
/home/<you>/.local/bin/ozzsoundboard play 'air horn'
```

Use the absolute path: OpenDeck runs commands through `sh` without your login `PATH`. `toggle` suits long clips
and a `stop` key makes a panic button.

## Config

`$XDG_CONFIG_HOME/ozzsoundboard/sounds.json`:

```json
{
  "sounds": {
    "air horn": "/home/me/Sounds/airhorn.wav"
  },
  "gains": { "mic": 1.0, "clipsToMic": 1.0, "clipsToMonitor": 1.0 },
  "input": ""
}
```

After editing it by hand run `ozzsoundboard reload` (or `systemctl --user reload ozzsoundboard`).

## How it works

`input` is a microphone's PipeWire node name (see `ozzsoundboard inputs`); empty means automatic.

```text
chosen mic  ──► capture stream ──► SPSC ring ──┐
                                               ├─► OzzSoundBoard Microphone (Audio/Source, mono)
clips ───────► Mixer ──────────────────────────┤
                    └──────────────────────────┴─► monitor stream ──► default speakers (stereo)
```

- `src/audio/Engine` owns three PipeWire streams on one thread loop, all processed on the realtime data thread.
  It watches the registry for `Audio/Source` nodes and the `default` metadata for the default source, and
  reconnects only the capture stream (pinned with `target.object`, `node.dont-fallback`) when the choice made by
  `src/audio/InputSelection` changes.
  The virtual microphone sets `node.always-process`, so clips run on schedule even when nothing is listening.
- `src/audio/Mixer` never allocates or frees on the audio thread. Commands arrive through a lock-free queue, each
  voice keeps a separate play position per output, and clips are released on the control thread once the audio
  side reports the voice finished.
- The microphone ring keeps at most three periods queued and drops the oldest audio beyond that, so latency
  cannot creep up when the microphone and the virtual source run on different clocks.
- `src/ipc` is a one-request-per-connection line protocol over `$XDG_RUNTIME_DIR/ozzsoundboard.sock`.
