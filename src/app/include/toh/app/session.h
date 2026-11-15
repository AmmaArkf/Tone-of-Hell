#pragma once

#include <algorithm>
#include <array>
#include <atomic>
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
#include "toh/plugins/clap_plugin.h"

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

/// One insert effect of a track: for now a CLAP plugin.
struct EffectSlot {
    std::shared_ptr<plugins::Plugin> plugin;
    std::string name;  // UTF-8
    bool enabled = true;
};

/// The automation of one parameter of a plugin of a track: its value (in the units of the
/// parameter) along the timeline.
struct ParamCurveInfo {
    std::shared_ptr<plugins::Plugin> plugin;
    std::uint32_t param = 0;
    std::vector<core::CurvePoint> points;  // sorted by frame, not empty
};

/// Changes to the sound of an audio region (see Session::edit_region_sound).
enum class SoundEdit {
    Normalize,  ///< as loud as possible without clipping
    Louder,     ///< +3 dB
    Quieter,    ///< -3 dB
    Reverse,    ///< backwards
    Invert,     ///< the polarity turned over
    Silence,    ///< nothing
    Stretch,    ///< longer or shorter by `amount` (2 = twice as long), the pitch stays
    Pitch,      ///< higher or lower by `amount` semitones, the length stays
};

/// A named place on the timeline, to find a part of the song again (verse, chorus...).
struct Marker {
    std::int64_t frame = 0;
    std::string name;
};

/// A group of tracks. Their sound is summed on a bus of the engine with its own fader and mute
/// before it reaches the master. A collapsed group is a stack: user interfaces show its tracks
/// as one row.
struct GroupInfo {
    std::string name;  // UTF-8
    float gain = 1.0f;
    bool mute = false;
    bool collapsed = false;
};

/// A clip of the clip grid: a region kept in a slot of a track, in one scene (a row of the grid).
struct ClipSlot {
    int scene = 0;
    RegionInfo region;  // its start is not used
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
    /// A sound that plays the note regions of this track instead (a sampler), or null.
    std::shared_ptr<const core::SamplerSound> sampler;
    /// Insert effects, in the order the sound passes them (before the fader).
    std::vector<EffectSlot> effects;
    /// An instrument plugin that plays the note regions (before everything else), if any.
    EffectSlot plugin_instrument;
    std::vector<RegionInfo> regions;  // in the order they were created
    /// Volume automation: the gain (0 to 2) along the timeline, multiplied with `gain`. Sorted
    /// by frame; empty means none.
    std::vector<core::CurvePoint> volume_curve;
    /// Pan automation: -1 (left) to 1 (right) along the timeline. While it has points they
    /// decide the pan instead of `pan`.
    std::vector<core::CurvePoint> pan_curve;
    /// Automation of plugin parameters. A curve of a plugin that is no longer on the track
    /// is kept (an undo may bring the plugin back) but does nothing.
    std::vector<ParamCurveInfo> param_curves;
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    bool selected = false;  // see Session::set_play_selected_only()
    /// The group the track belongs to (an index of Session::groups()), or -1 for none.
    int group = -1;
    /// Sidechain ducking: the track whose level makes this one quieter (-1: none), and how.
    int duck_key = -1;
    float duck_threshold_db = -30.0f;
    float duck_amount_db = 12.0f;
    float duck_release_ms = 150.0f;
    /// The clips of this track in the clip grid, one per scene at most.
    std::vector<ClipSlot> clips;
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
    /// Puts a sound that is already in memory (a stock sound of the library, for example) onto
    /// a track without notes, as a new region from `start`. `overview` may hand over a
    /// waveform overview that is already built.
    bool add_clip_region(std::size_t track, std::int64_t start, std::string name,
                         std::shared_ptr<const core::ClipData> clip, std::string* error = nullptr,
                         Overview* overview = nullptr);
    /// Decodes an audio file (like add_track_from_file) onto a track without notes, from `start`.
    bool add_file_region(std::size_t track, const std::filesystem::path& path, std::int64_t start,
                         std::string* error = nullptr);
    /// Adds a note region `length` frames long from `start` with `notes` (their times count
    /// from the region's beginning) on a track without recorded sound.
    bool add_note_region(std::size_t track, std::int64_t start, std::int64_t length, std::string name,
                         std::vector<core::Note> notes);
    /// Replaces the notes of a note region. Notes are sorted by start; notes without a length,
    /// outside the region's note list or above pitch 127 are dropped, velocities are limited
    /// to 0..1. Returns false for an audio region.
    bool set_region_notes(std::size_t track, std::size_t region, std::vector<core::Note> notes);
    bool set_track_instrument(std::size_t index, core::Instrument instrument);
    /// True for a track that holds note regions (an instrument track).
    bool is_instrument_track(std::size_t index) const noexcept;

