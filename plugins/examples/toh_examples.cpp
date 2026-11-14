// The CLAP plugins that come with Tone of Hell: Echo, Tremolo, Gain, Simple Synth, two against
// noise (Noise Gate and Denoiser), and the mixing effects Equalizer, Compressor, Limiter and Reverb.
//
// They are small on purpose: they show what a CLAP effect needs (a factory, audio ports,
// parameters and a saved state), they give a new user a few effects to try, and the tests use
// them to check the plugin host. Like the rest of the project they are under the MIT license.

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct ParamDef {
    clap_id id;
    const char* name;
    double min;
    double max;
    double default_value;
    const char* unit;
    bool stepped = false;
};

const ParamDef kEchoParams[] = {{0, "Time", 10.0, 2000.0, 375.0, "ms"},
                                {1, "Feedback", 0.0, 95.0, 40.0, "%"},
                                {2, "Mix", 0.0, 100.0, 35.0, "%"}};
const ParamDef kTremoloParams[] = {{0, "Rate", 0.1, 20.0, 5.0, "Hz"},
                                   {1, "Depth", 0.0, 100.0, 60.0, "%"}};
const ParamDef kGainParams[] = {{0, "Gain", -24.0, 24.0, 0.0, "dB"},
                                {1, "Pan", -100.0, 100.0, 0.0, "%"}};
const ParamDef kSynthParams[] = {{0, "Attack", 1.0, 2000.0, 8.0, "ms"},
                                 {1, "Release", 10.0, 4000.0, 300.0, "ms"},
                                 {2, "Brightness", 0.0, 100.0, 35.0, "%"},
                                 {3, "Volume", -36.0, 6.0, -6.0, "dB"}};

const char* const kEchoFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kTremoloFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_TREMOLO,
                                        CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kGainFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const ParamDef kEqParams[] = {{0, "Low gain", -18.0, 18.0, 0.0, "dB"},
                              {1, "Low frequency", 40.0, 600.0, 120.0, "Hz"},
                              {2, "Mid gain", -18.0, 18.0, 0.0, "dB"},
                              {3, "Mid frequency", 200.0, 8000.0, 1000.0, "Hz"},
                              {4, "Mid width", 0.3, 6.0, 1.0, "Q"},
                              {5, "High gain", -18.0, 18.0, 0.0, "dB"},
                              {6, "High frequency", 1500.0, 16000.0, 8000.0, "Hz"}};
const char* const kEqFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const ParamDef kCompParams[] = {{0, "Threshold", -60.0, 0.0, -18.0, "dB"},
                                {1, "Ratio", 1.0, 20.0, 4.0, ": 1"},
                                {2, "Attack", 0.1, 200.0, 10.0, "ms"},
                                {3, "Release", 10.0, 2000.0, 150.0, "ms"},
                                {4, "Make-up gain", 0.0, 24.0, 0.0, "dB"}};
const char* const kCompFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const ParamDef kLimitParams[] = {{0, "Ceiling", -12.0, 0.0, -1.0, "dB"},
                                 {1, "Release", 10.0, 1000.0, 100.0, "ms"},
                                 {2, "Input gain", 0.0, 24.0, 0.0, "dB"}};
const char* const kLimitFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_LIMITER,
                                      CLAP_PLUGIN_FEATURE_MASTERING, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const ParamDef kGateParams[] = {{0, "Threshold", -80.0, 0.0, -45.0, "dB"},
                                {1, "Attack", 0.1, 100.0, 2.0, "ms"},
                                {2, "Release", 5.0, 2000.0, 150.0, "ms"},
                                {3, "Depth", 0.0, 80.0, 60.0, "dB"}};
const ParamDef kDenoiseParams[] = {{0, "Learn noise", 0.0, 1.0, 0.0, "", true},
                                   {1, "Reduction", 0.0, 40.0, 18.0, "dB"},
                                   {2, "Sensitivity", 0.5, 4.0, 1.5, "x"},
                                   {3, "Smoothing", 0.0, 100.0, 60.0, "%"}};
const char* const kGateFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_GATE,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kDenoiseFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_RESTORATION,
                                        CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kSynthFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kEcho = {
    CLAP_VERSION_INIT, "org.toneofhell.echo", "Echo", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "The sound comes back again and again, quieter every time.", kEchoFeatures};
const clap_plugin_descriptor_t kTremolo = {
    CLAP_VERSION_INIT, "org.toneofhell.tremolo", "Tremolo", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "The volume swells and fades in a steady rhythm.", kTremoloFeatures};
const clap_plugin_descriptor_t kGain = {
    CLAP_VERSION_INIT, "org.toneofhell.gain", "Gain", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "Louder or quieter, and to the left or right.", kGainFeatures};
const clap_plugin_descriptor_t kSynth = {
    CLAP_VERSION_INIT, "org.toneofhell.synth", "Simple Synth", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "A small synthesizer: a soft sine that turns brighter towards a saw wave.", kSynthFeatures};
const clap_plugin_descriptor_t kGate = {
    CLAP_VERSION_INIT, "org.toneofhell.gate", "Noise Gate", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "Turns the sound down while it is quieter than the threshold: no noise in the pauses.", kGateFeatures};
