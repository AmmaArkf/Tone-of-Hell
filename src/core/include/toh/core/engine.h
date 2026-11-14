#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <cmath>

#include "toh/core/clip.h"
#include "toh/core/effect.h"
#include "toh/core/sampler.h"
#include "toh/core/soundfont.h"
#include "toh/core/spsc_queue.h"
#include "toh/core/tempo.h"
#include "toh/core/spsc_ring.h"

namespace toh::core {

/// A piece of a clip placed on the timeline of a track.
/// The shape of a fade: how the level rises from silence (t = 0) to full (t = 1). A fade-out
/// uses the same shape seen from the other side.
enum class FadeShape : std::uint8_t {
    Linear,  ///< A straight line.
    Curve,   ///< Slow at first, then fast (t squared): sounds even to the ear.
    SCurve,  ///< Slow at both ends (smoothstep).
};

/// The level of a fade with `shape` at `t` (0 to 1).
inline float fade_curve(FadeShape shape, float t) noexcept {
    switch (shape) {
        case FadeShape::Curve:
            return t * t;
        case FadeShape::SCurve:
            return t * t * (3.0f - 2.0f * t);
        case FadeShape::Linear:
            break;
    }
    return t;
}

struct Region {
    std::shared_ptr<const ClipData> clip;
    std::int64_t start = 0;     ///< Where it begins on the timeline, in frames.
    std::int64_t offset = 0;    ///< First frame of the clip that is used.
    std::int64_t length = 0;    ///< Number of frames that are used.
    std::int64_t fade_in = 0;   ///< Length of the fade at its beginning, in frames.
    std::int64_t fade_out = 0;  ///< Length of the fade at its end, in frames.
    FadeShape fade_shape = FadeShape::Linear;
    /// How fast the region plays its clip (1 = as recorded, 2 = twice as fast and an octave
    /// higher, like a tape). `length` counts timeline frames; the clip frames used are
    /// clip_frames_used().
    double rate = 1.0;
    /// For note regions (clip->notes): the instrument that plays them.
    Instrument instrument = Instrument::Keys;
    /// For note regions: a program of the SoundFont (0..127, SoundFont::kDrums) that plays them
    /// instead of `instrument`, or -1.
    int program = -1;
    /// For note regions: a sound that plays them instead of both (a sampler), or null.
    std::shared_ptr<const SamplerSound> sampler;
};

/// One point of an automation curve: the value at a timeline frame. Between two points the
/// value moves in a straight line; before the first and after the last it stays.
struct CurvePoint {
    std::int64_t frame = 0;
    float value = 1.0f;
};

/// The value of a curve (points sorted by frame, not empty) at a timeline frame.
inline float curve_at(const std::vector<CurvePoint>& points, std::int64_t frame) noexcept {
    if (frame <= points.front().frame) {
        return points.front().value;
    }
    if (frame >= points.back().frame) {
        return points.back().value;
    }
    std::size_t low = 0;
    std::size_t high = points.size() - 1;
    while (high - low > 1) {
        const std::size_t middle = (low + high) / 2;
        (points[middle].frame <= frame ? low : high) = middle;
    }
    const CurvePoint& a = points[low];
    const CurvePoint& b = points[high];
    const float t = static_cast<float>(frame - a.frame) / static_cast<float>(b.frame - a.frame);
    return a.value + (b.value - a.value) * t;
}

/// The automation of one parameter of a plugin on a track: its value along the timeline.
struct ParamCurve {
    std::shared_ptr<AudioEffect> effect;
    std::uint32_t id = 0;
    std::vector<CurvePoint> points;  ///< sorted by frame, not empty
    mutable bool sent = false;       // audio thread: the value given to the plugin last
    mutable float last = 0.0f;
};

/// How many frames of its clip a region plays.
inline std::int64_t clip_frames_used(std::int64_t length, double rate) noexcept {
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(length) * rate - 1e-9));
}

