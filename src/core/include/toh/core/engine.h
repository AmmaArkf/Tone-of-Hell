#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "toh/core/clip.h"
#include "toh/core/spsc_queue.h"

namespace toh::core {

/// The audio engine: a transport (play, pause, stop, seek, loop) and a mixer.
///
/// Signal path: every track plays one clip that starts at the beginning of the timeline. A
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

    /// Puts a clip on a track, stops playback and returns to the start. The clip must match the
    /// engine's sample rate and channel count (convert it when loading it).
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
    bool play();
    /// Fades out and stops, keeping the position.
    bool pause();
    /// Fades out, stops and returns to the start.
    bool stop();
    /// Moves to a frame. While playing, the sound fades out and back in around the jump.
    bool seek(std::int64_t frame);
    /// Turns looping on or off. The loop covers [start_frame, end_frame); an end_frame of -1
    /// means the end of the longest clip.
    bool set_loop(bool enabled, std::int64_t start_frame = 0, std::int64_t end_frame = -1);

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

    std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    std::uint32_t channels() const noexcept { return channels_; }
    /// Length of the fades used to avoid clicks, in frames.
    std::uint32_t fade_frames() const noexcept { return fade_frames_; }

    // ---- Audio thread --------------------------------------------------------------------

    /// Fills `out` with `frames` interleaved frames (`channels()` samples each).
    void render(float* out, std::uint32_t frames) noexcept;

private:
    static constexpr std::uint32_t kBlockFrames = 512;  // internal processing block

    enum class CommandType : std::uint8_t {
        SetTrackClip,
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
        SetLoop
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
        const ClipData* clip = nullptr;
    };

    enum class State : std::uint8_t { Stopped, Playing, FadingOut };
    enum class Pending : std::uint8_t { None, Pause, Stop, Seek };

    struct Track {
        const ClipData* clip = nullptr;
        std::int64_t frames = 0;
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
    };

    struct Bus {
        float gain = 1.0f;
        bool mute = false;
        float target = 1.0f;
        float current = 1.0f;
        float meter = 0.0f;
    };

    void apply(const Command& command) noexcept;
    void update_targets() noexcept;
    void finish_fade() noexcept;
    bool loop_region(std::int64_t& start, std::int64_t& end) const noexcept;
    bool run_transport(std::uint32_t frames, std::int64_t loop_start, std::int64_t end_pos,
                       bool looping) noexcept;
    void mix_block(float* out, std::uint32_t frames, bool audible) noexcept;

    const std::uint32_t sample_rate_;
    const std::uint32_t channels_;
    std::uint32_t fade_frames_ = 1;
    float fade_step_ = 1.0f;
    float gain_coef_ = 1.0f;

    SpscQueue<Command, 1024> commands_;               // control thread -> audio thread
    SpscQueue<const ClipData*, 256> retired_;         // audio thread -> control thread
    std::vector<std::shared_ptr<const ClipData>> owned_;  // control thread only

    // Audio-thread state.
    std::array<Track, kMaxTracks> tracks_{};
    std::array<Bus, kMaxBuses> buses_{};
    std::int64_t project_frames_ = 0;
    State state_ = State::Stopped;
    Pending pending_ = Pending::None;
    std::int64_t pending_seek_ = 0;
    std::int64_t pos_ = 0;
    bool loop_enabled_ = false;
    std::int64_t loop_start_ = 0;
    std::int64_t loop_end_ = -1;
    float fade_ = 0.0f;
    float gain_ = 1.0f;
    float gain_target_ = 1.0f;
    float master_meter_ = 0.0f;

    // Work buffers, allocated once in the constructor.
    std::vector<std::int64_t> frame_pos_;  // timeline frame for every output frame, -1 = silence
    std::vector<float> frame_fade_;
    std::vector<float> master_mix_;
    std::vector<float> bus_mix_;

    // Copies of the audio-thread state for other threads to read.
    std::atomic<std::int64_t> published_position_{0};
    std::atomic<std::int64_t> published_length_{0};
    std::atomic<bool> published_playing_{false};
    std::array<std::atomic<float>, kMaxTracks> track_peaks_{};
    std::array<std::atomic<float>, kMaxBuses> bus_peaks_{};
    std::atomic<float> master_peak_{0.0f};
    std::atomic<std::uint64_t> master_clips_{0};
};

}  // namespace toh::core
