# Building

You need Git, CMake 3.21 or newer and a C++17 compiler (and a C compiler, for the LAME MP3 encoder). Most libraries the project uses are included in `third_party/`. SDL3 (used by the ImGui interface) and LAME (the MP3 encoder) are downloaded automatically the first time you configure, so that step needs an internet connection; add `-DTOH_BUILD_GUI_IMGUI=OFF` to build without it.

## Windows (MinGW-w64)

```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
ctest --test-dir build --output-on-failure
build\apps\toh_imgui\toh_imgui.exe
```

With Visual Studio installed you can leave out `-G "MinGW Makefiles"`. The first build takes a few minutes because SDL is compiled.

## Linux

Install the packages SDL needs to open a window. On Debian or Ubuntu:

```
sudo apt install pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
  libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdbus-1-dev libudev-dev
```

Then:

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/apps/toh_imgui/toh_imgui
```

Sound goes through PulseAudio (or PipeWire's PulseAudio layer), ALSA or JACK, whichever is found when the program starts.

## Programs

### toh_imgui: the ImGui interface

Drop audio files (WAV, FLAC or MP3) onto the window or press "Add files...". Every file becomes a track with a volume slider, a pan slider, mute (M), solo (S) and remove (X). Click or drag in the ruler or on a waveform to move the playhead. Space starts and pauses playback; Home returns to the start.

Editing on the timeline:

| Action | How |
| --- | --- |
| Move a region | Drag it |
| Trim a region | Drag its left or right edge |
| Fade in or out | Drag the small square in a top corner of the region |
| Cut | `S` cuts the selected region at the playhead (without a selection: every region under it) |
| Remove a region | Select it and press `Delete`, or right-click it |
| Snap to the grid | "Snap to" in the toolbar; hold `Alt` while dragging to switch it off |
| Zoom | `Ctrl` + mouse wheel, or the `-`, `+` and "Fit" buttons |
| Scroll sideways | `Shift` + mouse wheel |

The grid follows the tempo and the time signature of the metronome. "Reverse" plays backwards from the playhead.

Recording: press "Record" (or `R`). The input device is opened the first time. The take goes onto the track whose `R` button is lit, or onto a new track. Press "Record" again, or "Stop", to finish. The "..." button next to it opens the recording settings: monitoring (hear the input; use headphones) and the latency correction, which moves takes earlier to make up for the delay of the sound card.

Settings: "Edit > Settings" chooses the language, the sound devices, the output volume and the input sensitivity, and lets you change every keyboard shortcut. "Help > Keyboard and mouse" lists the shortcuts as they are set.

Export: "File > Export audio" writes the whole project to a WAV or MP3 file as it sounds, without the metronome.

### toh_player: the command-line test player

`toh_player` plays a 440 Hz test tone, or the files given as arguments, one per track. Options: `--loop`, `--buffer <frames>` (default 256) and `--rate <hz>` (default 48000).

While it runs, type a command and press Enter:

| Command | What it does |
| --- | --- |
| `p` | Play or pause |
| `s` | Stop and return to the start |
| `r` | Back to the start |
| `j <seconds>` | Jump to a time |
| `l` | Loop on or off |
| `+` / `-` | Master volume |
| `v <track> <gain>` | Track fader (1 = unchanged) |
| `n <track> <pan>` | Track pan, from -1 (left) to 1 (right) |
| `m <track>` | Mute on or off |
| `o <track>` | Solo on or off |
| `i` | Show the level meters |
| `q` | Quit |

Tracks are numbered from 1 in the order of the files.
