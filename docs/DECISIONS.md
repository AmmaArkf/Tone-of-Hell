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

## Open decisions

| Decision | Needed by |
| --- | --- |
| GUI framework | Stage 3 (timeline) |
| Plugin formats and SDK licenses (CLAP first, then VST3) | Stage 7 |
| Project file format | Stage 5 |
