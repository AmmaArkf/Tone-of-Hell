#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "toh/audio/audio_device.h"
#include "toh/audio/wav_writer.h"
#include "toh/core/clip.h"
#include "toh/core/engine.h"

namespace toh::app {

/// A reduced picture of a clip for drawing waveforms: the lowest and highest sample value in
/// every bucket of `frames_per_bucket` frames (all channels together).
struct Overview {
    std::uint32_t frames_per_bucket = 256;
    std::vector<float> minimum;
    std::vector<float> maximum;

    /// Lowest and highest value between two frames. Returns false when the range is empty.
    bool range(std::int64_t first_frame, std::int64_t last_frame, float& low,
               float& high) const noexcept;
};

/// Builds the waveform overview of a clip.
Overview make_overview(const core::ClipData& clip, std::uint32_t frames_per_bucket = 256);

/// An audio file (or generated sound) that regions refer to. Never changes once created.
struct Source {
    std::string name;  // UTF-8
    std::shared_ptr<const core::ClipData> clip;
    Overview overview;

    /// Lowest and highest sample value between two frames of the clip, for drawing. Uses the
    /// samples themselves for short ranges and the overview for long ones.
    bool range(std::int64_t first_frame, std::int64_t last_frame, float& low,
               float& high) const noexcept;
};

/// A piece of a source placed on a track. All values are in frames.
struct RegionInfo {
    std::shared_ptr<const Source> source;
    std::int64_t start = 0;     // position on the timeline
    std::int64_t offset = 0;    // first frame of the source that is used
    std::int64_t length = 0;    // number of frames that are used
    std::int64_t fade_in = 0;
    std::int64_t fade_out = 0;

    std::int64_t end() const noexcept { return start + length; }
};

/// What a user interface needs to know about one track.
struct TrackInfo {
    std::string name;  // UTF-8
    /// The picture shown next to the track: empty for none, the name of a built-in picture
    /// (for example "guitar"), or "file:" followed by the path of an image file (UTF-8).
    std::string icon;
    std::vector<RegionInfo> regions;  // in the order they were created
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
};

/// Settings of the built-in metronome. `bpm` counts quarter notes per minute; the time
/// signature is `beats_per_bar` / `beat_unit`. The same values define the timeline grid.
struct MetronomeSettings {
    bool enabled = false;
    double bpm = 120.0;
    int beats_per_bar = 4;
    int beat_unit = 4;
    float volume = 0.5f;
};

/// How fine the grid is that edits snap to. `None` means no grid at all: nothing snaps and
/// user interfaces hide the grid lines and the bar numbers.
enum class GridStep { Bar, Beat, HalfBeat, QuarterBeat, None };

/// The state of the program that every user interface shares: the list of tracks, the engine
/// and the audio device. A user interface only draws this state and calls these methods; it
/// contains no audio logic of its own, so several interfaces can exist side by side.
///
/// All methods must be called from one thread (the UI thread).
class Session {
public:
    explicit Session(const audio::DeviceConfig& config = audio::DeviceConfig());
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Opens the sound device. Without it the session still works, silently.
    bool start_audio(std::string* error = nullptr);
    void stop_audio();
    bool audio_running() const noexcept { return device_.running(); }
    const audio::DeviceConfig& config() const noexcept { return config_; }

    /// Call regularly (for example once per drawn frame) for housekeeping.
    void update();

    // ---- Tracks --------------------------------------------------------------------------

    /// Loads a file onto a new track, as one region at the start of the timeline. This call
    /// blocks while the file is read.
    bool add_track_from_file(const std::filesystem::path& path, std::string* error = nullptr);
    /// Adds a track with an already loaded clip (used for test tones and by tests).
    bool add_track(std::string name, std::shared_ptr<const core::ClipData> clip,
                   std::string* error = nullptr);
    bool remove_track(std::size_t index);
    /// Adds a track without any audio, for example to record onto. Returns its index, or
    /// tracks().size() when the track limit is reached.
    std::size_t add_empty_track(std::string name);

