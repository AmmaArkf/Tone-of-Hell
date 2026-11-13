#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

/// Adds frames to a growing overview. `fill` counts the frames already in its last bucket
/// (0 for a new overview). Returns true when a sample reached full scale.
bool extend_overview(Overview& overview, std::size_t& fill, const float* interleaved,
                     std::size_t frames, std::size_t channels);

/// Writes audio to a cache file of raw float samples and builds its overview on the way. The
/// finished file is played from disk in pages of 1 MB (see core::PagedFile).
class CacheWriter {
public:
    CacheWriter() = default;
    ~CacheWriter();
    CacheWriter(const CacheWriter&) = delete;
    CacheWriter& operator=(const CacheWriter&) = delete;

    /// Creates the file (replacing an earlier one that was not finished).
    bool open(const std::filesystem::path& path, std::uint32_t channels,
              std::uint32_t sample_rate);
    bool is_open() const noexcept { return file_ != nullptr; }
    /// Appends interleaved frames. Returns false when nothing is open or the disk refused
    /// them (for example because it is full).
    bool write(const float* interleaved, std::size_t frames);
    std::int64_t frames() const noexcept { return frames_; }
    const Overview& overview() const noexcept { return overview_; }
    bool clipped() const noexcept { return clipped_; }
    bool failed() const noexcept { return failed_; }
    /// Closes the file and opens it for playing. Moves the overview to `overview` (if given).
    /// Returns null on failure. Either way the writer is empty afterwards.
    std::shared_ptr<const core::ClipData> finish(Overview* overview = nullptr,
                                                 std::string* error = nullptr);
    /// Closes and deletes an unfinished file.
    void discard();

private:
    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
    std::uint32_t channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::int64_t frames_ = 0;
    Overview overview_;
    std::size_t fill_ = 0;
    bool clipped_ = false;
    bool failed_ = false;
};

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
    core::FadeShape fade_shape = core::FadeShape::Curve;
    /// How fast the region plays its source (1 = as recorded); see core::Region::rate.
    double rate = 1.0;

    std::int64_t end() const noexcept { return start + length; }
    /// True for a region of notes (played by the track's instrument) instead of audio.
    bool has_notes() const noexcept {
        return source && source->clip && source->clip->notes != nullptr;
    }
    /// The source frames the region plays, from `offset` on.
    std::int64_t source_frames() const noexcept { return core::clip_frames_used(length, rate); }
};

/// What a user interface needs to know about one track.
struct TrackInfo {
    std::string name;  // UTF-8
    /// The picture shown next to the track: empty for none, the name of a built-in picture
    /// (for example "guitar"), or "file:" followed by the path of an image file (UTF-8).
    std::string icon;
    /// The instrument that plays the note regions of this track.
    core::Instrument instrument = core::Instrument::Keys;
    /// A program of the SoundFont that plays the note regions instead (0..127 General MIDI,
    /// core::SoundFont::kDrums for drums), or -1 for `instrument`.
    int program = -1;
    std::vector<RegionInfo> regions;  // in the order they were created
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    bool selected = false;  // see Session::set_play_selected_only()
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

    /// Loads a file onto a new track, as one region at the start of the timeline. The file is
    /// decoded into the cache folder and later played from there in pages of 1 MB, so it does
    /// not have to fit in memory. This call blocks while the file is decoded.
    bool add_track_from_file(const std::filesystem::path& path, std::string* error = nullptr);
    /// Adds a track with an already loaded clip (used for test tones and by tests).
    /// `overview` may hand over a waveform overview that is already built (it is moved from).
    bool add_track(std::string name, std::shared_ptr<const core::ClipData> clip,
                   std::string* error = nullptr, Overview* overview = nullptr);