const clap_plugin_descriptor_t kDenoise = {
    CLAP_VERSION_INIT, "org.toneofhell.denoiser", "Denoiser", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "Learns a steady noise (hiss, hum, a fan) from a part with only noise, then takes it out of the sound.",
    kDenoiseFeatures};

const clap_plugin_descriptor_t kEq = {
    CLAP_VERSION_INIT, "org.toneofhell.equalizer", "Equalizer", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "Three bands: more or less of the low, middle and high frequencies.", kEqFeatures};

const clap_plugin_descriptor_t kComp = {
    CLAP_VERSION_INIT, "org.toneofhell.compressor", "Compressor", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "Turns the loud parts down, so the sound gets more even.", kCompFeatures};

const clap_plugin_descriptor_t kLimit = {
    CLAP_VERSION_INIT, "org.toneofhell.limiter", "Limiter", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "Never lets the sound get louder than the ceiling: it looks ahead and turns peaks down in time.",
    kLimitFeatures};

enum class Kind { Echo, Tremolo, Gain, Synth, Gate, Denoise, Equalizer, Compressor, Limiter };

// The limiter looks 5 ms ahead: the sound is delayed that long, so the gain is down before a
// peak arrives.
struct Limiter {
    std::size_t length = 240;      // the look-ahead in frames
    std::vector<float> delay;      // the delayed sound, stereo
    std::vector<float> wanted;     // per frame: the gain that keeps it under the ceiling
    std::vector<float> lowest;     // per frame: the lowest wanted gain of the window before it
    double sum = 0.0;              // of `lowest`
    std::size_t position = 0;
    float recover = 1.0f;          // the wanted gain, coming back up slowly after a peak

    void prepare(double rate) {
        length = std::max<std::size_t>(static_cast<std::size_t>(std::llround(0.005 * rate)), 2);
        delay.assign(length * 2, 0.0f);
        clear();
    }
    void clear() {
        std::fill(delay.begin(), delay.end(), 0.0f);
        wanted.assign(length, 1.0f);
        lowest.assign(length, 1.0f);
        sum = static_cast<double>(length);
        position = 0;
        recover = 1.0f;
    }
};

// A second-order filter (the "Audio EQ Cookbook" formulas of Robert Bristow-Johnson).
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] = {0.0, 0.0};
    double z2[2] = {0.0, 0.0};

    float run(float input, std::size_t channel) noexcept {
        const double x = static_cast<double>(input);
        const double y = b0 * x + z1[channel];
        z1[channel] = b1 * x - a1 * y + z2[channel];
        z2[channel] = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
    void clear() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    void set(double n0, double n1, double n2, double d0, double d1, double d2) noexcept {
        b0 = n0 / d0;
        b1 = n1 / d0;
        b2 = n2 / d0;
        a1 = d1 / d0;
        a2 = d2 / d0;
    }
    // A bell around `hz`, `q` wide.
    void peak(double rate, double hz, double db, double q) noexcept {
        const double a = std::pow(10.0, db / 40.0);
        const double w = 2.0 * kPi * std::min(hz, 0.45 * rate) / rate;
        const double alpha = std::sin(w) / (2.0 * q);
        set(1.0 + alpha * a, -2.0 * std::cos(w), 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * std::cos(w),
            1.0 - alpha / a);
    }
    // Everything below (`high` false) or above (`high` true) `hz` louder or quieter.
    void shelf(double rate, double hz, double db, bool high) noexcept {
        const double a = std::pow(10.0, db / 40.0);
        const double w = 2.0 * kPi * std::min(hz, 0.45 * rate) / rate;
        const double c = std::cos(w);
        const double beta = 2.0 * std::sqrt(a) * std::sin(w) / 2.0 * std::sqrt(2.0);
        if (high) {
            set(a * ((a + 1.0) + (a - 1.0) * c + beta), -2.0 * a * ((a - 1.0) + (a + 1.0) * c),
                a * ((a + 1.0) + (a - 1.0) * c - beta), (a + 1.0) - (a - 1.0) * c + beta,
                2.0 * ((a - 1.0) - (a + 1.0) * c), (a + 1.0) - (a - 1.0) * c - beta);
        } else {
            set(a * ((a + 1.0) - (a - 1.0) * c + beta), 2.0 * a * ((a - 1.0) - (a + 1.0) * c),
                a * ((a + 1.0) - (a - 1.0) * c - beta), (a + 1.0) + (a - 1.0) * c + beta,
                -2.0 * ((a - 1.0) + (a + 1.0) * c), (a + 1.0) + (a - 1.0) * c - beta);
        }
    }
};

// The denoiser works on overlapping pieces of kFft frames, a quarter apart.
constexpr std::size_t kFft = 1024;
constexpr std::size_t kHop = kFft / 4;
constexpr std::size_t kBins = kFft / 2 + 1;
constexpr std::size_t kFftLatency = kFft - kHop;

