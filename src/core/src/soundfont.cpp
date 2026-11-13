#include "toh/core/soundfont.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "toh/core/paged_file.h"

namespace toh::core {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

// Little-endian reading from a byte buffer, checked against its end.
struct Reader {
    const char* data;
    std::size_t size;

    std::uint32_t u32(std::size_t at) const {
        if (at + 4 > size) {
            return 0;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(data + at);
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }
    std::uint16_t u16(std::size_t at) const {
        if (at + 2 > size) {
            return 0;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(data + at);
        return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }
    std::int16_t s16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
    std::uint8_t u8(std::size_t at) const {
        return at < size ? static_cast<std::uint8_t>(data[at]) : 0;
    }
    bool tag(std::size_t at, const char* name) const {
        return at + 4 <= size && std::memcmp(data + at, name, 4) == 0;
    }
};

struct Chunk {
    std::size_t at = 0;  // first byte of the contents
    std::size_t size = 0;
};

// Finds the sub-chunks of a LIST chunk whose contents start at `begin`.
void find_chunks(const Reader& r, std::size_t begin, std::size_t end,
                 const std::vector<const char*>& names, std::vector<Chunk>& found) {
    found.assign(names.size(), Chunk());
    std::size_t at = begin;
    while (at + 8 <= end) {
        const std::size_t size = r.u32(at + 4);
        for (std::size_t k = 0; k < names.size(); ++k) {
            if (r.tag(at, names[k])) {
                found[k] = {at + 8, std::min(size, end - (at + 8))};
            }
        }
        at += 8 + size + (size & 1u);
    }
}

// Generator numbers of the SoundFont 2 format.
enum Gen : int {
    kStartOffset = 0, kEndOffset = 1, kLoopStartOffset = 2, kLoopEndOffset = 3,
    kStartCoarse = 4, kEndCoarse = 12, kPan = 17, kAttack = 34, kHold = 35, kDecay = 36,
    kSustain = 37, kRelease = 38, kInstrument = 41, kKeyRange = 43, kVelocityRange = 44,
    kLoopStartCoarse = 45, kAttenuation = 48, kLoopEndCoarse = 50, kCoarseTune = 51,
    kFineTune = 52, kSampleId = 53, kSampleModes = 54, kRootKey = 58, kGenCount = 61,
};

struct GenSet {
    std::array<std::int32_t, kGenCount> value{};
    std::array<bool, kGenCount> set{};
    void put(int gen, std::int32_t amount) {
        if (gen >= 0 && gen < kGenCount) {
            value[static_cast<std::size_t>(gen)] = amount;
            set[static_cast<std::size_t>(gen)] = true;
        }
    }
    bool has(int gen) const { return set[static_cast<std::size_t>(gen)]; }
    std::int32_t get(int gen, std::int32_t fallback) const {
        return has(gen) ? value[static_cast<std::size_t>(gen)] : fallback;
    }
};

// The generators of the zones in [first_bag, last_bag) of a bag list, each zone as a GenSet
// (the global zone, if any, merged into the others).
std::vector<GenSet> read_zones(const Reader& r, const Chunk& bags, const Chunk& gens,
                               std::size_t first_bag, std::size_t last_bag, int terminal) {
    std::vector<GenSet> zones;
    GenSet global;
    const std::size_t bag_count = bags.size / 4;
    const std::size_t gen_count = gens.size / 4;
    for (std::size_t bag = first_bag; bag < last_bag && bag + 1 < bag_count; ++bag) {
        const std::size_t gen_first = r.u16(bags.at + bag * 4);
        const std::size_t gen_last = r.u16(bags.at + (bag + 1) * 4);
        GenSet zone = global;
        bool terminal_found = false;
        for (std::size_t g = gen_first; g < gen_last && g < gen_count; ++g) {
            const int oper = r.u16(gens.at + g * 4);
            const std::size_t amount_at = gens.at + g * 4 + 2;
            std::int32_t amount = r.s16(amount_at);
            if (oper == kKeyRange || oper == kVelocityRange) {
                amount = r.u8(amount_at) | (r.u8(amount_at + 1) << 8);  // low and high byte
            } else if (oper == kInstrument || oper == kSampleId) {
                amount = r.u16(amount_at);
                terminal_found = terminal_found || oper == terminal;
            }
            zone.put(oper, amount);
        }
        if (terminal_found) {
            zones.push_back(zone);
        } else if (bag == first_bag) {
            global = zone;  // the global zone: defaults for the others
        }
    }
    return zones;
}

double timecents_to_seconds(std::int32_t timecents) {
    return std::pow(2.0, static_cast<double>(timecents) / 1200.0);
}

}  // namespace

std::shared_ptr<const SoundFont> SoundFont::load(const std::filesystem::path& path,
                                                 std::string* error) {
    std::FILE* file = open_file(path, "rb");
    if (file == nullptr) {
        set_error(error, "cannot open " + path.u8string());
        return nullptr;
    }
    std::vector<char> bytes;
    char buffer[1 << 16];
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        bytes.insert(bytes.end(), buffer, buffer + got);
        if (got < sizeof(buffer)) {
            break;
        }
    }
    std::fclose(file);
    auto loaded = parse(bytes, error);
    if (loaded) {
        std::const_pointer_cast<SoundFont>(loaded)->path_ = path;
    }
    return loaded;
}

std::shared_ptr<const SoundFont> SoundFont::parse(const std::vector<char>& bytes, std::string* error) {
    const Reader r{bytes.data(), bytes.size()};
    if (!r.tag(0, "RIFF") || !r.tag(8, "sfbk")) {
        set_error(error, "this is not a SoundFont 2 file");
        return nullptr;
    }
    // The LIST chunks: sdta holds the samples, pdta the presets, instruments and samples.
    Chunk sdta;
    Chunk pdta;
    for (std::size_t at = 12; at + 12 <= bytes.size();) {
        const std::size_t size = r.u32(at + 4);
        if (r.tag(at, "LIST") && r.tag(at + 8, "sdta")) {
            sdta = {at + 12, std::min(size - 4, bytes.size() - (at + 12))};
        } else if (r.tag(at, "LIST") && r.tag(at + 8, "pdta")) {
            pdta = {at + 12, std::min(size - 4, bytes.size() - (at + 12))};
        }
        at += 8 + size + (size & 1u);
    }
    std::vector<Chunk> s;
    find_chunks(r, sdta.at, sdta.at + sdta.size, {"smpl"}, s);
    std::vector<Chunk> p;
    find_chunks(r, pdta.at, pdta.at + pdta.size,
                {"phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr"}, p);
    const Chunk& phdr = p[0];
    const Chunk& pbag = p[1];
    const Chunk& pgen = p[2];
    const Chunk& inst = p[3];
    const Chunk& ibag = p[4];
    const Chunk& igen = p[5];
    const Chunk& shdr = p[6];
    if (s[0].size == 0 || phdr.size < 38 * 2 || inst.size < 22 * 2 || shdr.size < 46 * 2) {
        set_error(error, "the SoundFont has no samples or presets");
        return nullptr;
    }

    auto font = std::make_shared<SoundFont>();
    font->data_.resize(s[0].size / 2);
    std::memcpy(font->data_.data(), bytes.data() + s[0].at, font->data_.size() * 2);
    const auto sample_frames = static_cast<std::uint32_t>(font->data_.size());

    const std::size_t preset_count = phdr.size / 38 - 1;  // the last record ends the list
    const std::size_t inst_count = inst.size / 22 - 1;
    const std::size_t sample_count = shdr.size / 46 - 1;
    for (std::size_t pi = 0; pi < preset_count; ++pi) {
        const std::size_t at = phdr.at + pi * 38;
        Preset preset;
        preset.name.assign(bytes.data() + at, strnlen(bytes.data() + at, 20));
        preset.program = r.u16(at + 20);
        preset.bank = r.u16(at + 22);
        const std::vector<GenSet> preset_zones =
            read_zones(r, pbag, pgen, r.u16(at + 24), r.u16(at + 38 + 24), kInstrument);
        for (const GenSet& pz : preset_zones) {
            const std::size_t ii = static_cast<std::size_t>(pz.get(kInstrument, 0));
            if (ii >= inst_count) {
                continue;
            }
            const std::size_t inst_at = inst.at + ii * 22;
            const std::vector<GenSet> inst_zones =
                read_zones(r, ibag, igen, r.u16(inst_at + 20), r.u16(inst_at + 22 + 20), kSampleId);
            for (const GenSet& iz : inst_zones) {
                const std::size_t si = static_cast<std::size_t>(iz.get(kSampleId, 0));
                if (si >= sample_count) {
                    continue;
                }
                const std::size_t sh = shdr.at + si * 46;
                // Ranges intersect; the other values of the preset are added to the instrument's.
                const auto range = [&](int gen, std::uint8_t& low, std::uint8_t& high) {
                    const std::int32_t a = iz.get(gen, 127 << 8);
                    const std::int32_t b = pz.get(gen, 127 << 8);
                    low = static_cast<std::uint8_t>(std::max(a & 0xFF, b & 0xFF));
                    high = static_cast<std::uint8_t>(std::min((a >> 8) & 0xFF, (b >> 8) & 0xFF));
                };
                const auto sum = [&](int gen, std::int32_t fallback) {
                    return iz.get(gen, fallback) + pz.get(gen, 0);
                };
                Zone zone;
                range(kKeyRange, zone.key_low, zone.key_high);
                range(kVelocityRange, zone.velocity_low, zone.velocity_high);
                if (zone.key_low > zone.key_high || zone.velocity_low > zone.velocity_high) {
                    continue;
                }
                const std::int64_t start = static_cast<std::int64_t>(r.u32(sh + 20)) +
                                           iz.get(kStartOffset, 0) + 32768LL * iz.get(kStartCoarse, 0);
                const std::int64_t end = static_cast<std::int64_t>(r.u32(sh + 24)) +
                                         iz.get(kEndOffset, 0) + 32768LL * iz.get(kEndCoarse, 0);
                const std::int64_t loop_start = static_cast<std::int64_t>(r.u32(sh + 28)) +
                                                iz.get(kLoopStartOffset, 0) +
                                                32768LL * iz.get(kLoopStartCoarse, 0);
                const std::int64_t loop_end = static_cast<std::int64_t>(r.u32(sh + 32)) +
                                              iz.get(kLoopEndOffset, 0) +
                                              32768LL * iz.get(kLoopEndCoarse, 0);
                if (start < 0 || end <= start + 1 || end > sample_frames) {
                    continue;
                }
                zone.start = static_cast<std::uint32_t>(start);
                zone.end = static_cast<std::uint32_t>(end);
                const int modes = iz.get(kSampleModes, 0) & 3;
                zone.loop = (modes == 1 || modes == 3) && loop_start >= start && loop_end <= end &&
                            loop_end > loop_start + 1;
                zone.loop_start = static_cast<std::uint32_t>(std::max<std::int64_t>(loop_start, start));
                zone.loop_end = static_cast<std::uint32_t>(std::min<std::int64_t>(loop_end, end));
                zone.sample_rate = std::max<std::uint32_t>(r.u32(sh + 36), 1);
                const int original = r.u8(sh + 40);
                const int correction = static_cast<std::int8_t>(r.u8(sh + 41));
                const int root = iz.get(kRootKey, -1) >= 0 ? iz.get(kRootKey, -1) : original;
                const double tune = sum(kCoarseTune, 0) + (sum(kFineTune, 0) + correction) / 100.0;
                zone.root_key = static_cast<double>(root) - tune;
                // Attenuation in centibels, scaled by 0.4 like most SoundFont players do.
                const double attenuation = std::max(0.0, 0.4 * sum(kAttenuation, 0));
                zone.gain = static_cast<float>(std::pow(10.0, -attenuation / 200.0));
                const double pan = std::clamp(sum(kPan, 0) / 500.0, -1.0, 1.0);
                const double angle = (pan + 1.0) * 0.25 * 3.14159265358979;
                zone.left = static_cast<float>(std::cos(angle) * 1.41421356);
                zone.right = static_cast<float>(std::sin(angle) * 1.41421356);
                zone.attack = std::max(0.001, timecents_to_seconds(sum(kAttack, -12000)));
                zone.hold = std::min(timecents_to_seconds(sum(kHold, -12000)), 20.0);
                zone.decay = std::max(0.001, std::min(timecents_to_seconds(sum(kDecay, -12000)), 20.0));
                zone.sustain = std::pow(10.0, -std::clamp(sum(kSustain, 0), 0, 1440) / 200.0);
                zone.release = std::clamp(timecents_to_seconds(sum(kRelease, -12000)), 0.005, 4.0);
                preset.release = std::max(preset.release, zone.release);
                preset.zones.push_back(zone);
            }
        }
        if (preset.zones.empty()) {
            continue;
        }
        preset.by_key.resize(128);
        for (std::size_t z = 0; z < preset.zones.size() && z < 65535; ++z) {
            for (int key = preset.zones[z].key_low; key <= preset.zones[z].key_high && key < 128; ++key) {
                preset.by_key[static_cast<std::size_t>(key)].push_back(static_cast<std::uint16_t>(z));
            }
        }
        const int number = preset.bank == 128 ? kDrums + preset.program : preset.program;
        if (preset.bank == 0 || preset.bank == 128) {
            font->by_program_.emplace(number, font->presets_.size());
        }
        font->presets_.push_back(std::move(preset));
    }
    if (font->presets_.empty()) {
        set_error(error, "the SoundFont has no playable presets");
        return nullptr;
    }
    return font;
}

const SoundFont::Preset* SoundFont::find(int program) const noexcept {
    const auto it = by_program_.find(program);
    return it != by_program_.end() ? &presets_[it->second] : nullptr;
}

void SoundFont::add_note(const Preset& preset, const Note& note, double t_frames, double sample_rate,
                         float& left, float& right) const noexcept {
    if (t_frames < 0.0 || note.pitch >= preset.by_key.size()) {
        return;
    }
    const double t = t_frames / sample_rate;
    const double held = static_cast<double>(note.length) / sample_rate;
    const auto velocity = static_cast<int>(std::lround(note.velocity * 127.0f));
    for (const std::uint16_t index : preset.by_key[note.pitch]) {
        const Zone& zone = preset.zones[index];
        if (velocity < zone.velocity_low || velocity > zone.velocity_high) {
            continue;
        }
        // Loudness: attack, hold, decay to the sustain level, then the release.
        const auto held_level = [&zone](double x) {
            if (x < zone.attack) {
                return x / zone.attack;
            }
            x -= zone.attack;
            if (x < zone.hold) {
                return 1.0;
            }
            x -= zone.hold;
            // The decay falls by 100 dB over its time; it stops at the sustain level.
            const double fallen = std::pow(10.0, -5.0 * x / zone.decay);
            return std::max(zone.sustain, fallen);
        };
        double level = 0.0;
        if (t < held) {
            level = held_level(t);
        } else if (t - held < zone.release) {
            // Like the decay, the release falls by 100 dB over its time (as SoundFont 2 says),
            // so a long release fades quickly at first instead of ringing on as if the sustain
            // pedal were held.
            level = held_level(held) * std::pow(10.0, -5.0 * (t - held) / zone.release);
        }
        if (level <= 0.0) {
            continue;
        }
        // Where in the sample: it plays faster for higher keys.
        const double ratio = std::pow(2.0, (static_cast<double>(note.pitch) - zone.root_key) / 12.0) *
                             zone.sample_rate / sample_rate;
        double position = static_cast<double>(zone.start) + t_frames * ratio;
        if (zone.loop && position >= static_cast<double>(zone.loop_end)) {
            const double loop_length = static_cast<double>(zone.loop_end - zone.loop_start);
            position = static_cast<double>(zone.loop_start) +
                       std::fmod(position - static_cast<double>(zone.loop_start), loop_length);
        }
        const auto frame = static_cast<std::uint32_t>(position);
        if (frame + 1 >= zone.end) {
            continue;  // a sample without a loop has ended
        }
        const auto frac = static_cast<float>(position - static_cast<double>(frame));
        const float a = static_cast<float>(data_[frame]) / 32768.0f;
        const float b = static_cast<float>(data_[frame + 1]) / 32768.0f;
        const float value = (a + (b - a) * frac) * zone.gain * static_cast<float>(level) * note.velocity * 0.5f;
        left += value * zone.left;
        right += value * zone.right;
    }
}

}  // namespace toh::core