    // ---- Playing notes live --------------------------------------------------------------

    /// The track whose sound keyboard notes use (kNewTrack for the simple keys).
    void set_live_track(std::size_t index) noexcept { live_track_ = index; }
    std::size_t live_track() const noexcept { return live_track_; }
    /// Plays a note from a keyboard right away. While notes are recorded (start_recording on
    /// an instrument track) it is also written down.
    bool note_on(int pitch, float velocity);
    void note_off(int pitch);

    // ---- The sampler ---------------------------------------------------------------------

    /// Makes a sound file the instrument of a track: its notes play the sound higher or lower.
    /// The note of the sound is found from its spectrum (up to the first minute is used).
    bool set_track_sampler(std::size_t index, const std::filesystem::path& file,
                           std::string* error = nullptr);
    bool set_track_sampler_sound(std::size_t index, std::shared_ptr<const core::SamplerSound> sound);
    /// The note at which the sound plays as recorded (with a fraction for fine tuning).
    bool set_track_sampler_root(std::size_t index, double root);
    bool clear_track_sampler(std::size_t index);

    // ---- Effects -------------------------------------------------------------------------

    static constexpr std::size_t kMaxEffects = 16;  // per track
    /// Adds the CLAP plugin `id` of a .clap file at the end of a track's effects.
    bool add_clap_effect(std::size_t track, const std::filesystem::path& file, const std::string& id,
                         std::string* error = nullptr);
    /// Adds a plugin of any format at the end of a track's effects.
    bool add_effect(std::size_t track, const plugins::PluginInfo& plugin, std::string* error = nullptr);
    bool remove_effect(std::size_t track, std::size_t slot);
    bool move_effect(std::size_t track, std::size_t slot, std::size_t to);
    bool set_effect_enabled(std::size_t track, std::size_t slot, bool enabled);
    /// Makes the CLAP plugin `id` of a .clap file the instrument of a track.
    bool set_track_plugin_instrument(std::size_t track, const std::filesystem::path& file,
                                     const std::string& id, std::string* error = nullptr);
    bool set_track_plugin_instrument(std::size_t track, const plugins::PluginInfo& plugin,
                                     std::string* error = nullptr);
    bool clear_track_plugin_instrument(std::size_t track);
    /// What could not be restored when the last project was opened (missing plugins).
    const std::vector<std::string>& open_warnings() const noexcept { return open_warnings_; }

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
    /// Replaces the volume automation of a track (see TrackInfo::volume_curve). The points
    /// are sorted and their values limited to 0..2.
    bool set_track_volume_curve(std::size_t index, std::vector<core::CurvePoint> points);
    /// Replaces the pan automation of a track (see TrackInfo::pan_curve); values limited to -1..1.
    bool set_track_pan_curve(std::size_t index, std::vector<core::CurvePoint> points);
    /// Replaces the automation of one parameter of a plugin of a track; no points removes it.
    /// The values are limited to the range of the parameter.
    bool set_param_curve(std::size_t index, const std::shared_ptr<plugins::Plugin>& plugin,
                         std::uint32_t param, std::vector<core::CurvePoint> points);
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

    // ---- Groups and stacks ---------------------------------------------------------------

