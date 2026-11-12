// toh_player: a small command-line player used to check the audio engine by ear.
//
//   toh_player                     plays a 440 Hz test tone
//   toh_player song.wav            plays a file (WAV, FLAC or MP3)
//   toh_player drums.wav bass.wav  plays several files together, one per track
//   options: --loop  --buffer <frames>  --rate <hz>
//
// While it runs, type a command and press Enter (see print_help below).

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif

#include "toh/audio/audio_device.h"
#include "toh/audio/clip_loader.h"
#include "toh/core/clip.h"
#include "toh/core/engine.h"

namespace {

using Path = std::filesystem::path;
using toh::core::Engine;

// Returns the command-line arguments (without the program name) as paths.
// On Windows the `argv` given to main() is in the legacy code page and loses characters that
// the code page cannot represent, so the arguments are read again as UTF-16.
std::vector<Path> command_line_arguments(int argc, char** argv) {
    std::vector<Path> arguments;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide != nullptr) {
        for (int i = 1; i < count; ++i) {
            arguments.emplace_back(std::wstring(wide[i]));
        }
        LocalFree(wide);
    }
#else
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
#endif
    return arguments;
}

// Reads a positive whole number from an argument; returns 0 when it is not one.
std::uint32_t to_number(const Path& argument) {
    std::uint32_t value = 0;
    for (const auto character : argument.native()) {
        if (character < '0' || character > '9' || value > 100000000u) {
            return 0;
        }
        value = value * 10 + static_cast<std::uint32_t>(character - '0');
    }
    return value;
}

void render_callback(void* user_data, float* out, std::uint32_t frames) {
    static_cast<Engine*>(user_data)->render(out, frames);
}

void print_help() {
    std::cout
        << "transport: p = play/pause, s = stop, r = back to start, j <seconds> = jump,\n"
           "           l = loop on/off, q = quit\n"
           "master:    + / - = volume\n"
           "tracks:    v <track> <gain> = fader (1 = unchanged), n <track> <pan> = pan (-1..1),\n"
           "           m <track> = mute on/off, o <track> = solo on/off, i = show levels\n"
           "           (tracks are numbered from 1 in the order of the files)\n";
}

}  // namespace