    /// The folder for the cache files of this session (imported files, recordings, joined
    /// regions), created when it is first needed and removed with the session. Until projects
    /// can be saved it lies in the system's folder for temporary files. Empty if it cannot be
    /// created.
    const std::filesystem::path& cache_folder();
    /// Frames that were wanted from disk before they were loaded (heard as silence).
    std::uint64_t disk_misses() const noexcept { return engine_.disk_misses(); }
    bool remove_track(std::size_t index);
    /// Adds a track without any audio, for example to record onto. Returns its index, or
    /// tracks().size() when the track limit is reached.
    std::size_t add_empty_track(std::string name);
    /// Adds a track with an instrument and one empty note region from `start`, `length`
    /// frames long. Returns its index, or tracks().size() when the track limit is reached.
    std::size_t add_instrument_track(std::string name, core::Instrument instrument,
                                     std::int64_t start, std::int64_t length, int program = -1);
    /// Replaces the notes of a note region. Notes are sorted by start; notes without a length,
    /// outside the region's note list or above pitch 127 are dropped, velocities are limited
    /// to 0..1. Returns false for an audio region.
    bool set_region_notes(std::size_t track, std::size_t region, std::vector<core::Note> notes);
    bool set_track_instrument(std::size_t index, core::Instrument instrument);
    /// Chooses a program of the SoundFont for a track (-1: the built-in instrument).
    bool set_track_program(std::size_t index, int program);
    /// The recorded instruments that programs refer to (also used by exports).
    void set_soundfont(std::shared_ptr<const core::SoundFont> soundfont);
    const std::shared_ptr<const core::SoundFont>& soundfont() const noexcept { return soundfont_; }

    const std::vector<TrackInfo>& tracks() const noexcept { return tracks_; }
    bool set_track_gain(std::size_t index, float linear);
    bool set_track_pan(std::size_t index, float pan);
    bool set_track_mute(std::size_t index, bool mute);
    bool set_track_solo(std::size_t index, bool solo);
    bool set_track_selected(std::size_t index, bool selected);
    /// Selects one track and deselects every other one.
    void select_only_track(std::size_t index);
    void clear_track_selection();
    /// When on and at least one track is selected, only the selected tracks are heard, in
    /// playback and in exports. With nothing selected every track plays.
    void set_play_selected_only(bool enabled);
    bool play_selected_only() const noexcept { return play_selected_only_; }
    /// Whether a track is heard: not muted, not silenced by the solo of another track and not
    /// left out by "play only the selected tracks".
    bool track_audible(std::size_t index) const noexcept;
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
    /// Moves a region to another track, beginning at `start`. It is added at the end of that
    /// track's region list; `moved` receives its new index.
    bool move_region_to_track(std::size_t track, std::size_t region, std::size_t target,
                              std::int64_t start, std::size_t* moved = nullptr);
    /// Moves the beginning of a region to `start`, keeping its end and the audio in place.
    bool trim_region_start(std::size_t track, std::size_t region, std::int64_t start);
    /// Moves the end of a region to `end`, keeping its beginning in place.
    bool trim_region_end(std::size_t track, std::size_t region, std::int64_t end);
    bool set_region_fades(std::size_t track, std::size_t region, std::int64_t fade_in,
                          std::int64_t fade_out);
    bool set_region_fade_shape(std::size_t track, std::size_t region, core::FadeShape shape);
    /// Plays a region faster or slower (0.25 to 4), like a tape: it keeps the same audio, gets
    /// shorter or longer on the timeline, and the pitch follows. Its fades are scaled along.
    bool set_region_speed(std::size_t track, std::size_t region, double speed);
    /// Cuts a region in two at a timeline frame. The second part is added at the end of the
    /// track's region list. Returns false when `frame` is not inside the region.
    bool split_region(std::size_t track, std::size_t region, std::int64_t frame);
    bool remove_region(std::size_t track, std::size_t region);
    /// Glues regions of one track into a single new region. The audio from the first start to
    /// the last end becomes a new source: overlaps are mixed, gaps become silence and the fades
    /// inside are applied, while the outer fade-in and fade-out stay adjustable. The new region
    /// takes the place of the lowest index; `joined` receives that index. Returns false for
    /// fewer than two regions, an index that does not exist, or more than an hour of audio.
    bool join_regions(std::size_t track, std::vector<std::size_t> regions,
                      std::size_t* joined = nullptr);

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
    const Overview& take_overview() const noexcept { return take_writer_.overview(); }
    /// True when the running take reached full scale: the input was overloaded.
    bool take_clipped() const noexcept { return take_writer_.clipped(); }

    // ---- Project -------------------------------------------------------------------------

