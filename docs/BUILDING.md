# Building

You need Git, CMake 3.20 or newer and a C++17 compiler. The libraries the project uses are included in `third_party/`, so nothing else has to be installed.

## Windows (MinGW-w64)

```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
ctest --test-dir build --output-on-failure
build\apps\toh_player\toh_player.exe
```

With Visual Studio installed you can leave out `-G "MinGW Makefiles"`.

## Linux

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/apps/toh_player/toh_player
```

Sound goes through PulseAudio (or PipeWire's PulseAudio layer), ALSA or JACK, whichever is found when the program starts.

## The test player

`toh_player` plays a 440 Hz test tone, or the WAV, FLAC or MP3 files given as arguments. Several files play together, one per track. Options: `--loop`, `--buffer <frames>` (default 256) and `--rate <hz>` (default 48000).

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