    static constexpr std::size_t kMaxGroups = core::Engine::kMaxBuses;
    const std::vector<GroupInfo>& groups() const noexcept { return groups_; }
    /// Makes a group of tracks (they leave the groups they were in). Returns its index, or
    /// groups().size() when there are kMaxGroups groups already. An empty name gives "Group N".
    std::size_t add_group(std::string name, const std::vector<std::size_t>& tracks);
    /// Removes a group; its tracks go straight to the master again.
    bool remove_group(std::size_t group);
    /// Puts a track into a group, or into none with -1.
    bool set_track_group(std::size_t track, int group);
    bool set_group_name(std::size_t group, const std::string& name);
    /// The fader of a group (0 to 8, 1 = unchanged).
    bool set_group_gain(std::size_t group, float linear);
    bool set_group_mute(std::size_t group, bool mute);
    /// Folds a group into a stack (one row) or opens it again. Not an edit of its own.
    bool set_group_collapsed(std::size_t group, bool collapsed);
    float group_peak(std::size_t group) const noexcept;
    /// The tracks of a group, in order.
    std::vector<std::size_t> group_tracks(std::size_t group) const;

    // ---- Sidechain -----------------------------------------------------------------------

    /// Sidechain ducking (see core::Engine::set_track_ducking): while the track `key` is loud,
    /// `track` gets quieter. -1 (or the track itself) switches it off. The threshold is limited
    /// to -60..0 dB, the amount to 0..48 dB and the release to 10..2000 ms.
    bool set_track_ducking(std::size_t track, int key, float threshold_db, float amount_db, float release_ms);

    // ---- Clip grid -----------------------------------------------------------------------
    // Every track has a slot in every scene (a row). A launched clip plays in a loop on its
    // track instead of the track's regions on the timeline, until the track goes back to the
    // timeline. While playing, launching and stopping wait for the next bar.

    static constexpr int kMaxScenes = 64;
    int scene_count() const noexcept { return scene_count_; }
    void add_scene() noexcept { scene_count_ = std::min(scene_count_ + 1, kMaxScenes); }
    /// The clip in a slot, or null for an empty slot.
    const RegionInfo* clip_slot(std::size_t track, int scene) const noexcept;
    /// Puts a copy of a region of a track into a slot of that track: `scene`, or the first
    /// empty one for -1. Returns the scene, or -1 when it cannot.
    int put_clip(std::size_t track, std::size_t region, int scene = -1);
    bool remove_clip(std::size_t track, int scene);
    /// Plays a clip in a loop. Starts playback when it is stopped.
    bool launch_clip(std::size_t track, int scene);
    /// Launches the clips of a scene; the tracks without a clip there go back to the timeline.
    void launch_scene(int scene);
    /// The track goes back to its regions on the timeline.
    void stop_clip(std::size_t track);
    /// Every track goes back to the timeline at once.
    void stop_all_clips();
    /// The scene whose clip plays (or is about to) on a track, or -1.
    int playing_clip(std::size_t track) const noexcept;

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

    // ---- Parts of tracks -------------------------------------------------------------------

    /// The timeline frames [begin, end) of the tracks first_track to last_track.
    struct Range {
        std::size_t first_track = 0;
        std::size_t last_track = 0;
        std::int64_t begin = 0;
        std::int64_t end = 0;
        bool empty() const noexcept { return end <= begin || last_track < first_track; }
    };
    /// Regions taken out of a range: one list per track, their starts counted from the
    /// beginning of the range.
    struct Clipboard {
        std::vector<std::vector<RegionInfo>> tracks;
        std::int64_t length = 0;
        bool empty() const noexcept {
            for (const auto& regions : tracks) {
                if (!regions.empty()) {
                    return false;
                }
            }
            return true;
        }
    };
    /// The parts of the regions inside a range. Regions that reach out of it are cut at its
    /// edges; the sources are shared, nothing is copied.
    Clipboard copy_range(const Range& range) const;
    /// Removes what lies inside a range, cutting regions at its edges; `taken` receives it.
    /// The rest stays where it is. Returns false when nothing was inside.
    bool cut_range(const Range& range, Clipboard* taken = nullptr);
    /// Keeps only what lies inside a range on its tracks: the rest of those tracks is removed.
    bool keep_range(const Range& range);
    /// Puts copied regions at `frame`, the first list onto `track` and the next ones onto the
    /// tracks below; tracks are added when there are not enough (kNewTrack: all new).
    bool paste(const Clipboard& clipboard, std::size_t track, std::int64_t frame);
    /// Take comping: what lies inside `range` on its first track (a take) replaces the same
    /// part of `track`. Returns false for an empty range or the same track.
    bool comp_range(const Range& range, std::size_t track);