// An in-place radix-2 FFT (the inverse is not scaled).
void fft(std::complex<float>* a, std::size_t n, bool inverse) {
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const std::complex<float> step(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

struct DenoiseChannel {
    std::array<float, kFft> in{};
    std::array<float, kFft> out{};
    std::array<float, kFft> sum{};       // overlapping pieces added up
    std::array<float, kBins> profile{};  // the learned noise: its level at every frequency
    std::array<double, kBins> learned{};
    std::array<float, kBins> gain{};
};

struct Denoise {
    std::array<DenoiseChannel, 2> channels{};
    std::array<float, kFft> window{};
    std::array<std::complex<float>, kFft> work{};
    std::size_t rover = kFftLatency;
    bool learning = false;
    std::uint32_t learned_frames = 0;
    std::array<float, 2 * kBins> pending{};  // a profile from a loaded state
    std::atomic<bool> has_pending{false};
};

// One playing note of the synthesizer.
struct Voice {
    bool active = false;
    bool releasing = false;
    int key = -1;
    double phase = 0.0;  // 0 to 2 pi
    double step = 0.0;   // phase per frame
    double level = 0.0;  // the envelope, 0 to 1
    float velocity = 0.0f;
};

struct Example {
    clap_plugin_t plugin{};
    Kind kind = Kind::Gain;
    const ParamDef* defs = nullptr;
    std::uint32_t count = 0;
    std::array<std::atomic<double>, 8> values{};
    double rate = 48000.0;
    std::vector<float> delay;  // stereo, interleaved
    std::size_t delay_frames = 0;
    std::size_t write = 0;
    double phase = 0.0;
    std::array<Voice, 16> voices{};
    float gate_level = 0.0f;
    float gate_gain = 1.0f;
    bool gate_open = true;
    Denoise* denoise = nullptr;  // only for the denoiser
    std::array<Biquad, 3> bands{};  // the equalizer: low, middle, high
    Limiter* limiter = nullptr;     // only for the limiter
    float comp_level = 0.0f;        // the compressor: the level it follows, and
    float comp_reduction = 0.0f;    // how far it turns down now, in dB (0 or less)
};

// One piece of the denoiser: to the spectrum, every frequency turned down by how much noise
// it holds, and back.
void denoise_frame(Example& e, DenoiseChannel& ch) {
    Denoise& d = *e.denoise;
    for (std::size_t k = 0; k < kFft; ++k) {
        d.work[k] = std::complex<float>(ch.in[k] * d.window[k], 0.0f);
    }
    fft(d.work.data(), kFft, false);
    const auto floor_gain = static_cast<float>(std::pow(10.0, -e.values[1].load() / 20.0));
    const auto sensitivity = static_cast<float>(e.values[2].load());
    const auto hold = static_cast<float>(e.values[3].load() / 100.0 * 0.9);
    for (std::size_t k = 0; k < kBins; ++k) {
        const float level = std::abs(d.work[k]);
        float gain = 1.0f;
        if (d.learning) {
            ch.learned[k] += static_cast<double>(level);
            ch.profile[k] = static_cast<float>(ch.learned[k] / static_cast<double>(d.learned_frames + 1));
        } else if (ch.profile[k] > 0.0f) {
            gain = level > 1e-9f ? std::max((level - sensitivity * ch.profile[k]) / level, floor_gain) : floor_gain;
            if (gain < ch.gain[k]) {
                gain = hold * ch.gain[k] + (1.0f - hold) * gain;  // closing slowly: less "musical noise"
            }
        }
        ch.gain[k] = gain;
        d.work[k] *= gain;
        if (k > 0 && k < kFft / 2) {
            d.work[kFft - k] *= gain;
        }
    }
    fft(d.work.data(), kFft, true);
    const float scale = 0.5f / static_cast<float>(kFft);  // the windows overlap to 2
    for (std::size_t k = 0; k < kFft; ++k) {
        ch.sum[k] += d.work[k].real() * d.window[k] * scale;
    }
    std::copy(ch.sum.begin(), ch.sum.begin() + kHop, ch.out.begin());
    std::copy(ch.sum.begin() + kHop, ch.sum.end(), ch.sum.begin());
    std::fill(ch.sum.end() - kHop, ch.sum.end(), 0.0f);
    std::copy(ch.in.begin() + kHop, ch.in.end(), ch.in.begin());
}

Example& self(const clap_plugin_t* plugin) { return *static_cast<Example*>(plugin->plugin_data); }

void apply_events(Example& e, const clap_input_events_t* events) {
    if (events == nullptr) {
        return;
    }
    const std::uint32_t count = events->size(events);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_event_header_t* header = events->get(events, i);
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID ||
            header->type != CLAP_EVENT_PARAM_VALUE) {
            continue;
        }
        const auto* event = reinterpret_cast<const clap_event_param_value_t*>(header);
        if (event->param_id < e.count) {
            const ParamDef& def = e.defs[event->param_id];
            e.values[event->param_id].store(std::clamp(event->value, def.min, def.max));
        }
    }
}

// ---- The plugin ----------------------------------------------------------------------------

bool plugin_init(const clap_plugin_t*) { return true; }

void plugin_destroy(const clap_plugin_t* plugin) {
    delete self(plugin).denoise;
    delete self(plugin).limiter;
    delete &self(plugin);
}