/// The audio engine: a transport (play, pause, stop, seek, loop) and a mixer.
///
/// Signal path: every track holds regions (pieces of clips placed on the timeline). A
/// track has a fader, a pan control, mute and solo, and sends its signal either straight to the
/// master or to one of the buses. Every bus has a fader and mute and feeds the master. The
/// master has a fader. Mixing is done in 32-bit float without any clipping; levels above 1.0
/// at the master are counted (see master_clip_count) and left for the audio device to clamp.
///
/// The engine knows nothing about sound cards. The audio backend calls render() whenever it
/// needs more samples, which keeps the engine fully testable without any audio hardware.
///
/// Threading rules
///  - The control methods (everything that returns bool, and collect_retired) must all be
///    called from ONE thread, for example the UI thread. They only post messages to the audio
///    thread through a lock-free queue and return false if the queue is full or an argument is
///    invalid.
///  - render() is called from the audio thread only. It never allocates, locks or throws.
///  - The state methods (is_playing, position_frames, the meters, ...) may be called from any
///    thread.
class Engine {
public:
    static constexpr std::uint32_t kMaxTracks = 64;
    static constexpr std::uint32_t kMaxBuses = 8;
    /// Track output that goes straight to the master.
    static constexpr int kMasterOutput = -1;

    Engine(std::uint32_t sample_rate, std::uint32_t channels);
    ~Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- Tracks (control thread) ---------------------------------------------------------

    /// Replaces everything on a track with the given regions, without interrupting playback.
    /// Every clip must match the engine's sample rate and channel count (convert it when
    /// loading it), and every region must lie inside its clip: start >= 0, offset >= 0,
    /// length > 0 and offset + length <= clip frames. Fades longer than the region are
    /// shortened. Regions that overlap are mixed together. An empty list empties the track.
    /// `effects` are the insert effects of the track, in the order the sound passes them.
    /// `instrument` (a plugin) plays the note regions of the track instead, before the effects.
    /// `volume` is the volume automation of the track (gain 0 to 2, multiplied with its fader).
    /// `params` automate parameters of the track's plugins: once per block the plugin gets the
    /// value of the curve where the playhead is.
    /// `pan` is its pan automation (-1 left to 1 right); while it has points, they decide the
    /// pan instead of set_track_pan().
    /// `latency` is how many frames the instrument and effects together delay the sound: the
    /// track then reads its regions that much ahead (delay compensation, up to 2 seconds).
    bool set_track_regions(std::uint32_t track, const std::vector<Region>& regions,
                           const std::vector<std::shared_ptr<AudioEffect>>& effects = {},
                           std::shared_ptr<AudioEffect> instrument = nullptr, std::int64_t latency = 0,
                           std::vector<CurvePoint> volume = {}, std::vector<CurvePoint> pan = {},
                           std::vector<ParamCurve> params = {});
    /// Asks the audio thread to confirm that it has taken in every change made so far:
    /// synced() returns `token` (or a later one) once it has.
    bool request_sync(std::int64_t token);
    std::int64_t synced() const noexcept { return synced_.load(std::memory_order_acquire); }
    /// A note played live on an instrument plugin (it must be the instrument of a track).
    bool live_plugin_note(std::shared_ptr<AudioEffect> instrument, bool on, int pitch, float velocity);
    /// Puts one whole clip at the start of a track, stops playback and returns to the start.
    bool set_track_clip(std::uint32_t track, std::shared_ptr<const ClipData> clip);
    /// Removes the clip from a track, stops playback and returns to the start.
    bool clear_track(std::uint32_t track);
    /// Track fader as a linear factor between 0 and 8 (1 = unchanged).
    bool set_track_gain(std::uint32_t track, float linear);
    /// Pan from -1 (left) to +1 (right). Only has an effect when the engine is stereo. The
    /// centre position leaves both channels unchanged.
    bool set_track_pan(std::uint32_t track, float pan);
    bool set_track_mute(std::uint32_t track, bool mute);
    /// While at least one track is soloed, only soloed tracks are heard.
    bool set_track_solo(std::uint32_t track, bool solo);
    /// Sends the track to a bus (0 .. kMaxBuses-1) or to the master (kMasterOutput).
    bool set_track_output(std::uint32_t track, int bus);

