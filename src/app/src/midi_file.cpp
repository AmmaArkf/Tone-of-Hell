#include "toh/app/midi_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

#include "toh/core/paged_file.h"
#include "toh/core/soundfont.h"

namespace toh::app {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

constexpr int kDrumChannel = 9;
constexpr int kTicksPerQuarter = 480;

struct Cursor {
    const std::vector<unsigned char>& bytes;
    std::size_t at;
    std::size_t end;

    bool done() const { return at >= end; }
    unsigned byte() { return at < end ? bytes[at++] : 0u; }
    std::uint32_t big(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 8) | byte();
        }
        return value;
    }
    // A variable-length quantity: 7 bits per byte, high bit set on all but the last.
    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned b = byte();
            value = (value << 7) | (b & 0x7Fu);
            if ((b & 0x80u) == 0) {
                break;
            }
        }
        return value;
    }
};

struct RawNote {
    std::uint64_t on = 0;
    std::uint64_t off = 0;
    int pitch = 60;
    int velocity = 100;
};

struct RawPart {
    std::string name;
    int channel = 0;
    int program = -1;
    std::vector<RawNote> notes;
};

}  // namespace

bool read_midi_file(const std::filesystem::path& path, std::uint32_t sample_rate, MidiSong& song,
                    std::string* error) {
    std::FILE* file = core::open_file(path, "rb");
    if (file == nullptr) {
        set_error(error, "cannot open " + path.u8string());
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char buffer[65536];
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        bytes.insert(bytes.end(), buffer, buffer + got);
        if (got < sizeof(buffer) || bytes.size() > (64u << 20)) {
            break;
        }
    }
    std::fclose(file);
    return parse_midi(bytes, sample_rate, song, error);
}