    // ---- Grid ----------------------------------------------------------------------------

    bool snap() const noexcept { return snap_; }
    void set_snap(bool enabled) noexcept { snap_ = enabled; }
    GridStep grid_step() const noexcept { return grid_step_; }
    void set_grid_step(GridStep step) noexcept { grid_step_ = step; }
    /// Length of one beat and of one bar in frames (fractional), from the metronome settings.
    double beat_frames() const noexcept;
    double bar_frames() const noexcept;

    // ---- Tempo map -------------------------------------------------------------------------
    // The tempo of the metronome settings holds from the start; tempo changes set another
    // tempo from a later frame on. The grid, snapping and the metronome follow them. Regions
    // and notes stay where they are in time.

    const std::vector<core::TempoChange>& tempo_changes() const noexcept { return tempo_changes_; }
    /// Sets the tempo from `frame` on (20 to 400). At frame 0 or earlier it sets the start tempo.
    bool set_tempo_change(std::int64_t frame, double bpm);
    bool remove_tempo_change(std::size_t index);

    /// Plays one track `factor` times as fast (0.5 = half speed): its regions get shorter or
    /// longer from the start of its first region on, and the regions after it move along.
    /// Recorded sound changes its pitch with the speed, like a tape; notes keep their pitch.
    /// The automation of the track follows. Fails when a region would play its sound slower
    /// than 25% or faster than 400%.
    bool change_track_speed(std::size_t index, double factor, std::string* error = nullptr,
                            bool keep_pitch = false);
    /// Changes the sound of an audio region: the region gets a new sound of its own (the old
    /// one stays for undo). The part of the source the region plays is all that is kept.
    bool edit_region_sound(std::size_t track, std::size_t region, SoundEdit edit, double amount = 0.0,
                           std::string* error = nullptr);
    /// Smart tempo: the tempo of the sound of an audio region, from its first two minutes, and
    /// the timeline frame of its first beat. Returns false when no steady beat is found.
    bool detect_region_tempo(std::size_t track, std::size_t region, double* bpm,
                             std::int64_t* first_beat = nullptr) const;
    /// The project takes the tempo of an audio region (its tempo changes are removed) and the
    /// region moves (by less than a bar) so that its beats fall on the bars.
    /// `bpm` other than 0 is used instead of the tempo found (for example twice or half of it,
    /// when the beat was heard at the wrong speed).
    bool adopt_region_tempo(std::size_t track, std::size_t region, std::string* error = nullptr, double bpm = 0.0);