    const std::vector<TrackInfo>& tracks() const noexcept { return tracks_; }
    bool set_track_gain(std::size_t index, float linear);
    bool set_track_pan(std::size_t index, float pan);
    bool set_track_mute(std::size_t index, bool mute);
    bool set_track_solo(std::size_t index, bool solo);
    /// Renames a track. Spaces at either end are removed; an empty name is refused.
    bool set_track_name(std::size_t index, const std::string& name);
    /// Sets the picture of a track (see TrackInfo::icon).
    bool set_track_icon(std::size_t index, std::string icon);
    float track_peak(std::size_t index) const noexcept;

    // ---- Regions -------------------------------------------------------------------------
    // Edits never interrupt playback. Each returns false when the track or region does not
    // exist; values out of range are limited to what is possible.

    /// Moves a region so that it begins at `start` (not before the start of the timeline).
    bool move_region(std::size_t track, std::size_t region, std::int64_t start);
    /// Moves the beginning of a region to `start`, keeping its end and the audio in place.
    bool trim_region_start(std::size_t track, std::size_t region, std::int64_t start);
    /// Moves the end of a region to `end`, keeping its beginning in place.
    bool trim_region_end(std::size_t track, std::size_t region, std::int64_t end);
    bool set_region_fades(std::size_t track, std::size_t region, std::int64_t fade_in,
                          std::int64_t fade_out);
    /// Cuts a region in two at a timeline frame. The second part is added at the end of the
    /// track's region list. Returns false when `frame` is not inside the region.
    bool split_region(std::size_t track, std::size_t region, std::int64_t frame);
    bool remove_region(std::size_t track, std::size_t region);

    // ---- Grid ----------------------------------------------------------------------------

    bool snap() const noexcept { return snap_; }
    void set_snap(bool enabled) noexcept { snap_ = enabled; }
    GridStep grid_step() const noexcept { return grid_step_; }
    void set_grid_step(GridStep step) noexcept { grid_step_ = step; }
    /// Length of one beat and of one bar in frames (fractional), from the metronome settings.
    double beat_frames() const noexcept;
    double bar_frames() const noexcept;
    /// The nearest grid position when snapping is on; otherwise `frame` itself.
    std::int64_t snap_frame(std::int64_t frame) const noexcept;

    // ---- Recording -----------------------------------------------------------------------

    /// Used as the track argument of start_recording() to record onto a new track.
    static constexpr std::size_t kNewTrack = static_cast<std::size_t>(-1);

    /// Opens or closes the input device (microphone or line input). The sound device is
    /// restarted, which interrupts playback for a moment. Returns false if no input could be
    /// opened; `error` (if given) says why.
    bool set_input_enabled(bool enabled, std::string* error = nullptr);
    bool input_enabled() const noexcept { return device_.has_input(); }

    /// The devices in use, by name; an empty name means the system default.
    const std::string& output_device() const noexcept { return config_.output_name; }
    const std::string& input_device() const noexcept { return config_.input_name; }
    /// Chooses the devices (names from audio::list_devices(); empty for the system default).
    /// If the audio is running it is restarted, which interrupts playback for a moment.
    bool set_devices(const std::string& output, const std::string& input,
                     std::string* error = nullptr);

    /// Input gain ("sensitivity"), linear, 0 to 8. Applied before monitoring and recording.
    float input_gain() const noexcept { return input_gain_; }
    bool set_input_gain(float linear);
    /// Peak level of the input after the input gain (0 when no input is open).
    float input_peak() const noexcept { return device_.has_input() ? engine_.input_peak() : 0.0f; }

    /// Lets you hear the input through the output. Beware of feedback with loudspeakers.
    bool monitoring() const noexcept { return monitoring_; }
    void set_monitoring(bool enabled);

    /// How much earlier than it arrived a recording is placed on the timeline, in frames. It
    /// makes up for the time sound needs to pass through the sound card (output plus input).
    std::int64_t record_offset_frames() const noexcept { return record_offset_; }
    void set_record_offset_frames(std::int64_t frames) noexcept;