bool parse_midi(const std::vector<unsigned char>& bytes, std::uint32_t sample_rate, MidiSong& song,
                std::string* error) {
    song = MidiSong();
    Cursor head{bytes, 0, bytes.size()};
    if (bytes.size() < 14 || head.big(4) != 0x4D546864u /* MThd */) {
        set_error(error, "this is not a MIDI file");
        return false;
    }
    const std::uint32_t head_size = head.big(4);
    const unsigned format = head.big(2);
    const unsigned track_count = head.big(2);
    const unsigned division = head.big(2);
    if (format > 1 || (division & 0x8000u) != 0 || division == 0) {
        set_error(error, format > 1 ? "MIDI files of format 2 are not supported"
                                    : "MIDI files timed in SMPTE frames are not supported");
        return false;
    }
    std::size_t at = 8 + head_size;

    std::vector<std::pair<std::uint64_t, std::uint32_t>> tempos;  // tick, microseconds per quarter
    std::vector<RawPart> parts;
    bool time_signature_seen = false;
    for (unsigned track = 0; track < track_count && at + 8 <= bytes.size(); ++track) {
        Cursor chunk{bytes, at, bytes.size()};
        const std::uint32_t tag = chunk.big(4);
        const std::uint32_t size = chunk.big(4);
        const std::size_t end = std::min<std::size_t>(chunk.at + size, bytes.size());
        at = end;
        if (tag != 0x4D54726Bu /* MTrk */) {
            continue;
        }
        Cursor c{bytes, chunk.at, end};
        std::uint64_t tick = 0;
        unsigned status = 0;
        std::string track_name;
        std::map<int, std::size_t> part_of_channel;   // channel -> index in parts
        std::map<int, int> program_of_channel;
        std::map<int, std::vector<std::size_t>> open;  // channel * 128 + pitch -> notes started
        const auto part_for = [&](int channel) -> RawPart& {
            const auto found = part_of_channel.find(channel);
            if (found != part_of_channel.end()) {
                return parts[found->second];
            }
            RawPart part;
            part.channel = channel;
            const auto program = program_of_channel.find(channel);
            part.program = program != program_of_channel.end() ? program->second : -1;
            parts.push_back(part);
            part_of_channel[channel] = parts.size() - 1;
            return parts.back();
        };
        while (!c.done()) {
            tick += c.vlq();
            unsigned first = c.byte();
            if (first < 0x80u) {  // running status: the byte is already data
                --c.at;
                first = status;
            } else if (first < 0xF0u) {
                status = first;
            }
            if (first == 0xFFu) {  // meta event
                const unsigned type = c.byte();
                const std::uint32_t length = c.vlq();
                const std::size_t data = c.at;
                if (type == 0x51 && length >= 3) {
                    tempos.emplace_back(tick, c.big(3));
                } else if (type == 0x58 && length >= 2 && !time_signature_seen) {
                    const unsigned numerator = c.byte();
                    const unsigned power = c.byte();
                    if (numerator >= 1 && numerator <= 32 && power <= 5) {
                        song.beats_per_bar = static_cast<int>(numerator);
                        song.beat_unit = 1 << power;
                        time_signature_seen = true;
                    }
                } else if (type == 0x03 && track_name.empty()) {
                    track_name.assign(reinterpret_cast<const char*>(bytes.data() + data),
                                      std::min<std::size_t>(length, end - data));
                } else if (type == 0x2F) {
                    break;
                }
                c.at = data + length;
                continue;
            }
            if (first == 0xF0u || first == 0xF7u) {  // system exclusive: skipped
                c.at += c.vlq();
                continue;
            }
            const int channel = static_cast<int>(first & 0x0Fu);
            switch (first & 0xF0u) {
                case 0x80:
                case 0x90: {
                    const int pitch = static_cast<int>(c.byte() & 0x7Fu);
                    const int velocity = static_cast<int>(c.byte() & 0x7Fu);
                    const int key = channel * 128 + pitch;
                    RawPart& part = part_for(channel);
                    const std::size_t part_index = static_cast<std::size_t>(&part - parts.data());
                    if ((first & 0xF0u) == 0x90 && velocity > 0) {
                        RawNote note;
                        note.on = tick;
                        note.off = tick;
                        note.pitch = pitch;
                        note.velocity = velocity;
                        part.notes.push_back(note);
                        open[key].push_back(part_index * 1000000u + part.notes.size() - 1);
                    } else if (!open[key].empty()) {
                        const std::size_t code = open[key].front();  // the oldest note ends first
                        open[key].erase(open[key].begin());
                        parts[code / 1000000u].notes[code % 1000000u].off = tick;
                    }
                    break;
                }
                case 0xA0:
                case 0xB0:
                case 0xE0:
                    c.byte();
                    c.byte();
                    break;
                case 0xC0: {
                    const int program = static_cast<int>(c.byte() & 0x7Fu);
                    program_of_channel[channel] = program;
                    const auto found = part_of_channel.find(channel);
                    if (found != part_of_channel.end() && parts[found->second].program < 0) {
                        parts[found->second].program = program;
                    }
                    break;
                }
                case 0xD0:
                    c.byte();
                    break;
                default:
                    break;
            }
        }
        // Notes that never ended stop at the end of the track; parts are named after it.
        for (auto& entry : open) {
            for (const std::size_t code : entry.second) {
                parts[code / 1000000u].notes[code % 1000000u].off = tick;
            }
        }
        for (const auto& entry : part_of_channel) {
            parts[entry.second].name = track_name;
        }
    }

    // Ticks to frames, following the tempo changes.
    std::sort(tempos.begin(), tempos.end());
    if (tempos.empty() || tempos.front().first != 0) {
        tempos.insert(tempos.begin(), {0, 500000});  // 120 BPM until the first change
    }
    song.bpm = 60000000.0 / std::max<std::uint32_t>(tempos.front().second, 1);
    for (const auto& tempo : tempos) {
        if (tempo.first == 0) {
            song.bpm = 60000000.0 / std::max<std::uint32_t>(tempo.second, 1);
        }
    }
    const double per_tick_base = static_cast<double>(sample_rate) / 1e6 / static_cast<double>(division);
    const auto to_frames = [&](std::uint64_t tick) {
        double frames = 0.0;
        for (std::size_t k = 0; k < tempos.size(); ++k) {
            const std::uint64_t from = tempos[k].first;
            const std::uint64_t to = k + 1 < tempos.size() ? tempos[k + 1].first : tick;
            if (tick <= from) {
                break;
            }
            const std::uint64_t span = std::min(tick, to) - from;
            frames += static_cast<double>(span) * static_cast<double>(tempos[k].second) * per_tick_base;
        }
        return static_cast<std::int64_t>(std::llround(frames));
    };

    for (const RawPart& raw : parts) {
        if (raw.notes.empty()) {
            continue;
        }
        MidiPart part;
        part.name = raw.name;
        part.channel = raw.channel;
        part.program = raw.channel == kDrumChannel ? core::SoundFont::kDrums : std::max(raw.program, 0);
        for (const RawNote& r : raw.notes) {
            core::Note note;
            note.start = to_frames(r.on);
            const std::int64_t end = to_frames(r.off);
            note.length = std::max<std::int64_t>(end - note.start, sample_rate / 100);  // at least 10 ms
            note.pitch = static_cast<std::uint8_t>(r.pitch);
            note.velocity = static_cast<float>(r.velocity) / 127.0f;
            part.notes.push_back(note);
            song.frames = std::max(song.frames, note.start + note.length);
        }
        std::stable_sort(part.notes.begin(), part.notes.end(),
                         [](const core::Note& a, const core::Note& b) { return a.start < b.start; });
        song.parts.push_back(std::move(part));
    }
    if (song.parts.empty()) {
        set_error(error, "the MIDI file contains no notes");
        return false;
    }
    return true;
}