    /// Saves the project into one file (usually "*.tohp"): a zip archive with a description
    /// of the project and the audio of every region. Returns false on failure (`error`, if
    /// given, says why); the file that was there before stays as it was.
    bool save_project(const std::filesystem::path& file, std::string* error = nullptr);
    /// Replaces everything in the session with a project saved by save_project(). On failure
    /// nothing changes.
    bool open_project(const std::filesystem::path& file, std::string* error = nullptr);
    /// Empties the session: no tracks, the playhead at the start, no project file.
    void new_project();
    /// The file the project was last saved to or opened from; empty for a new project.
    const std::filesystem::path& project_file() const noexcept { return project_file_; }
    /// True when the project differs from what was last saved, opened or started.
    bool modified() const;

    // ---- Autosave ------------------------------------------------------------------------

    /// True when there are unsaved changes that the last save_recovery() did not cover.
    bool needs_recovery_save() const;
    /// Writes the project to a recovery file (for autosave) without changing project_file()
    /// or modified(). The recovery file also remembers the project file.
    bool save_recovery(const std::filesystem::path& file, std::string* error = nullptr);
    /// Opens a recovery file: the project comes back as it was, with its project file, and
    /// counts as modified until it is saved.
    bool restore_recovery(const std::filesystem::path& file, std::string* error = nullptr);

    // ---- Undo ----------------------------------------------------------------------------
    // Edits of tracks and regions are recorded as steps. A user interface calls
    // commit_edits() whenever a gesture has ended (for example every frame while no mouse
    // button is held), so a whole drag becomes one step. Mute, solo, the track selection, the
    // loop and the metronome are not steps of their own.

    /// Records the edits since the last call as one step (does nothing without edits).
    void commit_edits();
    bool can_undo() const noexcept { return !undo_.empty(); }
    bool can_redo() const noexcept { return !redo_.empty(); }
    /// What undo() and redo() would take back or bring back, in English (for example
    /// "Remove a track"); pass it through tr() to show it.
    std::string undo_label() const;
    std::string redo_label() const;
    /// Return to the state before the last step / after the step that was undone. Both do
    /// nothing (and return false) while recording or when there is nothing to do.
    bool undo();
    bool redo();
    /// Forgets every step (done by open_project() and new_project()).
    void clear_history();

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
    /// Playback speed (0.25 to 4, 1 = normal); the pitch changes with it, like on a tape.
    double speed() const noexcept { return engine_.speed(); }
    bool set_speed(double speed);
    /// 5 % faster or slower, rounded to whole percents.
    void faster();
    void slower();
    /// What a "reverse" button does: changes the direction and, when reverse_starts_playback()
    /// is on, makes sure playback is running, starting from the current position.
    void toggle_reverse();
    /// Whether toggle_reverse() starts playback when it is stopped or paused. On by default.
    bool reverse_starts_playback() const noexcept { return reverse_starts_playback_; }
    void set_reverse_starts_playback(bool enabled) noexcept { reverse_starts_playback_ = enabled; }
    /// Whether reverse switches itself off when backwards playback reaches the start of the
    /// timeline and stops there (checked in update()). On by default.
    bool reverse_off_at_start() const noexcept { return reverse_off_at_start_; }
    void set_reverse_off_at_start(bool enabled) noexcept { reverse_off_at_start_ = enabled; }
    const MetronomeSettings& metronome() const noexcept { return metronome_; }
    /// Returns false (and changes nothing) when a value is out of range.
    bool set_metronome(const MetronomeSettings& settings);
    /// With the metronome on, playback goes on past the end of the project (see
    /// core::Engine::set_metronome_endless). On by default.
    bool metronome_endless() const noexcept { return engine_.metronome_endless(); }
    void set_metronome_endless(bool endless) noexcept { engine_.set_metronome_endless(endless); }

    bool is_playing() const noexcept { return engine_.is_playing(); }
    std::int64_t position_frames() const noexcept { return engine_.position_frames(); }
    /// End of the last region on any track, in frames.
    std::int64_t length_frames() const noexcept { return length_frames_; }
    std::uint32_t sample_rate() const noexcept { return config_.sample_rate; }