bool plugin_activate(const clap_plugin_t* plugin, double rate, std::uint32_t, std::uint32_t) {
    Example& e = self(plugin);
    e.rate = rate;
    if (e.limiter != nullptr) {
        e.limiter->prepare(rate);
    }
    if (e.kind == Kind::Echo) {
        e.delay_frames = static_cast<std::size_t>(rate * 2.1) + 1;  // a little over 2 seconds
        e.delay.assign(e.delay_frames * 2, 0.0f);
        e.write = 0;
    }
    e.phase = 0.0;
    return true;
}

void plugin_deactivate(const clap_plugin_t*) {}

bool plugin_start(const clap_plugin_t*) { return true; }

void plugin_stop(const clap_plugin_t*) {}

void plugin_reset(const clap_plugin_t* plugin) {
    Example& e = self(plugin);
    std::fill(e.delay.begin(), e.delay.end(), 0.0f);
    e.phase = 0.0;
    for (Voice& voice : e.voices) {
        voice = Voice{};
    }
    for (Biquad& band : e.bands) {
        band.clear();
    }
    e.comp_level = 0.0f;
    e.comp_reduction = 0.0f;
    if (e.limiter != nullptr) {
        e.limiter->clear();
    }
}

// A note starts or ends, or a parameter changes, for the synthesizer.
void synth_event(Example& e, const clap_event_header_t* header) {
    if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
        return;
    }
    if (header->type == CLAP_EVENT_NOTE_ON) {
        const auto* note = reinterpret_cast<const clap_event_note_t*>(header);
        if (note->key < 0 || note->key > 127) {
            return;
        }
        Voice* target = nullptr;
        for (Voice& voice : e.voices) {
            if (!voice.active) {
                target = &voice;
                break;
            }
        }
        if (target == nullptr) {  // all busy: one that is fading out, or the first
            target = &e.voices[0];
            for (Voice& voice : e.voices) {
                if (voice.releasing) {
                    target = &voice;
                    break;
                }
            }
        }
        *target = Voice{};
        target->active = true;
        target->key = note->key;
        target->velocity = static_cast<float>(std::clamp(note->velocity, 0.0, 1.0));
        target->step = 2.0 * kPi * 440.0 * std::pow(2.0, (note->key - 69) / 12.0) / e.rate;
    } else if (header->type == CLAP_EVENT_NOTE_OFF || header->type == CLAP_EVENT_NOTE_CHOKE) {
        const auto* note = reinterpret_cast<const clap_event_note_t*>(header);
        for (Voice& voice : e.voices) {
            if (voice.active && (note->key < 0 || voice.key == note->key)) {
                voice.releasing = true;
            }
        }
    } else if (header->type == CLAP_EVENT_PARAM_VALUE) {
        const auto* event = reinterpret_cast<const clap_event_param_value_t*>(header);
        if (event->param_id < e.count) {
            const ParamDef& def = e.defs[event->param_id];
            e.values[event->param_id].store(std::clamp(event->value, def.min, def.max));
        }
    }
}

clap_process_status synth_process(Example& e, const clap_process_t* process) {
    if (process->audio_outputs_count < 1) {
        return CLAP_PROCESS_ERROR;
    }
    const clap_audio_buffer_t& out = process->audio_outputs[0];
    if (out.channel_count == 0 || out.data32 == nullptr) {
        return CLAP_PROCESS_ERROR;
    }
    const clap_input_events_t* events = process->in_events;
    const std::uint32_t event_count = events != nullptr ? events->size(events) : 0;
    std::uint32_t next = 0;
    const double attack = std::max(e.values[0].load() / 1000.0 * e.rate, 1.0);
    const double release = std::max(e.values[1].load() / 1000.0 * e.rate, 1.0);
    const double bright = e.values[2].load() / 100.0;
    const double volume = 0.3 * std::pow(10.0, e.values[3].load() / 20.0);
    for (std::uint32_t i = 0; i < process->frames_count; ++i) {
        while (next < event_count) {
            const clap_event_header_t* header = events->get(events, next);
            if (header != nullptr && header->time > i) {
                break;
            }
            synth_event(e, header);
            ++next;
        }
        double sum = 0.0;
        for (Voice& voice : e.voices) {
            if (!voice.active) {
                continue;
            }
            if (voice.releasing) {
                voice.level -= 1.0 / release;
                if (voice.level <= 0.0) {
                    voice.active = false;
                    continue;
                }
            } else {
                voice.level = std::min(1.0, voice.level + 1.0 / attack);
            }
            const double sine = std::sin(voice.phase);
            const double saw = 1.0 - voice.phase / kPi;
            sum += ((1.0 - bright) * sine + bright * saw) * voice.level * voice.velocity;
            voice.phase += voice.step;
            if (voice.phase >= 2.0 * kPi) {
                voice.phase -= 2.0 * kPi;
            }
        }
        const auto sample = static_cast<float>(sum * volume);
        for (std::uint32_t c = 0; c < out.channel_count; ++c) {
            out.data32[c][i] = sample;
        }
    }
    for (; next < event_count; ++next) {
        synth_event(e, events->get(events, next));
    }
    return CLAP_PROCESS_CONTINUE;
}

