# Roadmap

Features are listed in descending order of necessity. Every level ends with a [stabilization phase](CONTRIBUTING.md#stabilization-bugs-are-fixed-at-the-end-of-each-level): we fix the bugs that were found and add no new features. We do not estimate dates, because they depend on the number of contributors and on how many iterations each stage needs.

Proposals for the plan are welcome through the "Suggest an idea or an automation" and "Suggest a plugin or an integration" templates.

## Development stages

A stage is done when the criterion in parentheses is met and has been checked on a real computer.

- [x] **1. Audio engine and player.** A WAV file plays through the sound card and play, stop, position and loop work (no clicks or dropouts at a 256-sample buffer).
- [x] **2. Tracks and mixer.** Volume, pan, mute, solo, buses and level meters (8 tracks play in sync and the sum does not clip).
- [x] **3. Timeline and regions.** Tracks with waveforms, moving, trimming, fades and a grid (an arrangement can be assembled with the mouse).
- [x] **4. Recording and export.** Recording from an input, monitoring and export to WAV (the recorded and exported audio line up in time with what you hear in playback).
- [x] **5. Project and undo.** Saving, loading, autosave and undo/redo (a project opens without losses).
- [x] **6. MIDI, piano roll and a simple synthesizer.** (A melody can be played from a keyboard, edited in the piano roll and played back.)
- [ ] **7. Plugin hosting.** CLAP first, then VST3, with delay compensation (a third-party synthesizer and reverb work in the mix and keep their state).
- [ ] **8. Automation and tempo map.** (Volume and plugin parameters follow curves, and the tempo can change inside a track.)
- [ ] **9. Built-in effects.** Equalizer, compressor, reverb, delay and limiter (each is checked on test signals and produces no clicks).
- [ ] **10. Advanced features by community demand.** The order is chosen by votes on issues.

Level 0 is complete: version 0.2 (see Releases). Level 1 starts with stage 6. A pre-release of level 0 comes out after stages 1 to 5, and of level 1 after stages 6 to 9. A stabilization phase runs before every release.

## Features by level

### Level 0. Core

Audio engine, transport, tracks, regions on the timeline, mixer, audio recording, import and export, projects and undo/redo.

### Level 1. Working minimum

MIDI and piano roll, hosting of CLAP and VST3 plugins, automation, tempo map, basic built-in effects and instruments, usability (shortcuts, multiple selection).

### Level 2. Advanced

Take comping, punch in/out and loop recording, waveform editing, time-stretch and pitch-shift, groups and stacks, sidechain, external controllers, score editor, clip grid, markers, video.

### Level 3. Distinctive and heavy

ML features (when open models with a suitable license are available), a sound library with a free license, advanced synthesizers, surround sound, a mastering assistant, smart tempo.

## How to influence the plan

- Add a 👍 to an issue with an idea you need too.
- Suggest a plugin, an effect or an integration using the template.
- Take an issue labeled `help wanted`.