    /// Shortcut for a single clip: the same as set_track_clip(0, clip).
    bool set_clip(std::shared_ptr<const ClipData> clip) { return set_track_clip(0, std::move(clip)); }

    // ---- Buses and master (control thread) ----------------------------------------------

    bool set_bus_gain(std::uint32_t bus, float linear);
    bool set_bus_mute(std::uint32_t bus, bool mute);
    /// Master fader as a linear factor between 0 and 8 (1 = unchanged).
    bool set_gain(float linear);

    // ---- Transport (control thread) ------------------------------------------------------

    /// Starts playing, or resumes after a pause. Playing a finished project starts it again.
    /// With `fade_in` false the sound starts at full level at once (used for offline export,
    /// where the short anti-click fade would alter the first milliseconds).
    bool play(bool fade_in = true);
    /// Fades out and stops, keeping the position.
    bool pause();
    /// Fades out, stops and returns to the start.
    bool stop();
    /// Moves to a frame. While playing, the sound fades out and back in around the jump.
    bool seek(std::int64_t frame);
    /// Turns looping on or off. The loop covers [start_frame, end_frame); an end_frame of -1
    /// means the end of the longest clip.
    bool set_loop(bool enabled, std::int64_t start_frame = 0, std::int64_t end_frame = -1);
    /// Plays backwards when enabled. While playing, the sound fades out and back in around the
    /// change of direction. Starting backwards from the very beginning starts at the end.
    bool set_reverse(bool reverse);
    /// Playback speed from kMinSpeed to kMaxSpeed (1 = normal). Like a tape, the pitch
    /// changes with the speed; frames in between are blended. Recording always runs at the
    /// normal speed. Any thread; returns false for a value out of range.
    bool set_speed(double speed) noexcept;
    double speed() const noexcept { return speed_.load(std::memory_order_relaxed); }
    /// The recorded instruments for note regions with a program. Control thread; the
    /// SoundFont stays alive as long as the engine.
    void set_soundfont(std::shared_ptr<const SoundFont> soundfont);

    /// Live notes, played right away (from a keyboard) whether or not playback runs. `program`
    /// and `instrument` choose the sound as for note regions. Control thread.
    bool live_note_on(int pitch, float velocity, int program, Instrument instrument,
                      std::shared_ptr<const SamplerSound> sampler = nullptr);
    bool live_note_off(int pitch);
    bool live_all_notes_off();
    static constexpr double kMinSpeed = 0.25;
    static constexpr double kMaxSpeed = 4.0;
    /// Configures the built-in metronome click, which follows the timeline position.
    /// `bpm` counts quarter notes per minute (20 to 400). The time signature is
    /// `beats_per_bar` / `beat_unit` (for example 6/8); the first beat of every bar sounds
    /// higher. `volume` is linear, 0 to 2, and is not affected by the master fader. With the
    /// metronome on, playback also runs when no clip is loaded.
    bool set_metronome(bool enabled, double bpm = 120.0, std::uint32_t beats_per_bar = 4,
                       std::uint32_t beat_unit = 4, float volume = 0.5f);
    /// The tempo along the timeline, for the metronome clicks; null: the tempo of
    /// set_metronome() throughout. Control thread.
    void set_tempo_map(std::shared_ptr<const TempoMap> map);
    /// With the metronome on, playback goes on past the end of the project until it is
    /// stopped (unless it loops). On by default. Any thread.
    void set_metronome_endless(bool endless) noexcept {
        metronome_endless_.store(endless, std::memory_order_relaxed);
    }
    bool metronome_endless() const noexcept {
        return metronome_endless_.load(std::memory_order_relaxed);
    }

    // ---- Input: monitoring and recording (control thread) -------------------------------