    /// The markers, in order of time. Adding, moving, renaming and removing them are edits
    /// that can be undone, and they are saved with the project.
    const std::vector<Marker>& markers() const noexcept { return markers_; }
    /// Adds a marker and returns its index (frames below 0 are moved to 0).
    std::size_t add_marker(std::int64_t frame, std::string name);
    bool remove_marker(std::size_t index);
    bool rename_marker(std::size_t index, std::string name);
    /// Moves a marker and returns its index in the new order (or `index` when it is invalid).
    std::size_t move_marker(std::size_t index, std::int64_t frame);
    /// The nearest marker before or after `frame` (strictly), or -1 when there is none.
    std::int64_t previous_marker_frame(std::int64_t frame) const noexcept;
    std::int64_t next_marker_frame(std::int64_t frame) const noexcept;
    double bpm_at(std::int64_t frame) const noexcept;
    /// Beats before a timeline frame, and the frame of a beat position (both with fractions).
    double beats_at(std::int64_t frame) const noexcept;
    double frame_at(double beats) const noexcept;
    /// The length of one beat in frames at a timeline frame.
    double beat_frames_at(std::int64_t frame) const noexcept;
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
    /// The size of one audio block in frames (32 to 8192). A bigger block gives the computer
    /// more time and stops crackling, but the sound reacts later. Restarts a running device.
    /// The default latency correction of recordings follows the block size.
    bool set_buffer_frames(std::uint32_t frames, std::string* error = nullptr);
    /// How many processor threads compute the tracks: -1 chooses by the processor (half of
    /// its threads), 1 is the audio thread alone. The helper threads sleep while nothing
    /// plays and are woken only for blocks with more than one track to compute.
    /// Restarts a running device.
    void set_audio_threads(int threads);
    int audio_threads() const noexcept { return audio_threads_; }
    /// The number of threads in use (the audio thread and its helpers).
    unsigned audio_threads_in_use() const noexcept { return engine_.worker_threads() + 1; }
    /// How much of the time for one audio block the computing took, at its recent peak
    /// (1 = all of it; above that the sound breaks up).
    float dsp_load() const noexcept { return dsp_load_.load(std::memory_order_relaxed); }
    /// The blocks that took longer than their own length since the audio started.
    std::uint32_t dsp_overloads() const noexcept { return dsp_overloads_.load(std::memory_order_relaxed); }
    /// Called by the audio device on its thread: renders one block and measures the load.
    void render_audio(const float* in, float* out, std::uint32_t frames) noexcept;
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

    /// Starts playback and records the input onto `track` (or a new track). On an instrument
    /// track it records the notes played with note_on() and note_off() instead, as a new note
    /// region. The take ends
    /// with stop_recording(), or when playback stops, pauses or jumps; it then appears as a
    /// region. Returns false if a recording is already running or no track is available.
    bool start_recording(std::size_t track = kNewTrack);
    /// Ends the take and pauses playback. The region appears after the next update().
    void stop_recording();
    bool is_recording() const noexcept { return recording_ || notes_recording_; }
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
    /// Saves in the background: the project is taken as it is now and another thread writes
    /// the file, so the program goes on meanwhile (big audio takes a while to write). Returns
    /// false while another background save is still running. Edits made meanwhile stay unsaved.
    bool save_project_in_background(const std::filesystem::path& file);
    /// The same for a recovery file (see save_recovery()).
    bool save_recovery_in_background(const std::filesystem::path& file);
    bool saving() const noexcept { return save_running_; }
    struct SaveResult {
        bool done = false;  // false: no background save has finished since the last call
        bool ok = false;
        bool recovery = false;
        std::filesystem::path file;
        std::string error;
    };
    /// How a background save went, once it is over (update() notices that); given out once.
    SaveResult take_save_result();
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

    // ---- MIDI files ----------------------------------------------------------------------