    /// Starts playback and records the input onto `track` (or a new track). The take ends
    /// with stop_recording(), or when playback stops, pauses or jumps; it then appears as a
    /// region. Returns false if a recording is already running or no track is available.
    bool start_recording(std::size_t track = kNewTrack);
    /// Ends the take and pauses playback. The region appears after the next update().
    void stop_recording();
    bool is_recording() const noexcept { return recording_; }
    /// The track being recorded onto (only meaningful while is_recording()).
    std::size_t record_track() const noexcept { return record_track_; }
    /// Length of the running take so far, in frames.
    std::int64_t recorded_frames() const noexcept;
    /// Waveform of the running take, growing while it is recorded. Frame 0 is the first frame
    /// of the take.
    const Overview& take_overview() const noexcept { return take_overview_; }
    /// True when the running take reached full scale: the input was overloaded.
    bool take_clipped() const noexcept { return take_clipped_; }

    // ---- Export --------------------------------------------------------------------------

    /// Writes the whole project to a WAV file as it sounds (faders, pan, mute, solo and the
    /// master fader included; the metronome and the monitored input are not). Playback is not
    /// disturbed. This call blocks until the file is written.
    bool export_wav(const std::filesystem::path& path, audio::SampleFormat format,
                    std::string* error = nullptr) const;
    /// The same as export_wav, as an MP3 file. `kbps` is a constant bit rate (320, 192, 128
    /// ...) or 0 for variable bit rate at the best quality.
    bool export_mp3(const std::filesystem::path& path, int kbps,
                    std::string* error = nullptr) const;

    // ---- Master --------------------------------------------------------------------------

    float master_gain() const noexcept { return master_gain_; }
    bool set_master_gain(float linear);
    float master_peak() const noexcept { return engine_.master_peak(); }
    std::uint64_t master_clip_count() const noexcept { return engine_.master_clip_count(); }

    // ---- Transport -----------------------------------------------------------------------

    void play();
    void pause();
    void toggle_play();
    void stop();
    void seek_frames(std::int64_t frame);
    bool loop() const noexcept { return loop_; }
    void set_loop(bool enabled);
    bool reverse() const noexcept { return reverse_; }
    /// Plays backwards when enabled.
    void set_reverse(bool reverse);
    /// What a "reverse" button does: changes the direction and makes sure playback is running,
    /// starting from the current position.
    void toggle_reverse();
    const MetronomeSettings& metronome() const noexcept { return metronome_; }
    /// Returns false (and changes nothing) when a value is out of range.
    bool set_metronome(const MetronomeSettings& settings);

    bool is_playing() const noexcept { return engine_.is_playing(); }
    std::int64_t position_frames() const noexcept { return engine_.position_frames(); }
    /// End of the last region on any track, in frames.
    std::int64_t length_frames() const noexcept { return length_frames_; }
    std::uint32_t sample_rate() const noexcept { return config_.sample_rate; }

    /// Direct access for tests that render audio without a sound device.
    core::Engine& engine() noexcept { return engine_; }

private:
    RegionInfo* find_region(std::size_t track, std::size_t region) noexcept;
    void sync_track_regions(std::size_t index);
    void sync_engine_tracks(std::size_t first);
    void update_length();
    void drain_recording();
    /// Renders the whole project offline and hands it to `sink` block by block. Stops and
    /// returns false as soon as `sink` returns false.
    bool render_project(const std::function<bool(const float*, std::uint32_t)>& sink) const;
    void finish_take();
    void extend_take_overview(const float* interleaved, std::size_t frames);

    audio::DeviceConfig config_;
    core::Engine engine_;
    audio::Device device_;
    std::vector<TrackInfo> tracks_;
    float master_gain_ = 1.0f;
    bool loop_ = false;
    bool reverse_ = false;
    bool snap_ = true;
    GridStep grid_step_ = GridStep::Beat;
    MetronomeSettings metronome_;
    std::int64_t length_frames_ = 0;

    bool monitoring_ = false;
    float input_gain_ = 1.0f;
    bool recording_ = false;
    bool record_seen_ = false;     // the engine has confirmed that it is recording
    bool stop_requested_ = false;
    std::size_t record_track_ = kNewTrack;
    std::int64_t record_offset_ = 0;
    std::vector<float> take_;          // the input recorded so far
    std::vector<float> drain_buffer_;
    Overview take_overview_;
    std::size_t take_bucket_fill_ = 0;  // frames already in the last bucket of take_overview_
    bool take_clipped_ = false;
    int take_count_ = 0;
};

}  // namespace toh::app