namespace {

void put_big(std::vector<unsigned char>& b, std::uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
        b.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
    }
}

void put_vlq(std::vector<unsigned char>& b, std::uint32_t value) {
    unsigned char bytes[5];
    int count = 0;
    do {
        bytes[count++] = static_cast<unsigned char>(value & 0x7Fu);
        value >>= 7;
    } while (value != 0 && count < 5);
    for (int i = count - 1; i >= 0; --i) {
        b.push_back(static_cast<unsigned char>(bytes[i] | (i > 0 ? 0x80u : 0u)));
    }
}

void put_track(std::vector<unsigned char>& file, const std::vector<unsigned char>& events) {
    put_big(file, 0x4D54726Bu, 4);
    put_big(file, static_cast<std::uint32_t>(events.size()), 4);
    file.insert(file.end(), events.begin(), events.end());
}

}  // namespace

bool write_midi_file(const std::filesystem::path& path, const MidiSong& song,
                     std::uint32_t sample_rate, std::string* error) {
    const double bpm = std::clamp(song.bpm, 20.0, 400.0);
    const double ticks_per_frame =
        static_cast<double>(kTicksPerQuarter) * bpm / 60.0 / static_cast<double>(sample_rate);
    const auto ticks = [&](std::int64_t frames) {
        return static_cast<std::uint32_t>(std::llround(static_cast<double>(std::max<std::int64_t>(frames, 0)) * ticks_per_frame));
    };

    std::vector<unsigned char> file;
    put_big(file, 0x4D546864u, 4);
    put_big(file, 6, 4);
    put_big(file, 1, 2);
    put_big(file, static_cast<std::uint32_t>(song.parts.size() + 1), 2);
    put_big(file, kTicksPerQuarter, 2);

    // Track 0: tempo and time signature.
    std::vector<unsigned char> head;
    put_vlq(head, 0);
    head.insert(head.end(), {0xFF, 0x51, 0x03});
    put_big(head, static_cast<std::uint32_t>(std::llround(60000000.0 / bpm)), 3);
    int power = 0;
    while ((1 << power) < song.beat_unit && power < 5) {
        ++power;
    }
    put_vlq(head, 0);
    head.insert(head.end(), {0xFF, 0x58, 0x04, static_cast<unsigned char>(std::clamp(song.beats_per_bar, 1, 32)),
                             static_cast<unsigned char>(power), 24, 8});
    put_vlq(head, 0);
    head.insert(head.end(), {0xFF, 0x2F, 0x00});
    put_track(file, head);

    for (const MidiPart& part : song.parts) {
        const auto channel = static_cast<unsigned char>(std::clamp(part.channel, 0, 15));
        std::vector<unsigned char> events;
        if (!part.name.empty()) {
            put_vlq(events, 0);
            events.insert(events.end(), {0xFF, 0x03});
            put_vlq(events, static_cast<std::uint32_t>(part.name.size()));
            events.insert(events.end(), part.name.begin(), part.name.end());
        }
        if (channel != kDrumChannel) {
            put_vlq(events, 0);
            events.push_back(static_cast<unsigned char>(0xC0u | channel));
            events.push_back(static_cast<unsigned char>(std::clamp(part.program, 0, 127)));
        }
        // Note-offs before note-ons at the same tick, so repeated notes stay separate.
        std::vector<std::pair<std::uint64_t, std::vector<unsigned char>>> timed;
        for (const core::Note& note : part.notes) {
            const std::uint64_t on = ticks(note.start);
            const std::uint64_t off = std::max<std::uint64_t>(ticks(note.start + note.length), on + 1);
            const auto velocity = static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(note.velocity * 127.0f)), 1, 127));
            timed.push_back({on * 2 + 1, {static_cast<unsigned char>(0x90u | channel), note.pitch, velocity}});
            timed.push_back({off * 2, {static_cast<unsigned char>(0x80u | channel), note.pitch, 0}});
        }
        std::stable_sort(timed.begin(), timed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::uint64_t now = 0;
        for (const auto& event : timed) {
            const std::uint64_t tick = event.first / 2;
            put_vlq(events, static_cast<std::uint32_t>(tick - now));
            now = tick;
            events.insert(events.end(), event.second.begin(), event.second.end());
        }
        put_vlq(events, 0);
        events.insert(events.end(), {0xFF, 0x2F, 0x00});
        put_track(file, events);
    }

    std::FILE* out = core::open_file(path, "wb");
    if (out == nullptr) {
        set_error(error, "cannot create " + path.u8string());
        return false;
    }
    const bool written = std::fwrite(file.data(), 1, file.size(), out) == file.size();
    if (std::fclose(out) != 0 || !written) {
        set_error(error, "cannot write " + path.u8string());
        return false;
    }
    return true;
}

}  // namespace toh::app