    /// Direct access for tests that render audio without a sound device.
    core::Engine& engine() noexcept { return engine_; }

private:
    RegionInfo* find_region(std::size_t track, std::size_t region) noexcept;
    /// Whether the engine should silence a track: muted, or left out by the selection.
    bool silenced(const TrackInfo& track) const noexcept;
    /// Sends the silenced state of every track to the engine.
    void sync_mutes();
    void sync_track_regions(std::size_t index);
    /// Empties one track slot of the engine.
    void clear_engine_slot(std::size_t slot);
    /// Writes the project archive. `head` adds lines to the description; `audio_level` is the
    /// zip compression level of the audio.
    bool write_project(const std::filesystem::path& file, std::string* error,
                       const std::string& head, int audio_level);
    /// Every source that a region uses, in the order of the tracks.
    std::vector<std::shared_ptr<const Source>> used_sources() const;
    /// The text that describes the project, with `files[i]` naming the audio of `sources[i]`.
    std::string project_text(const std::vector<std::shared_ptr<const Source>>& sources,
                             const std::vector<std::string>& files,
                             const std::map<std::string, std::string>* icons = nullptr) const;
    /// The project as it is now, for comparing (see modified()).
    std::string state_text() const;

    /// The tracks as they were at one moment, for undo and redo.
    struct Snapshot {
        std::vector<TrackInfo> tracks;
        std::string text;   // edit_text() of these tracks
        std::string label;  // what the step that follows (or was undone) did
    };
    /// Everything about the tracks that counts as an edit (not mute, solo or selection).
    std::string edit_text() const;
    /// Puts the tracks of a snapshot back and tells the engine.
    void restore_tracks(const std::vector<TrackInfo>& tracks);
    void sync_engine_tracks(std::size_t first);
    void update_length();
    void drain_recording();
    /// Renders the whole project offline and hands it to `sink` block by block. Stops and
    /// returns false as soon as `sink` returns false.
    bool render_project(const std::function<bool(const float*, std::uint32_t)>& sink) const;
    void finish_take();

    // ---- Playing from disk ---------------------------------------------------------------
    // A loader thread keeps the pages around the playhead in memory for every region whose
    // clip is on disk. It works on a copy of those regions, sent by update_streaming().

    struct StreamRegion {
        std::shared_ptr<core::PagedFile> file;
        std::int64_t start = 0;
        std::int64_t offset = 0;
        std::int64_t length = 0;
        double rate = 1.0;
    };
    std::filesystem::path new_cache_file(const char* kind, const char* extension = ".f32");
    void update_streaming();
    /// Loads the pages around `position` now, on the calling thread.
    void prefetch_now(std::int64_t position);
    void prefetch_regions(const std::vector<StreamRegion>& regions, std::int64_t position,
                          bool reverse, bool loop);
    void loader_main();

    audio::DeviceConfig config_;
    core::Engine engine_;
    audio::Device device_;
    std::vector<TrackInfo> tracks_;
    float master_gain_ = 1.0f;
    bool loop_ = false;
    bool reverse_ = false;
    bool snap_ = true;
    bool play_selected_only_ = false;
    bool reverse_starts_playback_ = true;
    bool reverse_off_at_start_ = true;
    bool was_playing_backwards_ = false;  // seen by the last update()
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
    CacheWriter take_writer_;  // the input recorded so far, written to the cache folder
    std::vector<float> drain_buffer_;
    int take_count_ = 0;

    std::filesystem::path cache_folder_;
    int cache_count_ = 0;

    std::shared_ptr<const core::SoundFont> soundfont_;
    std::filesystem::path project_file_;
    std::string saved_state_;  // state_text() when the project was saved, opened or started
    std::string autosaved_state_;               // state_text() at the last recovery save
    std::filesystem::path recovered_original_;  // "original_file" of the last opened project

    std::vector<Snapshot> undo_;  // oldest first
    std::vector<Snapshot> redo_;  // the most recently undone step last
    Snapshot committed_;          // the tracks as of the last commit_edits()

    std::mutex loader_mutex_;  // guards the loader_ members below
    std::condition_variable loader_cv_;
    std::vector<StreamRegion> loader_regions_;
    bool loader_reverse_ = false;
    bool loader_loop_ = false;
    bool loader_wake_ = false;
    bool loader_stop_ = false;
    std::int64_t loader_hint_ = -1;  // a position just jumped to, not yet seen by the engine
    std::mutex prefetch_mutex_;      // one thread at a time fills the pages
    std::mutex loader_busy_;         // held by the loader while it holds copies of regions
    std::thread loader_;             // last: started when everything else exists
};

}  // namespace toh::app
