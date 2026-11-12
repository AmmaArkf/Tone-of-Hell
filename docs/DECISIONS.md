# Decisions

A short record of technical decisions: what was chosen, why, and who decided. Decisions marked "default" were made during implementation and can be changed by the project owner; say so in an issue.

| # | Decision | Why | Decided by |
| --- | --- | --- | --- |
| 1 | License: MIT | Free to use, attribution required | Owner |
| 2 | Platforms: Windows and Linux from the start, no macOS | Owner's priority | Owner |
| 3 | Language: C++17 | Supported by every current compiler; enough for the engine | Default |
| 4 | Build system: CMake (3.20 or newer) | Same build on Windows, Linux and CI, independent of any editor | Default |
| 5 | Audio I/O for stage 1: miniaudio 0.11.26 | One header, public domain or MIT-0, WASAPI on Windows and ALSA/PulseAudio/JACK on Linux. ASIO comes later | Default |
| 6 | Tests: doctest 2.5.3 | One header, MIT, fast to compile | Default |
| 7 | Third-party code is copied into `third_party/` with version and checksum | Builds work offline and are reproducible | Default |
| 8 | The core (`toh_core`) has no dependency on audio hardware or any library | The engine can be tested without a sound card; backends can be replaced | Default |
| 9 | Control thread talks to the audio thread through a lock-free single-producer queue | Real-time rule: no locks or allocation on the audio thread | Default |
| 10 | Stage 1 loads a whole file into memory (30-minute limit) | Simplest correct start; streaming from disk comes with the timeline | Default |
| 11 | Play, pause, stop and seek use a 5 ms fade; fader, pan, mute and solo changes glide over about 10 ms | Prevents clicks | Default |
| 12 | Internal format: 32-bit float, interleaved; default 48 kHz stereo, 256-frame buffer | Common practice; matches the roadmap criterion | Default |
| 13 | Fixed mixer size for now: 64 tracks and 8 buses, one clip per track starting at time 0 | No allocation on the audio thread; regions and dynamic track counts come with the timeline (stage 3) | Default |
| 14 | The mix is never clipped or limited inside the engine. Samples above full scale at the master are counted for a clip indicator, and the audio device clamps them | In-range audio must pass unchanged; a limiter is a stage 9 effect | Default |
| 15 | Pan law: constant-power curve scaled so that the centre is unity gain on both channels | A centred track sounds exactly like the file | Default |
| 16 | Mute wins over solo; buses have no solo yet | Simplest predictable rule | Default |
| 17 | Meters show the post-fader peak with a 0.3 s release, one value per track, bus and master | Enough for a first mixer; stereo and loudness meters come with the GUI | Default |
| 18 | Two user interfaces: Dear ImGui (with SDL3) and Qt 6. Users choose which to build and install, and can switch between them when both are installed | Owner's choice: people prefer different interfaces | Owner |
| 19 | All program logic lives in `toh_app` (the `Session` class). A user interface only draws the session and forwards actions to it | Two interfaces must behave the same; logic is written and tested once | Default |
| 20 | The ImGui interface is the primary one: a feature lands there first, then in the Qt interface | Keeps the two from blocking each other | Default (proposed to the owner) |
| 21 | Switching interfaces restarts the window with the same project instead of swapping toolkits inside one process | Two windowing toolkits in one process are fragile | Default (proposed to the owner) |
| 22 | SDL3 is downloaded by CMake with a fixed checksum instead of being copied into the repository | The source archive is about 16 MB; exception to decision 7 | Default |
| 23 | The Qt interface is distributed under the terms of the LGPL (Qt as separate shared libraries); the project's own code stays MIT | Required by Qt's open source license | Default |
| 24 | A track holds regions (pieces of clips with a start, an offset, a length and two fades). The audio thread receives a complete new region list for a track on every edit | Edits never stop playback and need no locks; the old list is freed on the control thread | Default |
| 25 | Regions that overlap on one track are mixed together | Simplest rule; "the upper one wins" and crossfades can come later | Default |
| 26 | Region fades are linear | Predictable; other curves come with the fade editor | Default |
| 27 | The grid comes from the metronome settings (one tempo and time signature for the whole project); edits snap to bars, beats, half or quarter beats, and Alt switches snapping off while dragging | A tempo map is stage 8 | Default |
| 28 | Reverse playback is a transport direction, not a region effect; the metronome is generated from the timeline position | Both stay in step through seeks and loops | Default |
| 29 | The input device is opened only when the user asks for it (Record, or the recording settings), together with the output as one duplex stream | The microphone is not touched before it is needed; one stream keeps input and output in step | Default |
| 30 | The audio thread copies the input to a lock-free ring buffer (about 4 s); the session collects it on the UI thread and turns the finished take into a clip and a region | No allocation or file access on the audio thread; takes are kept in memory until projects can be saved (stage 5) | Default |
| 31 | A take is one unbroken forward run: it ends when playback stops, pauses, jumps or turns around. While recording, playback does not stop at the end of the project | Keeps every recorded frame at a known place on the timeline | Default |
| 32 | A take is placed earlier by a "latency correction" (default: two audio buffers; adjustable). The sound card does not report its true delay, so the default is an estimate | Recordings must line up with what was heard; exact calibration needs a loopback measurement, planned later | Default |
| 33 | Export renders the project with a second, offline engine: 16-bit, 24-bit or 32-bit float WAV, without the metronome and the monitored input, without dither | Playback is not disturbed and the file equals what the mixer produces; dither comes with mastering features | Default |

## Open decisions

| Decision | Needed by |
| --- | --- |
| Plugin formats and SDK licenses (CLAP first, then VST3) | Stage 7 |
| Project file format | Stage 5 |