int main(int argc, char** argv) {
    toh::audio::DeviceConfig config;
    std::vector<Path> files;
    bool loop = false;

    const std::vector<Path> arguments = command_line_arguments(argc, argv);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const Path& argument = arguments[i];
        if (argument == Path("--loop")) {
            loop = true;
        } else if (argument == Path("--buffer") && i + 1 < arguments.size()) {
            config.buffer_frames = to_number(arguments[++i]);
        } else if (argument == Path("--rate") && i + 1 < arguments.size()) {
            config.sample_rate = to_number(arguments[++i]);
        } else if (argument == Path("--help") || argument == Path("-h")) {
            std::cout << "usage: toh_player [files...] [--loop] [--buffer <frames>] [--rate <hz>]\n";
            print_help();
            return 0;
        } else {
            files.push_back(argument);
        }
    }
    if (config.buffer_frames == 0 || config.sample_rate == 0) {
        std::cerr << "error: --buffer and --rate need a positive whole number\n";
        return 1;
    }
    if (files.size() > Engine::kMaxTracks) {
        std::cerr << "error: at most " << Engine::kMaxTracks << " files\n";
        return 1;
    }

    Engine engine(config.sample_rate, config.channels);
    std::uint32_t track_count = 0;

    if (files.empty()) {
        std::cout << "No file given: playing a 440 Hz test tone.\n";
        loop = true;
        if (!engine.set_track_clip(
                0, toh::core::make_sine_clip(440.0, 2.0, config.sample_rate, config.channels))) {
            std::cerr << "error: cannot set up the engine\n";
            return 1;
        }
        track_count = 1;
    } else {
        for (const Path& file : files) {
            std::string error;
            const auto clip =
                toh::audio::load_clip(file, config.sample_rate, config.channels, &error);
            if (!clip) {
                std::cerr << "error: file " << (track_count + 1) << ": " << error << "\n";
                return 1;
            }
            if (!engine.set_track_clip(track_count, clip)) {
                std::cerr << "error: cannot set up the engine\n";
                return 1;
            }
            ++track_count;
        }
    }

    if (!engine.set_loop(loop) || !engine.play()) {
        std::cerr << "error: cannot set up the engine\n";
        return 1;
    }

    toh::audio::Device device;
    std::string error;
    if (!device.start(config, &render_callback, &engine, &error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    const double rate = static_cast<double>(config.sample_rate);
    std::cout << "Playing " << track_count << " track(s) at " << config.sample_rate
              << " Hz, buffer " << config.buffer_frames << " frames" << (loop ? ", loop on" : "")
              << ".\n";
    print_help();

    float master_gain = 1.0f;
    std::vector<bool> muted(track_count, false);
    std::vector<bool> soloed(track_count, false);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        input >> command;

        // Commands that name a track: read its number (1-based) first.
        std::uint32_t track = 0;
        bool track_ok = false;
        if (command == "v" || command == "n" || command == "m" || command == "o") {
            unsigned number = 0;
            if (input >> number && number >= 1 && number <= track_count) {
                track = number - 1;
                track_ok = true;
            } else {
                std::cout << "track number must be between 1 and " << track_count << "\n";
                continue;
            }
        }

        if (command == "q") {
            break;
        } else if (command == "p") {
            if (engine.is_playing()) {
                engine.pause();
            } else {
                engine.play();
            }
        } else if (command == "s") {
            engine.stop();
        } else if (command == "r") {
            engine.seek(0);
        } else if (command == "l") {
            loop = !loop;
            engine.set_loop(loop);
            std::cout << "loop " << (loop ? "on" : "off") << "\n";
        } else if (command == "+" || command == "-") {
            master_gain = command == "+" ? master_gain * 1.25f : master_gain / 1.25f;
            if (master_gain > 2.0f) {
                master_gain = 2.0f;
            }
            engine.set_gain(master_gain);
            std::cout << "master gain " << master_gain << "\n";
        } else if (command == "j") {
            double seconds = 0.0;
            if (input >> seconds && seconds >= 0.0) {
                engine.seek(static_cast<std::int64_t>(seconds * rate));
            }
        } else if (command == "v" && track_ok) {
            float value = 1.0f;
            if (!(input >> value) || !engine.set_track_gain(track, value)) {
                std::cout << "gain must be between 0 and 8\n";
            }
        } else if (command == "n" && track_ok) {
            float value = 0.0f;
            if (!(input >> value) || !engine.set_track_pan(track, value)) {
                std::cout << "pan must be between -1 (left) and 1 (right)\n";
            }
        } else if (command == "m" && track_ok) {
            muted[track] = !muted[track];
            engine.set_track_mute(track, muted[track]);
            std::cout << "track " << (track + 1) << (muted[track] ? " muted\n" : " unmuted\n");
        } else if (command == "o" && track_ok) {
            soloed[track] = !soloed[track];
            engine.set_track_solo(track, soloed[track]);
            std::cout << "track " << (track + 1) << (soloed[track] ? " solo on\n" : " solo off\n");
        } else if (command == "i") {
            for (std::uint32_t t = 0; t < track_count; ++t) {
                std::cout << "track " << (t + 1) << " peak " << engine.track_peak(t) << "\n";
            }
            std::cout << "master peak " << engine.master_peak() << ", samples over full scale: "
                      << engine.master_clip_count() << "\n";
        } else if (!command.empty()) {
            print_help();
        }
        std::cout << "position " << static_cast<double>(engine.position_frames()) / rate
                  << " s of " << static_cast<double>(engine.length_frames()) / rate << " s\n";
    }

    device.stop();  // the callback no longer runs after this, so the engine can be destroyed
    return 0;
}
