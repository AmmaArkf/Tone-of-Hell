# Tone of Hell

An open digital audio workstation (DAW) written in C++: recording, a multitrack mixer, MIDI and hosting of third-party plugins. The source code is open to everyone, and the project is developed together with its community.

> **Status: early development.** Planning and the first stage (the audio engine) are in progress. There is nothing to download yet, but ideas, questions and proposals are welcome right now. See [ROADMAP.md](ROADMAP.md) for the detailed plan.

## Goals

- A free program for recording, editing and mixing music.
- Fully open source code and open formats: MIDI, WAV, CLAP, VST3.
- Windows first, then Linux and macOS. The code is written to be portable.
- Hosting of third-party plugins (CLAP first, then VST3).
- Extensibility: built-in effects and instruments, automation and scripting.

## What this project is not

This is an original program, not a copy of anyone else's DAW. We do not use or accept:

- code, user interface, icons, sounds, loops or presets taken from other programs;
- code or data obtained by disassembling, decompiling or reverse engineering closed programs or their file formats;
- material whose license does not allow redistribution in an open source project.

Ideas and well-known principles of audio software are fine to use; protected code and material are not. Details are in [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

The status of every stage is tracked in [ROADMAP.md](ROADMAP.md). In short:

| Level | What it includes | Status |
| --- | --- | --- |
| 0. Core | Audio engine, transport, tracks, regions, mixer, recording, import and export, projects, undo/redo | Planned |
| 1. Working minimum | MIDI and piano roll, CLAP/VST3 plugins, automation, tempo map, basic effects and instruments | Planned |
| 2. Advanced | Comping, time-stretch, groups, sidechain, controllers, score editor, clip grid | Idea |
| 3. Distinctive | ML features, sound library, surround sound, mastering assistant | Idea |

## Building

Instructions will appear together with the first code (stage 1). Planned requirements:

- a compiler with C++17 support or newer (MSVC, GCC, Clang);
- CMake 3.20 or newer;
- Git.

## How to help

You can help without writing any code. Pick what suits you:

- **Found a bug?** Open an issue with the "Report a bug" template in the Issues tab.
- **Have an idea, or want to automate a chore?** Use the "Suggest an idea or an automation" template.
- **Want to propose a plugin, an effect, an instrument or support for a format?** Use the "Suggest a plugin or an integration" template.
- **Have a question, or want to discuss something?** Open the Discussions tab.
- **Want to write code?** Read [CONTRIBUTING.md](CONTRIBUTING.md) and look for issues labeled `good first issue`.

Before opening an issue, search for a similar one: it may already be under discussion. If you find one, add a 👍 and your own details.

## Security

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md), not in public issues.

## Community rules

Taking part in the project means agreeing to the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

The project is released under the MIT license, see [LICENSE](LICENSE).