    /// Reads a Standard MIDI File: every channel of every track becomes an instrument track
    /// with its General MIDI program (drums for channel 10), and the project takes the tempo
    /// and time signature of the file.
    bool import_midi(const std::filesystem::path& path, std::string* error = nullptr);
    /// Writes the notes of all note regions to a Standard MIDI File, one track per track of
    /// the project, at the tempo of the project.
    bool export_midi(const std::filesystem::path& path, std::string* error = nullptr) const;

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
    /// The range that playback loops over: [start, end). An end at or before the start loops
    /// the whole project. Saved with the project.
    void set_loop_range(std::int64_t start, std::int64_t end);
    std::int64_t loop_start() const noexcept { return loop_start_; }
    std::int64_t loop_end() const noexcept { return loop_end_; }
    bool has_loop_range() const noexcept { return loop_end_ > loop_start_; }
    /// Punch in and out: a take keeps only what is recorded inside the loop range, and it
    /// replaces what the track had there. Playback can start earlier to hear the way in.
    /// With the loop on, every pass over the range becomes a take of its own: the last one
    /// stays on the track, the earlier ones go to new tracks that are muted.
    bool punch() const noexcept { return punch_; }
    void set_punch(bool enabled) noexcept { punch_ = enabled; }
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
    struct ProjectSnapshot;
    std::shared_ptr<ProjectSnapshot> snapshot_project(const std::string& head, int audio_level) const;
    static bool write_snapshot(const ProjectSnapshot& snapshot, const std::filesystem::path& file,
                               std::string* error);
    bool start_background_save(const std::filesystem::path& file, bool recovery);
    void finish_background_save();
    void wait_for_background_save();
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
        std::vector<core::TempoChange> tempo;  // the tempo changes of the project
        std::vector<Marker> markers;
        std::vector<GroupInfo> groups;
        double bpm = 120.0;                     // and its tempo at the start
    };
    /// The tracks and the tempo as they are now.
    Snapshot capture(std::string text, std::string label) const;
    /// Puts the tempo of a snapshot back.
    void restore_tempo(const Snapshot& snapshot);
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
    void finish_note_take();

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
    int audio_threads_ = -1;
    std::atomic<float> dsp_load_{0.0f};
    std::atomic<std::uint32_t> dsp_overloads_{0};
    core::Engine engine_;
    audio::Device device_;
    std::vector<TrackInfo> tracks_;
    float master_gain_ = 1.0f;
    bool loop_ = false;
    std::int64_t loop_start_ = 0;
    std::int64_t loop_end_ = -1;
    bool punch_ = false;
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
    std::size_t live_track_ = kNewTrack;
    std::string pending_label_;  // the name of the next undo step, if an edit gave one
    std::vector<std::string> open_warnings_;
    std::int64_t sync_token_ = 0;
    std::vector<core::TempoChange> tempo_changes_;  // sorted by frame, all after frame 0
    std::vector<Marker> markers_;                   // sorted by frame
    std::vector<GroupInfo> groups_;                 // group g sums on bus g of the engine
    // What plays on each track: a clip of the grid from `at` on (scene -1: the timeline), and
    // before that what played before.
    struct Launch {
        int scene = -1;
        std::int64_t at = 0;
        int previous = -1;
        std::int64_t previous_at = 0;
    };
    std::vector<Launch> launches_;  // by track; missing entries: the timeline
    int scene_count_ = 8;
    void set_launch(std::size_t track, int scene);
    /// The regions the engine plays on a track: the timeline, or clips of the grid.
    std::vector<core::Region> track_content(std::size_t index) const;
    /// Adds what plays on a track (a clip of `scene` looped from `at`, or with -1 the
    /// timeline) between the frames `from` and `to`, cut at those frames.
    void append_content(std::vector<core::Region>& regions, std::size_t track, int scene, std::int64_t at,
                        std::int64_t from, std::int64_t to) const;
    /// Sends the faders and mutes of the groups to the buses of the engine.
    void sync_groups();
    void sort_markers();
    /// Gives a region a new sound made by `process` from the part of its source it plays.
    /// `length_factor` changes its length on the timeline (for stretching).
    bool rewrite_region_sound(RegionInfo& region, const std::function<void(std::vector<float>&)>& process,
                              double length_factor, std::string* error);
    /// Decodes a file into a new cache file; null on failure.
    std::shared_ptr<const core::ClipData> decode_to_cache(const std::filesystem::path& path,
                                                          Overview* overview, std::string* error);
    std::shared_ptr<const core::TempoMap> tempo_map_;
    void rebuild_tempo();
    // The instrument plugin each live key went to, so that its end goes there too.
    std::array<std::shared_ptr<plugins::Plugin>, 128> live_plugin_keys_{};
    bool notes_recording_ = false;
    bool notes_seen_playing_ = false;
    std::int64_t notes_record_start_ = 0;
    std::array<std::int64_t, 128> open_note_start_{};
    std::array<float, 128> open_note_velocity_{};
    std::vector<core::Note> recorded_notes_;  // with timeline positions
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
    // A save running in the background (see save_project_in_background()).
    std::thread save_thread_;
    bool save_running_ = false;
    std::atomic<bool> save_done_{false};
    bool save_ok_ = false;       // written by the save thread before save_done_
    std::string save_error_;
    std::string save_state_;     // state_text() when the save began
    SaveResult save_result_;
    std::uint64_t project_generation_ = 0;  // counts new and opened projects
    std::uint64_t save_generation_ = 0;
    std::thread loader_;             // last: started when everything else exists
};

}  // namespace toh::app