clap_process_status plugin_process(const clap_plugin_t* plugin, const clap_process_t* process) {
    Example& e = self(plugin);
    if (e.kind == Kind::Synth) {
        return synth_process(e, process);
    }
    apply_events(e, process->in_events);
    if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1) {
        return CLAP_PROCESS_ERROR;
    }
    const clap_audio_buffer_t& in = process->audio_inputs[0];
    const clap_audio_buffer_t& out = process->audio_outputs[0];
    if (in.channel_count == 0 || out.channel_count == 0 || in.data32 == nullptr || out.data32 == nullptr) {
        return CLAP_PROCESS_ERROR;
    }
    const auto input = [&in](std::uint32_t channel, std::uint32_t frame) {
        return in.data32[std::min(channel, in.channel_count - 1)][frame];
    };
    const auto output = [&out](std::uint32_t frame, float left, float right) {
        for (std::uint32_t c = 0; c < out.channel_count; ++c) {
            out.data32[c][frame] = c == 0 ? left : (c == 1 ? right : 0.5f * (left + right));
        }
    };

    const std::uint32_t frames = process->frames_count;
    switch (e.kind) {
        case Kind::Echo: {
            const double seconds = e.values[0].load() / 1000.0;
            const auto distance = std::clamp<std::size_t>(static_cast<std::size_t>(seconds * e.rate), 1,
                                                          e.delay_frames - 1);
            const auto feedback = static_cast<float>(e.values[1].load() / 100.0);
            const auto mix = static_cast<float>(e.values[2].load() / 100.0);
            for (std::uint32_t i = 0; i < frames; ++i) {
                const std::size_t read = (e.write + e.delay_frames - distance) % e.delay_frames;
                float result[2];
                for (std::uint32_t c = 0; c < 2; ++c) {
                    const float dry = input(c, i);
                    const float echo = e.delay[read * 2 + c];
                    e.delay[e.write * 2 + c] = dry + echo * feedback;
                    result[c] = dry + echo * mix;
                }
                output(i, result[0], result[1]);
                e.write = (e.write + 1) % e.delay_frames;
            }
            break;
        }
        case Kind::Tremolo: {
            const double step = 2.0 * kPi * e.values[0].load() / e.rate;
            const double depth = e.values[1].load() / 100.0;
            for (std::uint32_t i = 0; i < frames; ++i) {
                const auto gain = static_cast<float>(1.0 - depth * (0.5 - 0.5 * std::cos(e.phase)));
                e.phase = std::fmod(e.phase + step, 2.0 * kPi);
                output(i, input(0, i) * gain, input(1, i) * gain);
            }
            break;
        }
        case Kind::Synth:
            break;
        case Kind::Equalizer: {
            e.bands[0].shelf(e.rate, e.values[1].load(), e.values[0].load(), false);
            e.bands[1].peak(e.rate, e.values[3].load(), e.values[2].load(), e.values[4].load());
            e.bands[2].shelf(e.rate, e.values[6].load(), e.values[5].load(), true);
            for (std::uint32_t i = 0; i < frames; ++i) {
                float result[2];
                for (std::uint32_t c = 0; c < 2; ++c) {
                    float value = input(c, i);
                    for (Biquad& band : e.bands) {
                        value = band.run(value, c);
                    }
                    result[c] = value;
                }
                output(i, result[0], result[1]);
            }
            break;
        }
        case Kind::Compressor: {
            const auto threshold = static_cast<float>(e.values[0].load());
            const auto slope = static_cast<float>(1.0 / e.values[1].load() - 1.0);  // dB of reduction per dB over
            const auto attack = static_cast<float>(std::exp(-1.0 / (e.values[2].load() / 1000.0 * e.rate)));
            const auto release = static_cast<float>(std::exp(-1.0 / (e.values[3].load() / 1000.0 * e.rate)));
            const auto makeup = static_cast<float>(e.values[4].load());
            const auto fall = static_cast<float>(std::exp(-1.0 / (0.010 * e.rate)));
            for (std::uint32_t i = 0; i < frames; ++i) {
                const float left = input(0, i);
                const float right = input(1, i);
                // Both sides are turned down together, so the stereo picture stays.
                const float level = std::max(std::fabs(left), std::fabs(right));
                e.comp_level = level > e.comp_level ? level : e.comp_level * fall;
                const float db = 20.0f * std::log10(e.comp_level + 1e-9f);
                const float wanted = db > threshold ? (db - threshold) * slope : 0.0f;
                const float coefficient = wanted < e.comp_reduction ? attack : release;
                e.comp_reduction = wanted + (e.comp_reduction - wanted) * coefficient;
                const float gain = std::pow(10.0f, (e.comp_reduction + makeup) / 20.0f);
                output(i, left * gain, right * gain);
            }
            break;
        }
        case Kind::Limiter: {
            Limiter& l = *e.limiter;
            const auto ceiling = static_cast<float>(std::pow(10.0, e.values[0].load() / 20.0));
            const auto rise = static_cast<float>(1.0 - std::exp(-1.0 / (e.values[1].load() / 1000.0 * e.rate)));
            const auto drive = static_cast<float>(std::pow(10.0, e.values[2].load() / 20.0));
            const std::size_t n = l.length;
            for (std::uint32_t i = 0; i < frames; ++i) {
                const float left = input(0, i) * drive;
                const float right = input(1, i) * drive;
                const float peak = std::max(std::fabs(left), std::fabs(right));
                // The gain this frame needs; after a peak it comes back up slowly.
                const float needed = peak > ceiling ? ceiling / peak : 1.0f;
                l.recover = std::min(needed, l.recover + (1.0f - l.recover) * rise);
                l.wanted[l.position] = l.recover;
                // The lowest of the last n frames, averaged over n frames: a smooth gain that is
                // low enough exactly when the delayed peak comes out.
                float low = 1.0f;
                for (std::size_t k = 0; k < n; ++k) {
                    low = std::min(low, l.wanted[k]);
                }
                l.sum += static_cast<double>(low) - static_cast<double>(l.lowest[l.position]);
                l.lowest[l.position] = low;
                const auto gain = static_cast<float>(l.sum / static_cast<double>(n));
                l.delay[l.position * 2] = left;
                l.delay[l.position * 2 + 1] = right;
                l.position = (l.position + 1) % n;
                if (l.position == 0) {  // once around: add up again, so rounding does not build up
                    l.sum = 0.0;
                    for (std::size_t k = 0; k < n; ++k) {
                        l.sum += static_cast<double>(l.lowest[k]);
                    }
                }
                // The oldest frame in the delay line is n - 1 frames old.
                output(i, std::clamp(l.delay[l.position * 2] * gain, -ceiling, ceiling),
                       std::clamp(l.delay[l.position * 2 + 1] * gain, -ceiling, ceiling));
            }
            break;
        }
        case Kind::Gate: {
            const auto threshold = static_cast<float>(std::pow(10.0, e.values[0].load() / 20.0));
            const auto attack = static_cast<float>(std::exp(-1.0 / (e.values[1].load() / 1000.0 * e.rate)));
            const auto release = static_cast<float>(std::exp(-1.0 / (e.values[2].load() / 1000.0 * e.rate)));
            const auto closed = static_cast<float>(std::pow(10.0, -e.values[3].load() / 20.0));
            const auto fall = static_cast<float>(std::exp(-1.0 / (0.005 * e.rate)));
            for (std::uint32_t i = 0; i < frames; ++i) {
                const float left = input(0, i);
                const float right = input(1, i);
                const float level = std::max(std::fabs(left), std::fabs(right));
                e.gate_level = level > e.gate_level ? level : e.gate_level * fall;
                // It opens at the threshold and closes a little below it, so it does not flutter.
                if (e.gate_level > threshold) {
                    e.gate_open = true;
                } else if (e.gate_level < threshold * 0.7f) {
                    e.gate_open = false;
                }
                const float target = e.gate_open ? 1.0f : closed;
                const float coefficient = target > e.gate_gain ? attack : release;
                e.gate_gain = target + (e.gate_gain - target) * coefficient;
                output(i, left * e.gate_gain, right * e.gate_gain);
            }
            break;
        }
        case Kind::Denoise: {
            Denoise& d = *e.denoise;
            if (d.has_pending.exchange(false)) {  // a profile that came with a loaded state
                for (std::size_t c = 0; c < 2; ++c) {
                    std::copy(d.pending.begin() + static_cast<std::ptrdiff_t>(c * kBins),
                              d.pending.begin() + static_cast<std::ptrdiff_t>((c + 1) * kBins),
                              d.channels[c].profile.begin());
                }
            }
            const bool learn = e.values[0].load() >= 0.5;
            if (learn && !d.learning) {  // learning begins: forget the old noise
                for (DenoiseChannel& ch : d.channels) {
                    ch.learned.fill(0.0);
                }
                d.learned_frames = 0;
            }
            d.learning = learn;
            for (std::uint32_t i = 0; i < frames; ++i) {
                float result[2];
                for (std::uint32_t c = 0; c < 2; ++c) {
                    d.channels[c].in[d.rover] = input(c, i);
                    result[c] = d.channels[c].out[d.rover - kFftLatency];
                }
                output(i, result[0], result[1]);
                if (++d.rover >= kFft) {
                    d.rover = kFftLatency;
                    denoise_frame(e, d.channels[0]);
                    denoise_frame(e, d.channels[1]);
                    if (d.learning) {
                        ++d.learned_frames;
                    }
                }
            }
            break;
        }
        case Kind::Gain: {
            const double gain = std::pow(10.0, e.values[0].load() / 20.0);
            const double pan = e.values[1].load() / 100.0;
            const auto left = static_cast<float>(gain * std::min(1.0, 1.0 - pan));
            const auto right = static_cast<float>(gain * std::min(1.0, 1.0 + pan));
            for (std::uint32_t i = 0; i < frames; ++i) {
                output(i, input(0, i) * left, input(1, i) * right);
            }
            break;
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

// ---- Audio ports: one stereo input (not for the synthesizer) and one stereo output -------------

std::uint32_t ports_count(const clap_plugin_t* plugin, bool is_input) {
    return is_input && self(plugin).kind == Kind::Synth ? 0 : 1;
}

bool ports_get(const clap_plugin_t* plugin, std::uint32_t index, bool is_input, clap_audio_port_info_t* info) {
    if (index >= ports_count(plugin, is_input)) {
        return false;
    }
    *info = clap_audio_port_info_t{};
    info->id = is_input ? 0u : 1u;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kPorts = {ports_count, ports_get};

// ---- Note ports: the synthesizer takes notes ------------------------------------------------------

std::uint32_t note_ports_count(const clap_plugin_t*, bool is_input) { return is_input ? 1 : 0; }

bool note_ports_get(const clap_plugin_t*, std::uint32_t index, bool is_input, clap_note_port_info_t* info) {
    if (!is_input || index != 0) {
        return false;
    }
    *info = clap_note_port_info_t{};
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "%s", "Notes");
    return true;
}

const clap_plugin_note_ports_t kNotePorts = {note_ports_count, note_ports_get};

// ---- Parameters ----------------------------------------------------------------------------------

std::uint32_t params_count(const clap_plugin_t* plugin) { return self(plugin).count; }

bool params_info(const clap_plugin_t* plugin, std::uint32_t index, clap_param_info_t* info) {
    const Example& e = self(plugin);
    if (index >= e.count) {
        return false;
    }
    const ParamDef& def = e.defs[index];
    *info = clap_param_info_t{};
    info->id = def.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (def.stepped) {
        info->flags |= CLAP_PARAM_IS_STEPPED;
    }
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%s", def.name);
    info->module[0] = '\0';
    info->min_value = def.min;
    info->max_value = def.max;
    info->default_value = def.default_value;
    return true;
}

bool params_value(const clap_plugin_t* plugin, clap_id id, double* value) {
    const Example& e = self(plugin);
    if (id >= e.count) {
        return false;
    }
    *value = e.values[id].load();
    return true;
}

bool params_to_text(const clap_plugin_t* plugin, clap_id id, double value, char* text, std::uint32_t size) {
    const Example& e = self(plugin);
    if (id >= e.count) {
        return false;
    }
    std::snprintf(text, size, value >= 100.0 || value <= -100.0 ? "%.0f %s" : "%.1f %s", value, e.defs[id].unit);
    return true;
}

bool params_from_text(const clap_plugin_t* plugin, clap_id id, const char* text, double* value) {
    if (id >= self(plugin).count) {
        return false;
    }
    *value = std::strtod(text, nullptr);
    return true;
}

void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t*) {
    apply_events(self(plugin), in);
}

const clap_plugin_params_t kParams = {params_count, params_info, params_value,
                                      params_to_text, params_from_text, params_flush};

// ---- State: the parameter values -----------------------------------------------------------------

bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    const Example& e = self(plugin);
    std::array<double, 8> data{};
    for (std::uint32_t i = 0; i < e.count; ++i) {
        data[i] = e.values[i].load();
    }
    const auto* bytes = reinterpret_cast<const char*>(data.data());
    std::uint64_t left = sizeof(double) * e.count;
    while (left > 0) {
        const std::int64_t written = stream->write(stream, bytes, left);
        if (written <= 0) {
            return false;
        }
        bytes += written;
        left -= static_cast<std::uint64_t>(written);
    }
    if (e.denoise != nullptr) {  // the learned noise belongs to the state
        std::array<float, 2 * kBins> profile{};
        for (std::size_t c = 0; c < 2; ++c) {
            std::copy(e.denoise->channels[c].profile.begin(), e.denoise->channels[c].profile.end(),
                      profile.begin() + static_cast<std::ptrdiff_t>(c * kBins));
        }
        const auto* profile_bytes = reinterpret_cast<const char*>(profile.data());
        std::uint64_t profile_left = sizeof(float) * profile.size();
        while (profile_left > 0) {
            const std::int64_t written = stream->write(stream, profile_bytes, profile_left);
            if (written <= 0) {
                return false;
            }
            profile_bytes += written;
            profile_left -= static_cast<std::uint64_t>(written);
        }
    }
    return true;
}

bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    Example& e = self(plugin);
    std::array<double, 8> data{};
    auto* bytes = reinterpret_cast<char*>(data.data());
    std::uint64_t left = sizeof(double) * e.count;
    while (left > 0) {
        const std::int64_t read = stream->read(stream, bytes, left);
        if (read <= 0) {
            return false;
        }
        bytes += read;
        left -= static_cast<std::uint64_t>(read);
    }
    for (std::uint32_t i = 0; i < e.count; ++i) {
        e.values[i].store(std::clamp(data[i], e.defs[i].min, e.defs[i].max));
    }
    if (e.denoise != nullptr) {
        e.values[0].store(0.0);  // not learning after a load
        std::array<float, 2 * kBins> profile{};
        auto* profile_bytes = reinterpret_cast<char*>(profile.data());
        std::uint64_t profile_left = sizeof(float) * profile.size();
        while (profile_left > 0) {
            const std::int64_t read = stream->read(stream, profile_bytes, profile_left);
            if (read <= 0) {
                break;
            }
            profile_bytes += read;
            profile_left -= static_cast<std::uint64_t>(read);
        }
        if (profile_left == 0) {
            e.denoise->pending = profile;
            e.denoise->has_pending.store(true);
        }
    }
    return true;
}

const clap_plugin_state_t kState = {state_save, state_load};