    /// Mixes the input (microphone, instrument) into the output, after the master fader.
    /// `gain` is linear, 0 to 8. Beware of feedback when loudspeakers are used.
    bool set_monitoring(bool enabled, float gain = 1.0f);
    /// Input gain ("sensitivity") as a linear factor between 0 and 8. It is applied to the
    /// input before monitoring, recording and the input meter.
    bool set_input_gain(float linear);
    /// Arms recording. A take begins with the next frame that plays forwards and ends when
    /// playback stops, pauses, jumps or turns around, or with stop_recording(). While armed,
    /// playback also runs without clips and does not stop at the end of the project.
    bool start_recording();
    bool stop_recording();
    /// Fetches recorded input: up to `max_frames` interleaved frames are copied to
    /// `destination`. Returns the number of frames copied. Call it regularly while recording
    /// (the engine buffers about four seconds) and until it returns 0 afterwards.
    std::size_t read_recorded(float* destination, std::size_t max_frames);

    /// Releases clips the audio thread no longer uses. Call it now and then from the control
    /// thread (for example after changing clips).
    void collect_retired();

    // ---- State (any thread) --------------------------------------------------------------

    /// True while sound is playing, including the short fade-out after pause or stop.
    bool is_playing() const noexcept { return published_playing_.load(std::memory_order_relaxed); }
    /// Position of the next frame to play. Updated once per rendered block.
    std::int64_t position_frames() const noexcept {
        return published_position_.load(std::memory_order_relaxed);
    }
    /// Length of the longest clip, in frames.
    std::int64_t length_frames() const noexcept {
        return published_length_.load(std::memory_order_relaxed);
    }

    /// Peak meters: the highest absolute sample value after the fader, falling back slowly.
    /// 1.0 is full scale. Out-of-range indices return 0.
    float track_peak(std::uint32_t track) const noexcept;
    float bus_peak(std::uint32_t bus) const noexcept;
    float master_peak() const noexcept { return master_peak_.load(std::memory_order_relaxed); }
    /// Number of master output samples above full scale since the engine was created.
    std::uint64_t master_clip_count() const noexcept {
        return master_clips_.load(std::memory_order_relaxed);
    }

    /// Peak level of the input after the input gain, falling back slowly. 1.0 is full scale.
    float input_peak() const noexcept { return input_peak_.load(std::memory_order_relaxed); }

    /// True from start_recording() until the take has ended.
    bool is_recording() const noexcept {
        return published_recording_.load(std::memory_order_relaxed);
    }
    /// Timeline frame at which the current (or last) take began, or -1 if nothing was recorded
    /// since start_recording().
    std::int64_t record_start_frame() const noexcept {
        return record_start_.load(std::memory_order_relaxed);
    }
    /// Input frames lost because read_recorded() was not called often enough.
    std::uint64_t record_dropped_frames() const noexcept {
        return record_dropped_.load(std::memory_order_relaxed);
    }

    std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    std::uint32_t channels() const noexcept { return channels_; }
    /// Length of the fades used to avoid clicks, in frames.
    std::uint32_t fade_frames() const noexcept { return fade_frames_; }

    // ---- Audio thread --------------------------------------------------------------------

    /// Fills `out` with `frames` interleaved frames (`channels()` samples each). `in` holds
    /// the same number of input frames in the same layout, or is null when there is no input.
    void render(const float* in, float* out, std::uint32_t frames) noexcept;
    void render(float* out, std::uint32_t frames) noexcept { render(nullptr, out, frames); }

