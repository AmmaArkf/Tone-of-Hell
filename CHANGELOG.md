# Changelog

All notable changes to this project are listed here, newest first. The version numbers follow the rules in [docs/VERSIONING.md](docs/VERSIONING.md).

## 0.0.8

- While recording, the take shows its waveform as it grows, so you can see what is being recorded. Parts that reach full scale are drawn red, and the take is marked "Input overloaded".
- An input level meter appears next to the Record button while the input device is open.

## 0.0.7

- A double-click on a slider returns it to its normal value: track volume and master volume to 100%, pan to the centre, input sensitivity to 100%, metronome click volume to 50%.

## 0.0.6

- Export to MP3: File > Export audio now also offers MP3 at 320 kbps, 192 kbps or variable bit rate (best quality). The files carry a header with their exact length.
- MP3 encoding uses LAME 4.0, built as a separate library (LGPL) that is downloaded while the build is configured. The project's own code stays under the MIT license.
- CMake 3.21 or newer is now needed.

## 0.0.5

- A settings window (Edit > Settings) with three tabs:
  - General: the interface language, English or Russian.
  - Audio: choice of the output and input devices, output volume, input sensitivity and an input level meter.
  - Shortcuts: every keyboard shortcut can be changed or removed, and all can be reset to the defaults. Menus show the current shortcuts.
- Settings are remembered between runs in a settings file in the user's application data folder.
- New default shortcuts: L for loop, K for the metronome, F to show the whole project.

## 0.0.4

- The version number now changes with every change, and this changelog records what each version brought.

## 0.0.3 and earlier

Development before the changelog was started:

- **Recording and export (stage 4).** Recording from the input device onto a track with a latency correction, input monitoring, and export of the mix to WAV (16-bit, 24-bit, 32-bit float).
- **Menus and navigation.** A menu bar, a status bar with project, audio and build figures, a timeline scrollbar, mouse-wheel zoom, automatic scrolling while dragging, and an option to switch the grid off.
- **Timeline and regions (stage 3).** Regions that can be moved, trimmed, cut and faded, snapping to bars and beats, zoom and scroll, a ruler with bars and time.
- **The ImGui interface.** A window with tracks, waveforms, transport and mixer controls; reverse playback; a metronome with tempo, time signature and tap tempo.
- **Tracks and mixer (stage 2).** Up to 64 tracks with fader, pan, mute and solo, 8 buses, a master fader and level meters.
- **Audio engine and player (stage 1).** A real-time safe engine with play, pause, stop, seek and loop, audio output, decoding of WAV, FLAC and MP3 files, and a command-line test player.