// The denoiser delays the sound by the pieces it works on; the host makes up for it.
std::uint32_t latency_get(const clap_plugin_t* plugin) {
    const Example& e = self(plugin);
    if (e.limiter != nullptr) {
        return static_cast<std::uint32_t>(e.limiter->length - 1);
    }
    return static_cast<std::uint32_t>(kFftLatency);
}

const clap_plugin_latency_t kLatency = {latency_get};

const void* plugin_extension(const clap_plugin_t* plugin, const char* id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &kPorts;
    }
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &kParams;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &kState;
    }
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0 &&
        (self(plugin).kind == Kind::Denoise || self(plugin).kind == Kind::Limiter)) {
        return &kLatency;
    }
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0 && self(plugin).kind == Kind::Synth) {
        return &kNotePorts;
    }
    return nullptr;
}

void plugin_on_main_thread(const clap_plugin_t*) {}

// ---- The factory -----------------------------------------------------------------------------------

const clap_plugin_descriptor_t* const kDescriptors[] = {&kEcho, &kTremolo, &kGain, &kSynth, &kGate, &kDenoise,
                                                        &kEq, &kComp, &kLimit};
constexpr std::uint32_t kPluginCount = 9;

std::uint32_t factory_count(const clap_plugin_factory_t*) { return kPluginCount; }