    /// Makes clips on disk wait for the disk instead of playing silence for frames that are
    /// not loaded yet. Only for engines that render offline (exports), never for a live one.
    /// Set it before the first render().
    void set_blocking_reads(bool enabled) noexcept { blocking_reads_ = enabled; }
    /// Frames that the audio thread wanted from clips on disk before they were loaded (they
    /// were played as silence). Any thread.
    std::uint64_t disk_misses() const noexcept {
        return disk_misses_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint32_t kBlockFrames = 512;  // internal processing block

    // Everything on one track. Built on the control thread, then only read by the audio thread.
    struct TrackContent {
        std::vector<Region> regions;
        std::vector<std::shared_ptr<AudioEffect>> effects;
        std::shared_ptr<AudioEffect> instrument;
        std::int64_t latency = 0;  // frames the plugins delay the sound
        std::vector<CurvePoint> volume;  // volume automation, sorted by frame
        std::vector<CurvePoint> pan;     // pan automation, sorted by frame
        std::vector<ParamCurve> params;  // automation of plugin parameters
        std::int64_t end = 0;  // end of the last region
    };

    enum class CommandType : std::uint8_t {
        SetTrackContent,
        SetTrackGain,
        SetTrackPan,
        SetTrackMute,
        SetTrackSolo,
        SetTrackOutput,
        SetBusGain,
        SetBusMute,
        SetGain,
        Play,
        Pause,
        Stop,
        Seek,
        SetLoop,
        SetReverse,
        SetMetronome,
        SetMonitoring,
        SetInputGain,
        StartRecording,
        StopRecording,
        LiveNoteOn,
        LiveNoteOff,
        LiveAllOff,
        LivePluginNote,
        Sync
    };

    // Messages from the control thread to the audio thread. Must stay trivially copyable.
    struct Command {
        Command() noexcept = default;
        explicit Command(CommandType t) noexcept : type(t) {}

        CommandType type = CommandType::Stop;
        bool flag = false;
        std::int32_t index = 0;
        float value = 0.0f;
        float value2 = 0.0f;
        std::int64_t a = 0;
        std::int64_t b = 0;
        double number = 0.0;
        const TrackContent* content = nullptr;
        const SamplerSound* sampler = nullptr;
        AudioEffect* effect = nullptr;
    };

    enum class State : std::uint8_t { Stopped, Playing, FadingOut };
    enum class Pending : std::uint8_t { None, Pause, Stop, Seek };

    struct Track {
        const TrackContent* content = nullptr;
        std::int64_t frames = 0;  // end of the last region
        float gain = 1.0f;
        float pan_left = 1.0f;
        float pan_right = 1.0f;
        bool mute = false;
        bool solo = false;
        int bus = kMasterOutput;
        float target_left = 1.0f;  // what the fader, pan, mute and solo add up to
        float target_right = 1.0f;
        float current_left = 1.0f;  // smoothed towards the target
        float current_right = 1.0f;
        float meter = 0.0f;
        std::int64_t last_position = -1;  // for an instrument plugin: where the last block ended
        float automation = 1.0f;          // the last value of the volume automation
        float auto_pan_left = 1.0f;       // the last gains of the pan automation
        float auto_pan_right = 1.0f;
        bool notes_on = false;            // notes were started on it and not all ended
    };

    struct Bus {
        float gain = 1.0f;
        bool mute = false;
        float target = 1.0f;
        float current = 1.0f;
        float meter = 0.0f;
    };

    bool push_content(std::uint32_t track, std::unique_ptr<TrackContent> content, bool stop);
    void apply(const Command& command) noexcept;
    void update_targets() noexcept;
    void finish_fade() noexcept;
    bool loop_region(std::int64_t& start, std::int64_t& end) const noexcept;
    bool run_transport(std::uint32_t frames, std::int64_t begin_pos, std::int64_t end_pos,
                       bool looping) noexcept;
    float metronome_sample(std::int64_t frame) const noexcept;
    void mix_block(const float* in, float* out, std::uint32_t frames, bool audible) noexcept;
    void process_input(const float* in, float* out, std::uint32_t frames) noexcept;
    void render_live(float* out, std::uint32_t frames) noexcept;
    void render_stopped_instruments(float* out, std::uint32_t frames) noexcept;
    void schedule_notes(Track& track, const Region& region, const NoteData& notes, AudioEffect& instrument,
                        std::uint32_t frames, std::int64_t lowest, std::int64_t highest) noexcept;
    std::vector<std::shared_ptr<AudioEffect>> live_instruments_;  // kept alive for live notes
    std::atomic<std::int64_t> synced_{0};

    // Notes from a keyboard (audio thread only).
    struct LiveVoice {
        bool active = false;
        bool released = false;
        int pitch = 60;
        float velocity = 0.8f;
        int program = -1;
        Instrument instrument = Instrument::Keys;
        const SamplerSound* sampler = nullptr;
        double time = 0.0;      // frames since the note began
        double released_at = 0.0;
    };
    std::array<LiveVoice, 32> live_{};

    const std::uint32_t sample_rate_;
    const std::uint32_t channels_;
    std::uint32_t fade_frames_ = 1;
    float fade_step_ = 1.0f;
    float gain_coef_ = 1.0f;

    SpscQueue<Command, 1024> commands_;               // control thread -> audio thread
    SpscQueue<const TrackContent*, 256> retired_;         // audio thread -> control thread
    std::vector<std::unique_ptr<TrackContent>> owned_;  // control thread only

    // Audio-thread state.
    std::array<Track, kMaxTracks> tracks_{};
    std::array<Bus, kMaxBuses> buses_{};
    std::int64_t project_frames_ = 0;
    State state_ = State::Stopped;
    Pending pending_ = Pending::None;
    std::int64_t pending_seek_ = 0;
    std::int64_t pos_ = 0;
    double frac_ = 0.0;  // how far playback is between pos_ and pos_ + 1 (speeds other than 1)
    std::atomic<double> speed_{1.0};
    std::atomic<const SoundFont*> soundfont_{nullptr};
    std::vector<std::shared_ptr<const SoundFont>> soundfonts_;  // keeps every one alive
    std::vector<std::shared_ptr<const SamplerSound>> samplers_;  // the ones live notes used
    bool loop_enabled_ = false;
    std::int64_t loop_start_ = 0;
    std::int64_t loop_end_ = -1;
    float fade_ = 0.0f;
    float gain_ = 1.0f;
    float gain_target_ = 1.0f;
    float master_meter_ = 0.0f;
    bool reverse_ = false;
    bool wanted_reverse_ = false;  // takes effect at the bottom of the next fade
    bool metronome_enabled_ = false;
    std::atomic<bool> metronome_endless_{true};
    double metronome_beat_frames_ = 24000.0;
    std::int64_t metronome_beats_per_bar_ = 4;
    float metronome_volume_ = 0.5f;
    float metronome_gain_ = 0.0f;  // smoothed; 0 while the metronome is off
    std::atomic<const TempoMap*> tempo_{nullptr};
    std::vector<std::shared_ptr<const TempoMap>> tempos_;  // keeps every one alive
    bool monitoring_ = false;
    float monitor_volume_ = 1.0f;
    float monitor_gain_ = 0.0f;  // smoothed; 0 while monitoring is off
    float input_gain_target_ = 1.0f;
    float input_gain_ = 1.0f;  // smoothed
    float input_meter_ = 0.0f;
    std::vector<float> input_frame_;  // one input frame after the input gain
    bool record_armed_ = false;   // between start_recording() and the end of the take
    bool record_active_ = false;  // the take has begun
    std::int64_t record_next_ = 0;  // timeline frame the next recorded frame must have
    SpscRing recorded_;  // audio thread -> control thread

    // Work buffers, allocated once in the constructor.
    std::vector<std::int64_t> frame_pos_;  // timeline frame for every output frame, -1 = silence
    std::vector<float> frame_fade_;
    std::vector<float> frame_frac_;  // for every output frame: the part of the next frame
    std::vector<float> master_mix_;
    std::vector<float> track_mix_;
    std::array<std::int64_t, kBlockFrames> note_prev_{};  // per frame: the position before it
    std::vector<std::int64_t> shifted_pos_;          // frame_pos_ moved ahead for one track
    const std::int64_t* track_pos_ = nullptr;        // the positions the current track reads
    std::vector<float> bus_mix_;

    // Copies of the audio-thread state for other threads to read.
    std::atomic<std::int64_t> published_position_{0};
    std::atomic<std::int64_t> published_length_{0};
    std::atomic<bool> published_playing_{false};
    std::array<std::atomic<float>, kMaxTracks> track_peaks_{};
    std::array<std::atomic<float>, kMaxBuses> bus_peaks_{};
    std::atomic<float> master_peak_{0.0f};
    std::atomic<std::uint64_t> master_clips_{0};
    std::atomic<bool> published_recording_{false};
    std::atomic<float> input_peak_{0.0f};
    std::atomic<std::int64_t> record_start_{-1};
    std::atomic<std::uint64_t> record_dropped_{0};
    std::atomic<std::uint64_t> disk_misses_{0};
    bool blocking_reads_ = false;
};

}  // namespace toh::core