const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*, std::uint32_t index) {
    return index < kPluginCount ? kDescriptors[index] : nullptr;
}

const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (host == nullptr || id == nullptr || !clap_version_is_compatible(host->clap_version)) {
        return nullptr;
    }
    auto* e = new Example();
    if (std::strcmp(id, kEcho.id) == 0) {
        e->kind = Kind::Echo;
        e->defs = kEchoParams;
        e->count = 3;
        e->plugin.desc = &kEcho;
    } else if (std::strcmp(id, kTremolo.id) == 0) {
        e->kind = Kind::Tremolo;
        e->defs = kTremoloParams;
        e->count = 2;
        e->plugin.desc = &kTremolo;
    } else if (std::strcmp(id, kGain.id) == 0) {
        e->kind = Kind::Gain;
        e->defs = kGainParams;
        e->count = 2;
        e->plugin.desc = &kGain;
    } else if (std::strcmp(id, kSynth.id) == 0) {
        e->kind = Kind::Synth;
        e->defs = kSynthParams;
        e->count = 4;
        e->plugin.desc = &kSynth;
    } else if (std::strcmp(id, kGate.id) == 0) {
        e->kind = Kind::Gate;
        e->defs = kGateParams;
        e->count = 4;
        e->plugin.desc = &kGate;
    } else if (std::strcmp(id, kEq.id) == 0) {
        e->kind = Kind::Equalizer;
        e->defs = kEqParams;
        e->count = 7;
        e->plugin.desc = &kEq;
    } else if (std::strcmp(id, kComp.id) == 0) {
        e->kind = Kind::Compressor;
        e->defs = kCompParams;
        e->count = 5;
        e->plugin.desc = &kComp;
    } else if (std::strcmp(id, kLimit.id) == 0) {
        e->kind = Kind::Limiter;
        e->defs = kLimitParams;
        e->count = 3;
        e->plugin.desc = &kLimit;
        e->limiter = new Limiter();
        e->limiter->prepare(48000.0);
    } else if (std::strcmp(id, kDenoise.id) == 0) {
        e->kind = Kind::Denoise;
        e->defs = kDenoiseParams;
        e->count = 4;
        e->plugin.desc = &kDenoise;
        e->denoise = new Denoise();
        for (std::size_t k = 0; k < kFft; ++k) {
            e->denoise->window[k] = static_cast<float>(
                std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(k) / static_cast<double>(kFft))));
        }
        for (DenoiseChannel& ch : e->denoise->channels) {
            ch.gain.fill(1.0f);
        }
    } else {
        delete e;
        return nullptr;
    }
    for (std::uint32_t i = 0; i < e->count; ++i) {
        e->values[i].store(e->defs[i].default_value);
    }
    e->plugin.plugin_data = e;
    e->plugin.init = plugin_init;
    e->plugin.destroy = plugin_destroy;
    e->plugin.activate = plugin_activate;
    e->plugin.deactivate = plugin_deactivate;
    e->plugin.start_processing = plugin_start;
    e->plugin.stop_processing = plugin_stop;
    e->plugin.reset = plugin_reset;
    e->plugin.process = plugin_process;
    e->plugin.get_extension = plugin_extension;
    e->plugin.on_main_thread = plugin_on_main_thread;
    return &e->plugin;
}

const clap_plugin_factory_t kFactory = {factory_count, factory_descriptor, factory_create};

bool entry_init(const char*) { return true; }

void entry_deinit() {}

const void* entry_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit,
                                                                entry_factory};
