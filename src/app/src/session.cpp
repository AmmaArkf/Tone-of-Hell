#include "toh/app/session.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include "toh/audio/clip_loader.h"
#include "toh/core/stretch.h"
#include "toh/audio/mp3_writer.h"
#include "toh/app/midi_file.h"

#include "miniz.h"

namespace toh::app {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void render_callback(void* user_data, const float* in, float* out, std::uint32_t frames) {
    static_cast<Session*>(user_data)->render_audio(in, out, frames);
}

// The regions of a track in the form the engine wants.
std::vector<core::Region> engine_regions(const TrackInfo& track) {
    std::vector<core::Region> regions;
    regions.reserve(track.regions.size());
    for (const RegionInfo& info : track.regions) {
        core::Region region;
        region.clip = info.source->clip;
        region.start = info.start;
        region.offset = info.offset;
        region.length = info.length;
        region.fade_in = info.fade_in;
        region.fade_out = info.fade_out;
        region.fade_shape = info.fade_shape;
        region.rate = info.rate;
        region.instrument = track.instrument;
        region.program = track.program;
        region.sampler = track.sampler;
        regions.push_back(std::move(region));
    }
    return regions;
}

// The plugins of a track for an engine: the effects that exist, the instrument, and how many
// frames those that are on delay the sound.
struct TrackPlugins {
    std::vector<std::shared_ptr<core::AudioEffect>> effects;
    std::shared_ptr<core::AudioEffect> instrument;
    std::int64_t latency = 0;
    std::vector<core::ParamCurve> params;
};

// Reads "frame:value,frame:value" into a curve with its values limited to low..high.
std::vector<core::CurvePoint> parse_curve(const std::string& line, float low, float high) {
    std::vector<core::CurvePoint> points;
    for (const char* cursor = line.c_str(); *cursor != '\0';) {
        char* after = nullptr;
        core::CurvePoint point;
        point.frame = std::max<long long>(std::strtoll(cursor, &after, 10), 0);
        if (after == cursor || *after != ':') {
            break;
        }
        cursor = after + 1;
        point.value = std::clamp(static_cast<float>(std::strtod(cursor, &after)), low, high);
        if (after == cursor) {
            break;
        }
        points.push_back(point);
        cursor = *after == ',' ? after + 1 : after;
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const core::CurvePoint& a, const core::CurvePoint& b) { return a.frame < b.frame; });
    return points;
}

TrackPlugins track_plugins(const TrackInfo& track) {
    TrackPlugins plugins;
    // Parameter automation, for the plugins that are on the track now.
    for (const ParamCurveInfo& curve : track.param_curves) {
        bool present = curve.plugin && curve.plugin == track.plugin_instrument.plugin;
        for (const EffectSlot& slot : track.effects) {
            present = present || (curve.plugin && curve.plugin == slot.plugin);
        }
        if (present && !curve.points.empty()) {
            core::ParamCurve engine_curve;
            engine_curve.effect = curve.plugin;
            engine_curve.id = curve.param;
            engine_curve.points = curve.points;
            plugins.params.push_back(std::move(engine_curve));
        }
    }
    if (track.plugin_instrument.plugin) {
        plugins.instrument = track.plugin_instrument.plugin;
        plugins.latency += track.plugin_instrument.plugin->latency();
    }
    for (const EffectSlot& slot : track.effects) {
        if (slot.plugin) {
            slot.plugin->set_enabled(slot.enabled);
            plugins.effects.push_back(slot.plugin);
            if (slot.enabled) {
                plugins.latency += slot.plugin->latency();
            }
        }
    }
    return plugins;
}

// A source of notes: the notes cleaned up and sorted, `frames` long.
std::shared_ptr<const Source> note_source(std::string name, std::vector<core::Note> notes,
                                          std::int64_t frames, std::uint32_t channels,
                                          std::uint32_t sample_rate) {
    std::vector<core::Note> clean;
    clean.reserve(notes.size());
    for (core::Note note : notes) {
        if (note.length <= 0 || note.pitch > 127 || note.start < 0 || note.start >= frames) {
            continue;
        }
        note.velocity = std::clamp(note.velocity, 0.0f, 1.0f);
        note.length = std::min(note.length, frames - note.start);
        clean.push_back(note);
    }
    std::stable_sort(clean.begin(), clean.end(),
                     [](const core::Note& a, const core::Note& b) { return a.start < b.start; });
    auto data = std::make_shared<core::NoteData>();
    data->notes = std::move(clean);
    data->frames = frames;
    auto clip = std::make_shared<core::ClipData>();
    clip->channels = channels;
    clip->sample_rate = sample_rate;
    clip->notes = std::move(data);
    auto source = std::make_shared<Source>();
    source->name = std::move(name);
    source->clip = std::move(clip);
    return source;
}

// Keeps the fades of a region inside its length.
void limit_fades(RegionInfo& region) {
    region.fade_in = std::clamp<std::int64_t>(region.fade_in, 0, region.length);
    region.fade_out = std::clamp<std::int64_t>(region.fade_out, 0, region.length - region.fade_in);
}

// The part of a region inside the timeline frames [begin, end). A fade stays only on an
// edge of the region that is kept.
bool region_part(const RegionInfo& region, std::int64_t begin, std::int64_t end, RegionInfo& part) {
    const std::int64_t from = std::max(region.start, begin);
    const std::int64_t to = std::min(region.end(), end);
    if (to <= from) {
        return false;
    }
    part = region;
    part.start = from;
    part.offset = region.offset + std::llround(static_cast<double>(from - region.start) * region.rate);
    part.length = to - from;
    if (from != region.start) {
        part.fade_in = 0;
    }
    if (to != region.end()) {
        part.fade_out = 0;
    }
    limit_fades(part);
    return true;
}

// Where the state of a track's instrument plugin is kept inside a project.
std::string instrument_state_entry(std::size_t track) {
    return "plugins/instrument-" + std::to_string(track + 1) + ".bin";
}

// Where the state of an effect is kept inside a project.
std::string effect_state_entry(std::size_t track, std::size_t slot) {
    return "plugins/state-" + std::to_string(track + 1) + "-" + std::to_string(slot + 1) + ".bin";
}

}  // namespace

// ---- Overview and Source -----------------------------------------------------------------

Overview make_overview(const core::ClipData& clip, std::uint32_t frames_per_bucket) {
    Overview overview;
    overview.frames_per_bucket = std::max<std::uint32_t>(frames_per_bucket, 1);

    const std::size_t channels = clip.channels;
    const auto frames = static_cast<std::size_t>(clip.frames());
    if (channels == 0 || frames == 0) {
        return overview;
    }
    if (clip.notes != nullptr) {
        return overview;  // notes have no waveform
    }
    if (clip.disk != nullptr) {
        // Read the file block by block.
        const std::size_t block = 65536;
        std::vector<float> buffer(block * channels);
        std::size_t fill = 0;
        std::int64_t at = 0;
        for (;;) {
            const std::size_t got = core::read_clip_frames(clip, at, block, buffer.data());
            if (got == 0) {
                break;
            }
            extend_overview(overview, fill, buffer.data(), got, channels);
            at += static_cast<std::int64_t>(got);
        }
        return overview;
    }

    const std::size_t bucket = overview.frames_per_bucket;
    const std::size_t count = (frames + bucket - 1) / bucket;
    overview.minimum.resize(count);
    overview.maximum.resize(count);

    for (std::size_t b = 0; b < count; ++b) {
        const std::size_t begin = b * bucket * channels;
        const std::size_t end = std::min(frames, (b + 1) * bucket) * channels;
        float low = clip.samples[begin];
        float high = low;
        for (std::size_t i = begin + 1; i < end; ++i) {
            low = std::min(low, clip.samples[i]);
            high = std::max(high, clip.samples[i]);
        }
        overview.minimum[b] = low;
        overview.maximum[b] = high;
    }
    return overview;
}

bool Overview::range(std::int64_t first_frame, std::int64_t last_frame, float& low,
                     float& high) const noexcept {
    if (minimum.empty() || last_frame < first_frame || last_frame < 0) {
        return false;
    }
    const auto bucket = static_cast<std::int64_t>(frames_per_bucket);
    const auto count = static_cast<std::int64_t>(minimum.size());
    const std::int64_t first = std::max<std::int64_t>(first_frame, 0) / bucket;
    const std::int64_t last = std::min(last_frame / bucket, count - 1);
    if (first > last) {
        return false;
    }
    low = minimum[static_cast<std::size_t>(first)];
    high = maximum[static_cast<std::size_t>(first)];
    for (std::int64_t b = first + 1; b <= last; ++b) {
        low = std::min(low, minimum[static_cast<std::size_t>(b)]);
        high = std::max(high, maximum[static_cast<std::size_t>(b)]);
    }
    return true;
}

bool Source::range(std::int64_t first_frame, std::int64_t last_frame, float& low,
                   float& high) const noexcept {
    if (!clip || clip->notes != nullptr || last_frame < first_frame) {
        return false;
    }
    // Long ranges: the overview is accurate enough and much faster.
    if (last_frame - first_frame >= 4 * static_cast<std::int64_t>(overview.frames_per_bucket)) {
        return overview.range(first_frame, last_frame, low, high);
    }
    // Short ranges (zoomed in): look at the samples themselves.
    const std::int64_t frames = clip->frames();
    const std::int64_t first = std::max<std::int64_t>(first_frame, 0);
    const std::int64_t last = std::min(last_frame, frames - 1);
    if (first > last) {
        return false;
    }
    const std::size_t channels = clip->channels;
    if (clip->disk != nullptr) {
        thread_local std::vector<float> buffer;
        const auto count = static_cast<std::size_t>(last - first + 1);
        buffer.resize(count * channels);
        const std::size_t got = core::read_clip_frames(*clip, first, count, buffer.data());
        if (got == 0) {
            return false;
        }
        low = buffer[0];
        high = low;
        for (std::size_t i = 1; i < got * channels; ++i) {
            low = std::min(low, buffer[i]);
            high = std::max(high, buffer[i]);
        }
        return true;
    }
    const std::size_t begin = static_cast<std::size_t>(first) * channels;
    const std::size_t end = static_cast<std::size_t>(last + 1) * channels;
    low = clip->samples[begin];
    high = low;
    for (std::size_t i = begin + 1; i < end; ++i) {
        low = std::min(low, clip->samples[i]);
        high = std::max(high, clip->samples[i]);
    }
    return true;
}

// ---- Session -----------------------------------------------------------------------------

Session::Session(const audio::DeviceConfig& config)
    : config_(config),
      engine_(config.sample_rate, config.channels),
      // A first guess for the way through the sound card: one buffer out and one buffer in.
      record_offset_(2 * static_cast<std::int64_t>(config.buffer_frames)) {
    set_audio_threads(-1);
    saved_state_ = state_text();  // an empty project counts as saved
    clear_history();
    loader_ = std::thread([this] { loader_main(); });
}

Session::~Session() {
    if (save_thread_.joinable()) {
        save_thread_.join();  // a background save finishes its file
    }
    stop_audio();  // the audio callback must not run while the engine is destroyed
    {
        const std::lock_guard<std::mutex> lock(loader_mutex_);
        loader_stop_ = true;
    }
    loader_cv_.notify_one();
    if (loader_.joinable()) {
        loader_.join();
    }

    // Let go of every cache file, then remove the cache folder.
    take_writer_.discard();
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        engine_.set_track_regions(static_cast<std::uint32_t>(i), {});
    }
    engine_.render(nullptr, 0);
    engine_.collect_retired();
    tracks_.clear();
    loader_regions_.clear();
    if (!cache_folder_.empty()) {
        std::error_code code;
        std::filesystem::remove_all(cache_folder_, code);
    }
}

// ---- Cache files -------------------------------------------------------------------------

const std::filesystem::path& Session::cache_folder() {
    if (!cache_folder_.empty()) {
        return cache_folder_;
    }
    std::error_code code;
    const std::filesystem::path base = std::filesystem::temp_directory_path(code) / "Tone of Hell";
    if (code) {
        return cache_folder_;
    }
    // Folders left behind by sessions that could not clean up (a crash) go after a week.
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
    for (std::filesystem::directory_iterator it(base, code), end; !code && it != end;
         it.increment(code)) {
        const std::string name = it->path().filename().u8string();
        std::error_code time_code;
        if (name.rfind("session-", 0) == 0 && it->last_write_time(time_code) < old && !time_code) {
            std::error_code remove_code;
            std::filesystem::remove_all(it->path(), remove_code);
        }
    }
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate =
            base / ("session-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        std::error_code create_code;
        if (std::filesystem::create_directories(candidate, create_code) && !create_code) {
            cache_folder_ = candidate;
            break;
        }
    }
    return cache_folder_;
}

std::filesystem::path Session::new_cache_file(const char* kind, const char* extension) {
    const std::filesystem::path& folder = cache_folder();
    if (folder.empty()) {
        return {};
    }
    return folder / (std::string(kind) + "-" + std::to_string(++cache_count_) + extension);
}

// ---- Playing from disk -------------------------------------------------------------------

void Session::update_streaming() {
    std::vector<StreamRegion> regions;
    for (const TrackInfo& track : tracks_) {
        for (const RegionInfo& info : track.regions) {
            if (info.source && info.source->clip && info.source->clip->disk) {
                regions.push_back(
                    {info.source->clip->disk, info.start, info.offset, info.length, info.rate});
            }
        }
    }
    {
        const std::lock_guard<std::mutex> lock(loader_mutex_);
        loader_regions_ = std::move(regions);
        loader_reverse_ = reverse_;
        loader_loop_ = loop_;
        loader_wake_ = true;
    }
    loader_cv_.notify_one();
}

void Session::prefetch_now(std::int64_t position) {
    std::vector<StreamRegion> regions;
    bool reverse = false;
    bool loop = false;
    {
        const std::lock_guard<std::mutex> lock(loader_mutex_);
        regions = loader_regions_;
        reverse = loader_reverse_;
        loop = loader_loop_;
    }
    prefetch_regions(regions, position, reverse, loop);
}

// Which pages every file needs, most urgent first: the page at the playhead, the pages ahead
// of it in the direction of playback, one behind, and the start of the loop.
void Session::prefetch_regions(const std::vector<StreamRegion>& regions, std::int64_t position,
                               bool reverse, bool loop) {
    std::map<core::PagedFile*, std::vector<std::int64_t>> wanted;
    for (const StreamRegion& region : regions) {
        const std::int64_t page_frames = region.file->frames_per_page();
        const std::int64_t step = std::max<std::int64_t>(page_frames / 2, 1);
        const std::int64_t direction = reverse ? -1 : 1;
        std::vector<std::int64_t>& pages = wanted[region.file.get()];
        const auto want = [&](std::int64_t timeline_frame) {
            if (timeline_frame >= region.start && timeline_frame < region.start + region.length) {
                const auto into = static_cast<double>(timeline_frame - region.start);
                pages.push_back(
                    (region.offset + static_cast<std::int64_t>(into * region.rate)) / page_frames);
            }
        };
        for (std::int64_t k = 0; k < 5; ++k) {
            want(position + direction * k * step);
        }
        want(position - direction * step);
        if (loop) {
            for (std::int64_t k = 0; k < 3; ++k) {
                want(k * step);
            }
        }
    }
    const std::lock_guard<std::mutex> lock(prefetch_mutex_);
    for (const auto& entry : wanted) {
        if (!entry.second.empty()) {
            entry.first->prefetch(entry.second);
        }
    }
}

void Session::loader_main() {
    std::unique_lock<std::mutex> lock(loader_mutex_);
    while (!loader_stop_) {
        loader_cv_.wait_for(lock, std::chrono::milliseconds(15),
                            [this] { return loader_stop_ || loader_wake_; });
        if (loader_stop_) {
            break;
        }
        loader_wake_ = false;
        std::unique_lock<std::mutex> busy(loader_busy_);
        {
            // The copy keeps the files open while their pages load; it is gone before `busy`
            // is released.
            const std::vector<StreamRegion> regions = loader_regions_;
            const bool reverse = loader_reverse_;
            const bool loop = loader_loop_;
            const std::int64_t hint = loader_hint_;
            loader_hint_ = -1;
            lock.unlock();
            if (!regions.empty()) {
                prefetch_regions(regions, hint >= 0 ? hint : engine_.position_frames(), reverse,
                                 loop);
            }
        }
        busy.unlock();
        lock.lock();
    }
}

bool Session::start_audio(std::string* error) {
    return device_.start(config_, &render_callback, this, error);
}

void Session::stop_audio() { device_.stop(); }

void Session::update() {
    finish_background_save();
    if (!device_.running()) {
        // No audio thread is running: apply the queued control messages here instead.
        engine_.render(nullptr, 0);
    }
    engine_.collect_retired();

    // Backwards playback that ran into the start of the timeline: forwards again.
    const bool playing = engine_.is_playing();
    if (reverse_off_at_start_ && reverse_ && was_playing_backwards_ && !playing &&
        engine_.position_frames() <= 0) {
        set_reverse(false);
    }
    was_playing_backwards_ = playing && reverse_;

    // Plugins get the time they asked for on the main thread. A track whose plugins changed
    // their latency is compensated anew.
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        bool latency_changed = false;
        const TrackInfo& track = tracks_[t];
        if (track.plugin_instrument.plugin) {
            track.plugin_instrument.plugin->idle();
            latency_changed = track.plugin_instrument.plugin->take_latency_changed();
        }
        for (const EffectSlot& slot : track.effects) {
            if (slot.plugin) {
                slot.plugin->idle();
                latency_changed = slot.plugin->take_latency_changed() || latency_changed;
            }
        }
        if (latency_changed) {
            sync_track_regions(t);
        }
    }
    if (notes_recording_) {
        // Notes are recorded until playback stops (Stop, pause, the end of a loop-less song).
        if (engine_.is_playing()) {
            notes_seen_playing_ = true;
        } else if (notes_seen_playing_) {
            finish_note_take();
        }
    }
    if (recording_) {
        drain_recording();
        if (engine_.is_recording()) {
            record_seen_ = true;
        } else if (record_seen_ || stop_requested_) {
            finish_take();  // the take has ended (stop button, pause, jump ...)
        }
    }
}

// ---- Tracks ------------------------------------------------------------------------------

bool Session::add_track_from_file(const std::filesystem::path& path, std::string* error) {
    if (tracks_.size() >= core::Engine::kMaxTracks) {
        set_error(error, "the track limit is reached");
        return false;
    }
    Overview overview;
    auto clip = decode_to_cache(path, &overview, error);
    if (!clip) {
        return false;
    }
    return add_track(path.filename().u8string(), std::move(clip), error, &overview);
}

bool Session::add_clip_region(std::size_t track, std::int64_t start, std::string name,
                              std::shared_ptr<const core::ClipData> clip, std::string* error,
                              Overview* overview) {
    if (track >= tracks_.size()) {
        set_error(error, "the track does not exist");
        return false;
    }
    if (is_instrument_track(track)) {
        set_error(error, "a track with notes cannot hold recorded sound");
        return false;
    }
    if (!clip || clip->channels != config_.channels || clip->sample_rate != config_.sample_rate ||
        clip->frames() <= 0 || clip->notes != nullptr) {
        set_error(error, "the sound does not match the session format");
        return false;
    }
    auto source = std::make_shared<Source>();
    source->name = std::move(name);
    source->overview = overview != nullptr ? std::move(*overview) : make_overview(*clip);
    source->clip = std::move(clip);
    RegionInfo region;
    region.start = std::max<std::int64_t>(start, 0);
    region.length = source->clip->frames();
    region.source = std::move(source);
    auto& regions = tracks_[track].regions;
    regions.push_back(std::move(region));
    std::stable_sort(regions.begin(), regions.end(),
                     [](const RegionInfo& a, const RegionInfo& b) { return a.start < b.start; });
    sync_track_regions(track);
    update_length();
    return true;
}

bool Session::add_file_region(std::size_t track, const std::filesystem::path& path, std::int64_t start,
                              std::string* error) {
    if (track >= tracks_.size()) {
        set_error(error, "the track does not exist");
        return false;
    }
    Overview overview;
    auto clip = decode_to_cache(path, &overview, error);
    if (!clip) {
        return false;
    }
    return add_clip_region(track, start, path.filename().u8string(), std::move(clip), error, &overview);
}

bool Session::add_note_region(std::size_t track, std::int64_t start, std::int64_t length, std::string name,
                              std::vector<core::Note> notes) {
    if (track >= tracks_.size() || length <= 0) {
        return false;
    }
    auto& regions = tracks_[track].regions;
    for (const RegionInfo& region : regions) {
        if (!region.has_notes()) {
            return false;  // a track with recorded sound
        }
    }
    RegionInfo region;
    region.source = note_source(std::move(name), {}, length, config_.channels, config_.sample_rate);
    region.start = std::max<std::int64_t>(start, 0);
    region.length = length;
    const Source* const added = region.source.get();
    regions.push_back(std::move(region));
    std::stable_sort(regions.begin(), regions.end(),
                     [](const RegionInfo& a, const RegionInfo& b) { return a.start < b.start; });
    for (std::size_t r = 0; r < regions.size(); ++r) {
        if (regions[r].source.get() == added) {
            set_region_notes(track, r, std::move(notes));  // sorts and checks them, and syncs
            break;
        }
    }
    update_length();
    return true;
}

std::shared_ptr<const core::ClipData> Session::decode_to_cache(const std::filesystem::path& path,
                                                               Overview* overview, std::string* error) {
    CacheWriter writer;
    const std::filesystem::path cache = new_cache_file("import");
    if (cache.empty() || !writer.open(cache, config_.channels, config_.sample_rate)) {
        set_error(error, "cannot create a cache file in the folder for temporary files");
        return nullptr;
    }
    const std::int64_t frames = audio::decode_stream(
        path, config_.sample_rate, config_.channels,
        [&writer](const float* data, std::size_t count) { return writer.write(data, count); },
        error);
    if (frames < 0) {
        writer.discard();
        return nullptr;
    }
    if (writer.failed()) {
        writer.discard();
        set_error(error, "cannot write the cache file (is the disk full?)");
        return nullptr;
    }
    if (frames == 0) {
        writer.discard();
        set_error(error, "the file contains no audio");
        return nullptr;
    }
    return writer.finish(overview, error);
}

bool Session::add_track(std::string name, std::shared_ptr<const core::ClipData> clip,
                        std::string* error, Overview* overview) {
    if (tracks_.size() >= core::Engine::kMaxTracks) {
        set_error(error, "the track limit is reached");
        return false;
    }
    if (!clip || clip->channels != config_.channels || clip->sample_rate != config_.sample_rate) {
        set_error(error, "the clip does not match the session format");
        return false;
    }

    TrackInfo track;
    track.name = name;
    if (clip->frames() > 0) {
        auto source = std::make_shared<Source>();
        source->name = std::move(name);
        source->overview = overview != nullptr ? std::move(*overview) : make_overview(*clip);
        source->clip = std::move(clip);

        RegionInfo region;
        region.length = source->clip->frames();
        region.source = std::move(source);
        track.regions.push_back(std::move(region));
    }
    tracks_.push_back(std::move(track));

    sync_engine_tracks(tracks_.size() - 1);
    update_length();
    return true;
}

std::size_t Session::add_empty_track(std::string name) {
    if (tracks_.size() >= core::Engine::kMaxTracks) {
        return tracks_.size();
    }
    TrackInfo track;
    track.name = std::move(name);
    tracks_.push_back(std::move(track));
    sync_engine_tracks(tracks_.size() - 1);
    return tracks_.size() - 1;
}

std::size_t Session::add_instrument_track(std::string name, core::Instrument instrument,
                                          std::int64_t start, std::int64_t length, int program) {
    if (tracks_.size() >= core::Engine::kMaxTracks || length <= 0 || program < -1 ||
        program > core::SoundFont::kDrums) {
        return tracks_.size();
    }
    TrackInfo track;
    track.name = name;
    track.icon = program == core::SoundFont::kDrums ? "drums" : "keys";
    track.instrument = instrument;
    track.program = program;
    RegionInfo region;
    region.source = note_source(std::move(name), {}, length, config_.channels, config_.sample_rate);
    region.start = std::max<std::int64_t>(start, 0);
    region.length = length;
    track.regions.push_back(std::move(region));
    tracks_.push_back(std::move(track));
    sync_engine_tracks(tracks_.size() - 1);
    update_length();
    return tracks_.size() - 1;
}

bool Session::set_region_notes(std::size_t track, std::size_t region, std::vector<core::Note> notes) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr || !info->has_notes()) {
        return false;
    }
    info->source = note_source(info->source->name, std::move(notes), info->source->clip->frames(),
                               config_.channels, config_.sample_rate);
    sync_track_regions(track);
    return true;
}

bool Session::set_track_sampler(std::size_t index, const std::filesystem::path& file,
                                std::string* error) {
    if (index >= tracks_.size()) {
        set_error(error, "the track does not exist");
        return false;
    }
    std::vector<float> mono;
    const std::size_t limit = static_cast<std::size_t>(config_.sample_rate) * 60;
    const std::uint32_t channels = std::max<std::uint32_t>(config_.channels, 1);
    const std::int64_t frames = audio::decode_stream(
        file, config_.sample_rate, channels,
        [&mono, limit, channels](const float* data, std::size_t count) {
            for (std::size_t i = 0; i < count && mono.size() < limit; ++i) {
                float sum = 0.0f;
                for (std::uint32_t c = 0; c < channels; ++c) {
                    sum += data[i * channels + c];
                }
                mono.push_back(sum / static_cast<float>(channels));
            }
            return true;
        },
        error);
    if (frames < 0) {
        return false;
    }
    if (mono.empty()) {
        set_error(error, "the file contains no audio");
        return false;
    }
    return set_track_sampler_sound(
        index, core::make_sampler_sound(file.stem().u8string(), std::move(mono), config_.sample_rate));
}

bool Session::set_track_sampler_sound(std::size_t index,
                                      std::shared_ptr<const core::SamplerSound> sound) {
    if (index >= tracks_.size()) {
        return false;
    }
    tracks_[index].sampler = std::move(sound);
    sync_track_regions(index);
    pending_label_ = "Sampler";
    return true;
}

bool Session::set_track_sampler_root(std::size_t index, double root) {
    if (index >= tracks_.size() || !tracks_[index].sampler || !(root >= 0.0 && root <= 127.0)) {
        return false;
    }
    auto changed = std::make_shared<core::SamplerSound>(*tracks_[index].sampler);
    changed->root = root;
    return set_track_sampler_sound(index, std::move(changed));
}

bool Session::clear_track_sampler(std::size_t index) {
    if (index >= tracks_.size() || !tracks_[index].sampler) {
        return false;
    }
    return set_track_sampler_sound(index, nullptr);
}

bool Session::add_clap_effect(std::size_t track, const std::filesystem::path& file, const std::string& id,
                              std::string* error) {
    plugins::PluginInfo info;
    info.format = "clap";
    info.path = file;
    info.id = id;
    return add_effect(track, info, error);
}

bool Session::add_effect(std::size_t track, const plugins::PluginInfo& info, std::string* error) {
    if (track >= tracks_.size()) {
        set_error(error, "the track does not exist");
        return false;
    }
    if (tracks_[track].effects.size() >= kMaxEffects) {
        set_error(error, "a track has at most " + std::to_string(kMaxEffects) + " effects");
        return false;
    }
    auto plugin = plugins::create_plugin(info.format, info.path, info.id,
                                         static_cast<double>(config_.sample_rate), config_.channels, error);
    if (!plugin) {
        return false;
    }
    EffectSlot slot;
    slot.name = plugin->info().name;
    slot.plugin = std::move(plugin);
    tracks_[track].effects.push_back(std::move(slot));
    sync_track_regions(track);
    pending_label_ = "Add an effect";
    return true;
}

bool Session::remove_effect(std::size_t track, std::size_t slot) {
    if (track >= tracks_.size() || slot >= tracks_[track].effects.size()) {
        return false;
    }
    auto& effects = tracks_[track].effects;
    effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(slot));
    sync_track_regions(track);
    pending_label_ = "Remove an effect";
    return true;
}

bool Session::move_effect(std::size_t track, std::size_t slot, std::size_t to) {
    if (track >= tracks_.size() || slot >= tracks_[track].effects.size() ||
        to >= tracks_[track].effects.size() || slot == to) {
        return false;
    }
    auto& effects = tracks_[track].effects;
    EffectSlot moved = std::move(effects[slot]);
    effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(slot));
    effects.insert(effects.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    sync_track_regions(track);
    pending_label_ = "Move an effect";
    return true;
}

bool Session::set_effect_enabled(std::size_t track, std::size_t slot, bool enabled) {
    if (track >= tracks_.size() || slot >= tracks_[track].effects.size()) {
        return false;
    }
    tracks_[track].effects[slot].enabled = enabled;
    sync_track_regions(track);
    pending_label_ = "Switch an effect on or off";
    return true;
}

bool Session::set_track_plugin_instrument(std::size_t track, const std::filesystem::path& file,
                                          const std::string& id, std::string* error) {
    plugins::PluginInfo info;
    info.format = "clap";
    info.path = file;
    info.id = id;
    return set_track_plugin_instrument(track, info, error);
}

bool Session::set_track_plugin_instrument(std::size_t track, const plugins::PluginInfo& info,
                                          std::string* error) {
    if (track >= tracks_.size()) {
        set_error(error, "the track does not exist");
        return false;
    }
    auto plugin = plugins::create_plugin(info.format, info.path, info.id,
                                         static_cast<double>(config_.sample_rate), config_.channels, error);
    if (!plugin) {
        return false;
    }
    EffectSlot slot;
    slot.name = plugin->info().name;
    slot.plugin = std::move(plugin);
    tracks_[track].plugin_instrument = std::move(slot);
    sync_track_regions(track);
    pending_label_ = "Instrument plugin";
    return true;
}

bool Session::clear_track_plugin_instrument(std::size_t track) {
    if (track >= tracks_.size() || !tracks_[track].plugin_instrument.plugin) {
        return false;
    }
    tracks_[track].plugin_instrument = EffectSlot();
    sync_track_regions(track);
    pending_label_ = "Instrument plugin";
    return true;
}

bool Session::is_instrument_track(std::size_t index) const noexcept {
    if (index >= tracks_.size()) {
        return false;
    }
    for (const RegionInfo& region : tracks_[index].regions) {
        if (region.has_notes()) {
            return true;
        }
    }
    return false;
}

bool Session::note_on(int pitch, float velocity) {
    if (pitch < 0 || pitch > 127) {
        return false;
    }
    int program = -1;
    core::Instrument instrument = core::Instrument::Keys;
    std::shared_ptr<const core::SamplerSound> sampler;
    std::shared_ptr<plugins::Plugin> plugin;
    if (live_track_ < tracks_.size()) {
        program = tracks_[live_track_].program;
        instrument = tracks_[live_track_].instrument;
        sampler = tracks_[live_track_].sampler;
        plugin = tracks_[live_track_].plugin_instrument.plugin;
    }
    auto& held = live_plugin_keys_[static_cast<std::size_t>(pitch)];
    if (held) {
        engine_.live_plugin_note(held, false, pitch, 0.0f);  // the same key again
        held.reset();
    }
    if (plugin) {
        engine_.live_plugin_note(plugin, true, pitch, velocity);
        held = std::move(plugin);
    } else {
        engine_.live_note_on(pitch, velocity, program, instrument, std::move(sampler));
    }
    if (notes_recording_) {
        const auto key = static_cast<std::size_t>(pitch);
        if (open_note_start_[key] >= 0) {
            note_off(pitch);  // the same key again: the earlier note ends here
        }
        open_note_start_[key] = engine_.position_frames();
        open_note_velocity_[key] = std::clamp(velocity, 0.0f, 1.0f);
    }
    return true;
}

void Session::note_off(int pitch) {
    if (pitch < 0 || pitch > 127) {
        return;
    }
    engine_.live_note_off(pitch);
    const auto key = static_cast<std::size_t>(pitch);
    if (live_plugin_keys_[key]) {
        engine_.live_plugin_note(live_plugin_keys_[key], false, pitch, 0.0f);
        live_plugin_keys_[key].reset();
    }
    if (notes_recording_ && open_note_start_[key] >= 0) {
        core::Note note;
        note.start = open_note_start_[key];
        note.length = std::max<std::int64_t>(engine_.position_frames() - note.start,
                                             static_cast<std::int64_t>(config_.sample_rate / 100));
        note.pitch = static_cast<std::uint8_t>(pitch);
        note.velocity = open_note_velocity_[key];
        recorded_notes_.push_back(note);
        open_note_start_[key] = -1;
    }
}

void Session::finish_note_take() {
    for (int pitch = 0; pitch < 128; ++pitch) {
        note_off(pitch);  // keys still held end now (and fall silent)
    }
    notes_recording_ = false;
    if (recorded_notes_.empty() || record_track_ >= tracks_.size()) {
        recorded_notes_.clear();
        return;
    }
    std::int64_t begin = recorded_notes_.front().start;
    std::int64_t end = 0;
    for (const core::Note& note : recorded_notes_) {
        begin = std::min(begin, note.start);
        end = std::max(end, note.start + note.length);
    }
    std::vector<core::Note> notes = recorded_notes_;
    for (core::Note& note : notes) {
        note.start -= begin;
    }
    recorded_notes_.clear();
    // Half a second more, so the last release is heard.
    const std::int64_t length = end - begin + static_cast<std::int64_t>(config_.sample_rate / 2);
    RegionInfo region;
    region.source = note_source(tracks_[record_track_].name, std::move(notes), length,
                                config_.channels, config_.sample_rate);
    region.start = begin;
    region.length = length;
    tracks_[record_track_].regions.push_back(std::move(region));
    sync_track_regions(record_track_);
    update_length();
}

bool Session::set_track_program(std::size_t index, int program) {
    if (index >= tracks_.size() || program < -1 || program > core::SoundFont::kDrums) {
        return false;
    }
    tracks_[index].program = program;
    sync_track_regions(index);
    return true;
}

void Session::set_soundfont(std::shared_ptr<const core::SoundFont> soundfont) {
    soundfont_ = std::move(soundfont);
    engine_.set_soundfont(soundfont_);
}

bool Session::set_track_instrument(std::size_t index, core::Instrument instrument) {
    if (index >= tracks_.size()) {
        return false;
    }
    tracks_[index].instrument = instrument;
    sync_track_regions(index);
    return true;
}

bool Session::remove_track(std::size_t index) {
    if (index >= tracks_.size()) {
        return false;
    }
    tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(index));
    if (recording_ && record_track_ != kNewTrack) {
        if (index == record_track_) {
            record_track_ = kNewTrack;  // its track is gone: the take will be dropped
        } else if (index < record_track_) {
            --record_track_;
        }
    }
    sync_engine_tracks(index);  // every later track moves up one slot
    sync_mutes();               // the removed track may have been the only selected one
    update_length();
    return true;
}

// Sends the regions of one track to the engine.
void Session::sync_track_regions(std::size_t index) {
    std::vector<core::Region> regions;
    regions.reserve(tracks_[index].regions.size());
    for (const RegionInfo& info : tracks_[index].regions) {
        core::Region region;
        region.clip = info.source->clip;
        region.start = info.start;
        region.offset = info.offset;
        region.length = info.length;
        region.fade_in = info.fade_in;
        region.fade_out = info.fade_out;
        region.fade_shape = info.fade_shape;
        region.rate = info.rate;
        region.instrument = tracks_[index].instrument;
        region.program = tracks_[index].program;
        region.sampler = tracks_[index].sampler;
        regions.push_back(std::move(region));
    }
    const TrackPlugins plugins = track_plugins(tracks_[index]);
    engine_.set_track_regions(static_cast<std::uint32_t>(index), regions, plugins.effects,
                              plugins.instrument, plugins.latency, tracks_[index].volume_curve,
                              tracks_[index].pan_curve, plugins.params);
    update_streaming();
}

// Copies tracks_[first..] into the engine's track slots and empties the slot after the last one.
void Session::sync_engine_tracks(std::size_t first) {
    for (std::size_t i = first; i < tracks_.size(); ++i) {
        const TrackInfo& track = tracks_[i];
        const auto slot = static_cast<std::uint32_t>(i);
        sync_track_regions(i);
        engine_.set_track_gain(slot, track.gain);
        engine_.set_track_pan(slot, track.pan);
        engine_.set_track_mute(slot, silenced(track));
        engine_.set_track_solo(slot, track.solo);
        engine_.set_track_output(slot, track.group);
    }
    if (tracks_.size() < core::Engine::kMaxTracks) {
        const auto slot = static_cast<std::uint32_t>(tracks_.size());
        engine_.set_track_regions(slot, {});
        engine_.set_track_gain(slot, 1.0f);
        engine_.set_track_pan(slot, 0.0f);
        engine_.set_track_mute(slot, false);
        engine_.set_track_solo(slot, false);
        engine_.set_track_output(slot, core::Engine::kMasterOutput);
    }
}

void Session::update_length() {
    length_frames_ = 0;
    for (const TrackInfo& track : tracks_) {
        for (const RegionInfo& region : track.regions) {
            length_frames_ = std::max(length_frames_, region.end());
        }
    }
}

bool Session::set_track_gain(std::size_t index, float linear) {
    if (index >= tracks_.size() ||
        !engine_.set_track_gain(static_cast<std::uint32_t>(index), linear)) {
        return false;
    }
    tracks_[index].gain = linear;
    return true;
}

bool Session::set_track_volume_curve(std::size_t index, std::vector<core::CurvePoint> points) {
    if (index >= tracks_.size()) {
        return false;
    }
    for (core::CurvePoint& point : points) {
        point.frame = std::max<std::int64_t>(point.frame, 0);
        point.value = std::clamp(point.value, 0.0f, 2.0f);
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const core::CurvePoint& a, const core::CurvePoint& b) { return a.frame < b.frame; });
    tracks_[index].volume_curve = std::move(points);
    sync_track_regions(index);
    pending_label_ = "Volume automation";
    return true;
}

bool Session::set_track_pan_curve(std::size_t index, std::vector<core::CurvePoint> points) {
    if (index >= tracks_.size()) {
        return false;
    }
    for (core::CurvePoint& point : points) {
        point.frame = std::max<std::int64_t>(point.frame, 0);
        point.value = std::clamp(point.value, -1.0f, 1.0f);
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const core::CurvePoint& a, const core::CurvePoint& b) { return a.frame < b.frame; });
    tracks_[index].pan_curve = std::move(points);
    sync_track_regions(index);
    pending_label_ = "Pan automation";
    return true;
}

bool Session::set_param_curve(std::size_t index, const std::shared_ptr<plugins::Plugin>& plugin,
                              std::uint32_t param, std::vector<core::CurvePoint> points) {
    if (index >= tracks_.size() || !plugin) {
        return false;
    }
    double low = 0.0;
    double high = 1.0;
    for (const plugins::Plugin::Param& info : plugin->params()) {
        if (info.id == param) {
            low = info.min;
            high = info.max;
        }
    }
    for (core::CurvePoint& point : points) {
        point.frame = std::max<std::int64_t>(point.frame, 0);
        point.value = std::clamp(point.value, static_cast<float>(low), static_cast<float>(high));
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const core::CurvePoint& a, const core::CurvePoint& b) { return a.frame < b.frame; });
    auto& curves = tracks_[index].param_curves;
    auto found = std::find_if(curves.begin(), curves.end(), [&](const ParamCurveInfo& curve) {
        return curve.plugin == plugin && curve.param == param;
    });
    if (points.empty()) {
        if (found != curves.end()) {
            curves.erase(found);
        }
    } else if (found != curves.end()) {
        found->points = std::move(points);
    } else {
        curves.push_back(ParamCurveInfo{plugin, param, std::move(points)});
    }
    sync_track_regions(index);
    pending_label_ = "Parameter automation";
    return true;
}

bool Session::set_track_pan(std::size_t index, float pan) {
    if (index >= tracks_.size() || !engine_.set_track_pan(static_cast<std::uint32_t>(index), pan)) {
        return false;
    }
    tracks_[index].pan = pan;
    return true;
}

bool Session::set_track_mute(std::size_t index, bool mute) {
    if (index >= tracks_.size()) {
        return false;
    }
    tracks_[index].mute = mute;
    engine_.set_track_mute(static_cast<std::uint32_t>(index), silenced(tracks_[index]));
    return true;
}

bool Session::set_track_selected(std::size_t index, bool selected) {
    if (index >= tracks_.size()) {
        return false;
    }
    tracks_[index].selected = selected;
    sync_mutes();
    return true;
}

void Session::select_only_track(std::size_t index) {
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        tracks_[i].selected = i == index;
    }
    sync_mutes();
}

void Session::clear_track_selection() {
    for (TrackInfo& track : tracks_) {
        track.selected = false;
    }
    sync_mutes();
}

void Session::set_play_selected_only(bool enabled) {
    play_selected_only_ = enabled;
    sync_mutes();
}

bool Session::silenced(const TrackInfo& track) const noexcept {
    if (track.mute) {
        return true;
    }
    if (!play_selected_only_ || track.selected) {
        return false;
    }
    for (const TrackInfo& other : tracks_) {
        if (other.selected) {
            return true;  // some track is selected, and this one is not
        }
    }
    return false;
}

void Session::sync_mutes() {
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        engine_.set_track_mute(static_cast<std::uint32_t>(i), silenced(tracks_[i]));
    }
}

bool Session::track_audible(std::size_t index) const noexcept {
    if (index >= tracks_.size() || silenced(tracks_[index])) {
        return false;
    }
    if (tracks_[index].solo) {
        return true;
    }
    for (const TrackInfo& other : tracks_) {
        if (other.solo) {
            return false;
        }
    }
    return true;
}

bool Session::set_track_solo(std::size_t index, bool solo) {
    if (index >= tracks_.size() ||
        !engine_.set_track_solo(static_cast<std::uint32_t>(index), solo)) {
        return false;
    }
    tracks_[index].solo = solo;
    return true;
}

bool Session::set_track_name(std::size_t index, const std::string& name) {
    const char* const blanks = " \t\r\n";
    const std::size_t first = name.find_first_not_of(blanks);
    if (index >= tracks_.size() || first == std::string::npos) {
        return false;
    }
    tracks_[index].name = name.substr(first, name.find_last_not_of(blanks) - first + 1);
    return true;
}

bool Session::set_track_icon(std::size_t index, std::string icon) {
    if (index >= tracks_.size()) {
        return false;
    }
    tracks_[index].icon = std::move(icon);
    return true;
}

// ---- Groups --------------------------------------------------------------------------------

namespace {

std::string trimmed(const std::string& text) {
    const char* const blanks = " \t\r\n";
    const std::size_t first = text.find_first_not_of(blanks);
    return first == std::string::npos ? std::string()
                                      : text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

}  // namespace

void Session::sync_groups() {
    for (std::uint32_t bus = 0; bus < core::Engine::kMaxBuses; ++bus) {
        const bool used = bus < groups_.size();
        engine_.set_bus_gain(bus, used ? groups_[bus].gain : 1.0f);
        engine_.set_bus_mute(bus, used && groups_[bus].mute);
    }
}

std::size_t Session::add_group(std::string name, const std::vector<std::size_t>& tracks) {
    if (groups_.size() >= kMaxGroups) {
        return groups_.size();
    }
    GroupInfo group;
    group.name = trimmed(name);
    if (group.name.empty()) {
        group.name = "Group " + std::to_string(groups_.size() + 1);
    }
    groups_.push_back(std::move(group));
    const int index = static_cast<int>(groups_.size() - 1);
    for (const std::size_t track : tracks) {
        if (track < tracks_.size()) {
            tracks_[track].group = index;
            engine_.set_track_output(static_cast<std::uint32_t>(track), index);
        }
    }
    sync_groups();
    pending_label_ = "Make a group";
    return groups_.size() - 1;
}

bool Session::remove_group(std::size_t group) {
    if (group >= groups_.size()) {
        return false;
    }
    groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(group));
    const int removed = static_cast<int>(group);
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        int& member = tracks_[i].group;
        if (member == removed) {
            member = -1;
        } else if (member > removed) {
            --member;
        }
        engine_.set_track_output(static_cast<std::uint32_t>(i), member);
    }
    sync_groups();
    pending_label_ = "Remove a group";
    return true;
}

bool Session::set_track_group(std::size_t track, int group) {
    if (track >= tracks_.size() || group < -1 || group >= static_cast<int>(groups_.size())) {
        return false;
    }
    tracks_[track].group = group;
    engine_.set_track_output(static_cast<std::uint32_t>(track), group);
    pending_label_ = "Change the group of a track";
    return true;
}

bool Session::set_group_name(std::size_t group, const std::string& name) {
    std::string clean = trimmed(name);
    if (group >= groups_.size() || clean.empty()) {
        return false;
    }
    groups_[group].name = std::move(clean);
    pending_label_ = "Rename a group";
    return true;
}

bool Session::set_group_gain(std::size_t group, float linear) {
    if (group >= groups_.size() || !(linear >= 0.0f && linear <= 8.0f)) {
        return false;
    }
    groups_[group].gain = linear;
    engine_.set_bus_gain(static_cast<std::uint32_t>(group), linear);
    pending_label_ = "Group volume";
    return true;
}

bool Session::set_group_mute(std::size_t group, bool mute) {
    if (group >= groups_.size()) {
        return false;
    }
    groups_[group].mute = mute;
    engine_.set_bus_mute(static_cast<std::uint32_t>(group), mute);
    return true;
}

bool Session::set_group_collapsed(std::size_t group, bool collapsed) {
    if (group >= groups_.size()) {
        return false;
    }
    groups_[group].collapsed = collapsed;
    return true;
}

float Session::group_peak(std::size_t group) const noexcept {
    return group < groups_.size() ? engine_.bus_peak(static_cast<std::uint32_t>(group)) : 0.0f;
}

std::vector<std::size_t> Session::group_tracks(std::size_t group) const {
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].group == static_cast<int>(group)) {
            members.push_back(i);
        }
    }
    return members;
}

float Session::track_peak(std::size_t index) const noexcept {
    return index < tracks_.size() ? engine_.track_peak(static_cast<std::uint32_t>(index)) : 0.0f;
}

// ---- Regions -----------------------------------------------------------------------------

RegionInfo* Session::find_region(std::size_t track, std::size_t region) noexcept {
    if (track >= tracks_.size() || region >= tracks_[track].regions.size()) {
        return nullptr;
    }
    return &tracks_[track].regions[region];
}

bool Session::move_region(std::size_t track, std::size_t region, std::int64_t start) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr) {
        return false;
    }
    info->start = std::max<std::int64_t>(start, 0);
    sync_track_regions(track);
    update_length();
    return true;
}

bool Session::move_region_to_track(std::size_t track, std::size_t region, std::size_t target,
                                   std::int64_t start, std::size_t* moved) {
    if (find_region(track, region) == nullptr || target >= tracks_.size()) {
        return false;
    }
    if (target == track) {
        if (moved != nullptr) {
            *moved = region;
        }
        return move_region(track, region, start);
    }
    RegionInfo info = tracks_[track].regions[region];
    info.start = std::max<std::int64_t>(start, 0);
    auto& from = tracks_[track].regions;
    from.erase(from.begin() + static_cast<std::ptrdiff_t>(region));
    tracks_[target].regions.push_back(std::move(info));
    if (moved != nullptr) {
        *moved = tracks_[target].regions.size() - 1;
    }
    sync_track_regions(track);
    sync_track_regions(target);
    update_length();
    return true;
}

bool Session::trim_region_start(std::size_t track, std::size_t region, std::int64_t start) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr) {
        return false;
    }
    // A note region grows to the left as far as wanted: its notes move right inside a longer
    // source, so that they stay where they are on the timeline.
    if (info->has_notes() && start < info->start - info->offset) {
        const std::int64_t wanted = std::max<std::int64_t>(start, 0);
        const std::int64_t extra = (info->start - info->offset) - wanted;
        const std::int64_t frames = info->source->clip->frames() + extra;
        if (extra > 0 && frames <= static_cast<std::int64_t>(config_.sample_rate) * 3600) {
            std::vector<core::Note> notes = info->source->clip->notes->notes;
            for (core::Note& note : notes) {
                note.start += extra;
            }
            info->source = note_source(info->source->name, std::move(notes), frames,
                                       config_.channels, config_.sample_rate);
            info->offset += extra;
        }
    }
    // Not before the start of the source or of the timeline, and at least one frame must stay.
    // The source moves `rate` frames for every frame on the timeline.
    const auto source_before = static_cast<std::int64_t>(static_cast<double>(info->offset) / info->rate);
    const std::int64_t earliest = std::max<std::int64_t>(info->start - source_before, 0);
    const std::int64_t latest = info->end() - 1;
    const std::int64_t new_start = std::clamp(start, earliest, latest);
    const std::int64_t delta = new_start - info->start;
    info->start = new_start;
    info->offset = std::max<std::int64_t>(
        info->offset + std::llround(static_cast<double>(delta) * info->rate), 0);
    info->length -= delta;
    limit_fades(*info);
    sync_track_regions(track);
    update_length();
    return true;
}

bool Session::trim_region_end(std::size_t track, std::size_t region, std::int64_t end) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr) {
        return false;
    }
    // A note region grows to the right as far as wanted (up to an hour): its source gets longer.
    const std::int64_t frames_needed = info->offset + (end - info->start);
    if (info->has_notes() && frames_needed > info->source->clip->frames() &&
        frames_needed <= static_cast<std::int64_t>(config_.sample_rate) * 3600) {
        info->source = note_source(info->source->name, info->source->clip->notes->notes, frames_needed,
                                   config_.channels, config_.sample_rate);
    }
    const auto available = static_cast<std::int64_t>(
        static_cast<double>(info->source->clip->frames() - info->offset) / info->rate);
    info->length = std::clamp<std::int64_t>(end - info->start, 1, std::max<std::int64_t>(available, 1));
    limit_fades(*info);
    sync_track_regions(track);
    update_length();
    return true;
}

bool Session::set_region_fades(std::size_t track, std::size_t region, std::int64_t fade_in,
                               std::int64_t fade_out) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr) {
        return false;
    }
    info->fade_in = fade_in;
    info->fade_out = fade_out;
    limit_fades(*info);
    sync_track_regions(track);
    return true;
}

bool Session::set_region_fade_shape(std::size_t track, std::size_t region, core::FadeShape shape) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr) {
        return false;
    }
    info->fade_shape = shape;
    sync_track_regions(track);
    return true;
}

bool Session::set_region_speed(std::size_t track, std::size_t region, double speed) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr || info->has_notes() ||
        !(speed >= core::Engine::kMinSpeed && speed <= core::Engine::kMaxSpeed)) {
        return false;
    }
    // The same audio at another speed: the length on the timeline changes instead.
    const double audio = static_cast<double>(info->length) * info->rate;
    const std::int64_t available = info->source->clip->frames() - info->offset;
    std::int64_t length = std::max<std::int64_t>(std::llround(audio / speed), 1);
    while (length > 1 && core::clip_frames_used(length, speed) > available) {
        --length;
    }
    const double scale = static_cast<double>(length) / static_cast<double>(info->length);
    info->fade_in = std::llround(static_cast<double>(info->fade_in) * scale);
    info->fade_out = std::llround(static_cast<double>(info->fade_out) * scale);
    info->length = length;
    info->rate = speed;
    limit_fades(*info);
    sync_track_regions(track);
    update_length();
    return true;
}

bool Session::split_region(std::size_t track, std::size_t region, std::int64_t frame) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr || frame <= info->start || frame >= info->end()) {
        return false;
    }
    RegionInfo second = *info;
    second.start = frame;
    second.offset =
        info->offset + std::llround(static_cast<double>(frame - info->start) * info->rate);
    second.length = info->end() - frame;
    second.fade_in = 0;
    limit_fades(second);  // keeps the original fade-out, shortened if needed

    info->length = frame - info->start;
    info->fade_out = 0;
    limit_fades(*info);

    tracks_[track].regions.push_back(std::move(second));  // `info` is invalid from here on
    sync_track_regions(track);
    return true;
}

bool Session::join_regions(std::size_t track, std::vector<std::size_t> regions,
                           std::size_t* joined) {
    if (track >= tracks_.size()) {
        return false;
    }
    std::vector<RegionInfo>& list = tracks_[track].regions;
    std::sort(regions.begin(), regions.end());
    regions.erase(std::unique(regions.begin(), regions.end()), regions.end());
    if (regions.size() < 2 || regions.back() >= list.size()) {
        return false;
    }

    // The span that the new region covers, and the regions that give it its outer fades.
    const RegionInfo* first = &list[regions.front()];
    const RegionInfo* last = first;
    for (const std::size_t index : regions) {
        const RegionInfo& region = list[index];
        if (region.start < first->start) {
            first = &region;
        }
        if (region.end() > last->end()) {
            last = &region;
        }
    }
    const std::int64_t begin = first->start;
    const std::int64_t length = last->end() - begin;
    if (length <= 0 || length > static_cast<std::int64_t>(config_.sample_rate) * 3600) {
        return false;
    }

    // Note regions join into one note list. Audio and notes cannot be joined.
    std::size_t with_notes = 0;
    for (const std::size_t index : regions) {
        if (list[index].has_notes()) {
            ++with_notes;
        }
    }
    if (with_notes != 0 && with_notes != regions.size()) {
        return false;
    }
    if (with_notes != 0) {
        std::vector<core::Note> notes;
        for (const std::size_t index : regions) {
            const RegionInfo& part = list[index];
            const std::int64_t from = part.offset;
            const std::int64_t to = part.offset + part.length;
            for (const core::Note& note : part.source->clip->notes->notes) {
                if (note.start < from || note.start >= to) {
                    continue;
                }
                core::Note moved = note;
                moved.start = part.start - begin + (note.start - from);
                moved.length = std::min(note.length, to - note.start);
                notes.push_back(moved);
            }
        }
        RegionInfo merged;
        merged.source = note_source(first->source->name, std::move(notes), length,
                                    config_.channels, config_.sample_rate);
        merged.start = begin;
        merged.length = length;
        const std::size_t position = regions.front();
        for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(position), std::move(merged));
        if (joined != nullptr) {
            *joined = position;
        }
        sync_track_regions(track);
        update_length();
        return true;
    }

    // The joined audio is mixed block by block into a new cache file.
    const std::size_t channels = config_.channels;
    CacheWriter writer;
    const std::filesystem::path cache = new_cache_file("joined");
    if (cache.empty() || !writer.open(cache, config_.channels, config_.sample_rate)) {
        return false;
    }
    const std::int64_t block = 65536;
    std::vector<float> mix(static_cast<std::size_t>(block) * channels);
    // A region may play its source up to four times as fast: room for that, plus one frame.
    std::vector<float> piece(static_cast<std::size_t>(block * 4 + 2) * channels);
    for (std::int64_t at = begin; at < begin + length; at += block) {
        const std::int64_t count = std::min(block, begin + length - at);
        std::fill(mix.begin(), mix.end(), 0.0f);
        for (const std::size_t index : regions) {
            const RegionInfo& region = list[index];
            const std::int64_t from = std::max(at, region.start);
            const std::int64_t to = std::min(at + count, region.end());
            if (from >= to) {
                continue;
            }
            // The same fade shapes as the engine uses; the outer fades are kept, not applied.
            const std::int64_t fade_in = &region == first ? 0 : region.fade_in;
            const std::int64_t fade_out = &region == last ? 0 : region.fade_out;
            // The source frames this part plays (rate frames per timeline frame), plus one more
            // to blend with.
            const auto source_at = [&region](std::int64_t f) {
                return static_cast<double>(region.offset) + static_cast<double>(f) * region.rate;
            };
            const std::int64_t source_end = region.offset + region.source_frames();
            const auto first_source = static_cast<std::int64_t>(source_at(from - region.start));
            const std::int64_t last_source = std::min(
                static_cast<std::int64_t>(source_at(to - 1 - region.start)) + 1, source_end - 1);
            const std::size_t got = core::read_clip_frames(
                *region.source->clip, first_source,
                static_cast<std::size_t>(std::max<std::int64_t>(last_source - first_source + 1, 1)),
                piece.data());
            if (got == 0) {
                continue;
            }
            for (std::int64_t t = from; t < to; ++t) {
                const std::int64_t f = t - region.start;
                const double position = source_at(f) - static_cast<double>(first_source);
                const auto at_piece = std::min(static_cast<std::size_t>(position), got - 1);
                const auto frac = static_cast<float>(position - static_cast<double>(at_piece));
                const std::size_t next = std::min(at_piece + 1, got - 1);
                const std::size_t k = static_cast<std::size_t>(t - from);
                float gain = 1.0f;
                if (f < fade_in) {
                    gain = core::fade_curve(region.fade_shape, (static_cast<float>(f) + 0.5f) /
                                                                   static_cast<float>(fade_in));
                }
                const std::int64_t remaining = region.length - 1 - f;
                if (remaining < fade_out) {
                    gain *= core::fade_curve(region.fade_shape,
                                             (static_cast<float>(remaining) + 0.5f) /
                                                 static_cast<float>(fade_out));
                }
                float* dst = mix.data() + static_cast<std::size_t>(from - at + static_cast<std::int64_t>(k)) * channels;
                const float* src = piece.data() + at_piece * channels;
                const float* src_next = piece.data() + next * channels;
                for (std::size_t c = 0; c < channels; ++c) {
                    dst[c] += (src[c] + (src_next[c] - src[c]) * frac) * gain;
                }
            }
        }
        if (!writer.write(mix.data(), static_cast<std::size_t>(count))) {
            writer.discard();
            return false;
        }
    }
    Overview overview;
    auto clip = writer.finish(&overview);
    if (!clip) {
        return false;
    }

    auto source = std::make_shared<Source>();
    source->name = first->source->name;
    source->overview = std::move(overview);
    source->clip = std::move(clip);

    RegionInfo merged;
    merged.source = std::move(source);
    merged.start = begin;
    merged.length = length;
    merged.fade_in = first->fade_in;
    merged.fade_out = last->fade_out;
    merged.fade_shape = first->fade_shape;
    limit_fades(merged);

    // `first` and `last` point into the list: they are not used after this.
    const std::size_t position = regions.front();
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(position), std::move(merged));
    if (joined != nullptr) {
        *joined = position;
    }
    sync_track_regions(track);
    update_length();
    return true;
}

Session::Clipboard Session::copy_range(const Range& range) const {
    Clipboard clipboard;
    if (range.empty() || range.first_track >= tracks_.size()) {
        return clipboard;
    }
    const std::size_t last = std::min(range.last_track, tracks_.size() - 1);
    clipboard.length = range.end - range.begin;
    for (std::size_t t = range.first_track; t <= last; ++t) {
        std::vector<RegionInfo> parts;
        for (const RegionInfo& region : tracks_[t].regions) {
            RegionInfo part;
            if (region_part(region, range.begin, range.end, part)) {
                part.start -= range.begin;
                parts.push_back(std::move(part));
            }
        }
        clipboard.tracks.push_back(std::move(parts));
    }
    return clipboard;
}

bool Session::cut_range(const Range& range, Clipboard* taken) {
    if (taken != nullptr) {
        *taken = copy_range(range);
    }
    if (range.empty() || range.first_track >= tracks_.size()) {
        return false;
    }
    const std::size_t last = std::min(range.last_track, tracks_.size() - 1);
    bool changed = false;
    for (std::size_t t = range.first_track; t <= last; ++t) {
        std::vector<RegionInfo> kept;
        bool touched = false;
        for (const RegionInfo& region : tracks_[t].regions) {
            if (region.end() <= range.begin || region.start >= range.end) {
                kept.push_back(region);
                continue;
            }
            touched = true;
            RegionInfo part;
            if (region_part(region, region.start, range.begin, part)) {
                kept.push_back(part);
            }
            if (region_part(region, range.end, region.end(), part)) {
                kept.push_back(part);
            }
        }
        if (touched) {
            tracks_[t].regions = std::move(kept);
            sync_track_regions(t);
            changed = true;
        }
    }
    if (changed) {
        update_length();
        pending_label_ = taken != nullptr ? "Cut" : "Delete the selection";
    }
    return changed;
}

bool Session::keep_range(const Range& range) {
    if (range.empty() || range.first_track >= tracks_.size()) {
        return false;
    }
    const std::size_t last = std::min(range.last_track, tracks_.size() - 1);
    bool changed = false;
    for (std::size_t t = range.first_track; t <= last; ++t) {
        std::vector<RegionInfo> kept;
        bool touched = false;
        for (const RegionInfo& region : tracks_[t].regions) {
            RegionInfo part;
            if (region_part(region, range.begin, range.end, part)) {
                touched = touched || part.start != region.start || part.length != region.length;
                kept.push_back(part);
            } else {
                touched = true;
            }
        }
        if (touched) {
            tracks_[t].regions = std::move(kept);
            sync_track_regions(t);
            changed = true;
        }
    }
    if (changed) {
        update_length();
        pending_label_ = "Keep only the selection";
    }
    return changed;
}

bool Session::paste(const Clipboard& clipboard, std::size_t track, std::int64_t frame) {
    if (clipboard.empty() || frame < 0) {
        return false;
    }
    const std::size_t first = std::min(track, tracks_.size());
    bool pasted = false;
    for (std::size_t i = 0; i < clipboard.tracks.size(); ++i) {
        const std::size_t target = first + i;
        while (target >= tracks_.size()) {
            const std::size_t before = tracks_.size();
            add_empty_track("Pasted");
            if (tracks_.size() == before) {
                break;  // no room for more tracks
            }
        }
        if (target >= tracks_.size()) {
            break;
        }
        if (clipboard.tracks[i].empty()) {
            continue;
        }
        for (RegionInfo region : clipboard.tracks[i]) {
            region.start += frame;
            tracks_[target].regions.push_back(std::move(region));
        }
        sync_track_regions(target);
        pasted = true;
    }
    if (pasted) {
        update_length();
        pending_label_ = "Paste";
    }
    return pasted;
}

bool Session::remove_region(std::size_t track, std::size_t region) {
    if (find_region(track, region) == nullptr) {
        return false;
    }
    auto& regions = tracks_[track].regions;
    regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(region));
    sync_track_regions(track);
    update_length();
    return true;
}

// ---- Grid --------------------------------------------------------------------------------

// ---- Recording ---------------------------------------------------------------------------

bool Session::set_input_enabled(bool enabled, std::string* error) {
    config_.capture = enabled;
    if (!device_.running()) {
        return true;  // takes effect when the audio is started
    }
    device_.stop();
    if (!device_.start(config_, &render_callback, this, error)) {
        return false;
    }
    if (enabled && !device_.has_input()) {
        config_.capture = false;
        set_error(error, "no input device could be opened");
        return false;
    }
    return true;
}

void Session::render_audio(const float* in, float* out, std::uint32_t frames) noexcept {
    const auto begin = std::chrono::steady_clock::now();
    engine_.render(in, out, frames);
    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const double block = static_cast<double>(frames) / static_cast<double>(std::max<std::uint32_t>(config_.sample_rate, 1));
    const float load = block > 0.0 ? static_cast<float>(used / block) : 0.0f;
    // The peak falls back slowly, so a short spike stays visible for a moment.
    const float before = dsp_load_.load(std::memory_order_relaxed);
    dsp_load_.store(std::max(load, before * 0.98f), std::memory_order_relaxed);
    if (load > 1.0f) {
        dsp_overloads_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Session::set_audio_threads(int threads) {
    audio_threads_ = threads;
    unsigned helpers = 0;
    if (threads < 0) {
        // Half of the processor's threads at most, so the rest of the computer stays free.
        const unsigned cores = std::thread::hardware_concurrency();
        helpers = cores >= 4 ? cores / 2 - 1 : 0;
    } else if (threads > 1) {
        helpers = static_cast<unsigned>(threads - 1);
    }
    helpers = std::min(helpers, core::Engine::kMaxWorkers);
    if (helpers == engine_.worker_threads()) {
        return;
    }
    const bool running = device_.running();
    if (running) {
        device_.stop();  // the engine may change its threads only while it is not rendering
    }
    engine_.set_worker_threads(helpers);
    if (running) {
        device_.start(config_, &render_callback, this, nullptr);
    }
}

bool Session::set_buffer_frames(std::uint32_t frames, std::string* error) {
    if (frames < 32 || frames > 8192) {
        set_error(error, "the buffer size must be from 32 to 8192 frames");
        return false;
    }
    if (record_offset_ == 2 * static_cast<std::int64_t>(config_.buffer_frames)) {
        record_offset_ = 2 * static_cast<std::int64_t>(frames);  // the default follows the buffer
    }
    config_.buffer_frames = frames;
    dsp_overloads_.store(0, std::memory_order_relaxed);
    if (!device_.running()) {
        return true;  // takes effect when the audio is started
    }
    device_.stop();
    return device_.start(config_, &render_callback, this, error);
}

bool Session::set_devices(const std::string& output, const std::string& input,
                          std::string* error) {
    config_.output_name = output;
    config_.input_name = input;
    if (!device_.running()) {
        return true;  // takes effect when the audio is started
    }
    device_.stop();
    return device_.start(config_, &render_callback, this, error);
}

bool Session::set_input_gain(float linear) {
    if (!engine_.set_input_gain(linear)) {
        return false;
    }
    input_gain_ = linear;
    return true;
}

void Session::set_monitoring(bool enabled) {
    if (engine_.set_monitoring(enabled, 1.0f)) {
        monitoring_ = enabled;
    }
}

void Session::set_record_offset_frames(std::int64_t frames) noexcept {
    const auto limit = static_cast<std::int64_t>(config_.sample_rate);  // one second either way
    record_offset_ = std::clamp(frames, -limit, limit);
}

std::int64_t Session::recorded_frames() const noexcept {
    if (notes_recording_) {
        return std::max<std::int64_t>(engine_.position_frames() - notes_record_start_, 0);
    }
    return take_writer_.frames();
}

bool Session::start_recording(std::size_t track) {
    if (recording_ || notes_recording_) {
        return false;
    }
    if (is_instrument_track(track)) {
        // Notes, not sound: what is played on a keyboard becomes a note region.
        notes_recording_ = true;
        notes_seen_playing_ = false;
        record_track_ = track;
        recorded_notes_.clear();
        open_note_start_.fill(-1);
        notes_record_start_ = engine_.position_frames();
        play();
        return true;
    }
    // The take goes straight to a cache file. Input left over from an earlier take is thrown
    // away first, while no file is open.
    take_writer_.discard();
    drain_recording();
    const std::filesystem::path cache = new_cache_file("take");
    if (cache.empty() || !take_writer_.open(cache, config_.channels, config_.sample_rate)) {
        return false;
    }
    if (track >= tracks_.size()) {
        if (tracks_.size() >= core::Engine::kMaxTracks) {
            take_writer_.discard();
            return false;
        }
        TrackInfo info;
        info.name = "Recording " + std::to_string(tracks_.size() + 1);
        tracks_.push_back(std::move(info));
        sync_engine_tracks(tracks_.size() - 1);
        track = tracks_.size() - 1;
    }

    record_track_ = track;
    record_seen_ = false;
    stop_requested_ = false;
    recording_ = true;
    engine_.start_recording();
    play();
    return true;
}

void Session::stop_recording() {
    if (notes_recording_) {
        finish_note_take();
        engine_.pause();
        return;
    }
    if (!recording_) {
        return;
    }
    engine_.stop_recording();
    engine_.pause();
    stop_requested_ = true;
}

// Moves the recorded input from the engine's ring buffer to `take_`.
void Session::drain_recording() {
    const std::size_t channels = config_.channels;
    const std::size_t block = 4096;
    drain_buffer_.resize(block * channels);
    for (;;) {
        const std::size_t frames = engine_.read_recorded(drain_buffer_.data(), block);
        if (frames == 0) {
            break;
        }
        take_writer_.write(drain_buffer_.data(), frames);  // ignored when no take is open
    }
}

// ---- Overviews and cache files -----------------------------------------------------------

bool extend_overview(Overview& overview, std::size_t& fill, const float* interleaved,
                     std::size_t frames, std::size_t channels) {
    bool clipped = false;
    const std::size_t bucket = overview.frames_per_bucket;
    for (std::size_t i = 0; i < frames; ++i) {
        const float* frame = interleaved + i * channels;
        float low = frame[0];
        float high = frame[0];
        for (std::size_t c = 1; c < channels; ++c) {
            low = std::min(low, frame[c]);
            high = std::max(high, frame[c]);
        }
        if (high >= 0.999f || low <= -0.999f) {
            clipped = true;
        }
        if (fill == 0) {
            overview.minimum.push_back(low);
            overview.maximum.push_back(high);
        } else {
            overview.minimum.back() = std::min(overview.minimum.back(), low);
            overview.maximum.back() = std::max(overview.maximum.back(), high);
        }
        if (++fill == bucket) {
            fill = 0;
        }
    }
    return clipped;
}

CacheWriter::~CacheWriter() { discard(); }

bool CacheWriter::open(const std::filesystem::path& path, std::uint32_t channels,
                       std::uint32_t sample_rate) {
    discard();
    if (channels == 0) {
        return false;
    }
    file_ = core::open_file(path, "wb");
    if (file_ == nullptr) {
        return false;
    }
    path_ = path;
    channels_ = channels;
    sample_rate_ = sample_rate;
    return true;
}

bool CacheWriter::write(const float* interleaved, std::size_t frames) {
    if (file_ == nullptr || failed_) {
        return false;
    }
    if (frames == 0) {
        return true;
    }
    if (std::fwrite(interleaved, sizeof(float) * channels_, frames, file_) != frames) {
        failed_ = true;
        return false;
    }
    frames_ += static_cast<std::int64_t>(frames);
    if (extend_overview(overview_, fill_, interleaved, frames, channels_)) {
        clipped_ = true;
    }
    return true;
}

std::shared_ptr<const core::ClipData> CacheWriter::finish(Overview* overview, std::string* error) {
    if (file_ == nullptr) {
        set_error(error, "nothing was written");
        return nullptr;
    }
    const bool closed = std::fclose(file_) == 0;
    file_ = nullptr;
    std::shared_ptr<const core::ClipData> result;
    if (!closed || failed_) {
        set_error(error, "cannot write the cache file (is the disk full?)");
        std::error_code code;
        std::filesystem::remove(path_, code);
    } else if (auto disk = core::PagedFile::open(path_, channels_, error)) {
        auto clip = std::make_shared<core::ClipData>();
        clip->channels = channels_;
        clip->sample_rate = sample_rate_;
        disk->set_remove_on_close(true);  // a cache file goes when nothing uses it any more
        clip->disk = std::move(disk);
        result = std::move(clip);
        if (overview != nullptr) {
            *overview = std::move(overview_);
        }
    }
    path_.clear();
    discard();  // resets the rest
    return result;
}

void CacheWriter::discard() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
        std::error_code code;
        std::filesystem::remove(path_, code);
    }
    path_.clear();
    frames_ = 0;
    overview_ = Overview();
    fill_ = 0;
    clipped_ = false;
    failed_ = false;
}

// Turns the finished take into a region on the recorded track.
void Session::finish_take() {
    drain_recording();
    recording_ = false;

    const std::int64_t start = engine_.record_start_frame();
    if (take_writer_.frames() < 1 || start < 0 || record_track_ >= tracks_.size()) {
        take_writer_.discard();
        return;  // nothing was recorded, or the track was removed meanwhile
    }

    Overview overview;
    auto clip = take_writer_.finish(&overview);
    if (!clip) {
        return;
    }

    auto source = std::make_shared<Source>();
    const int take = ++take_count_;
    source->name = "Take " + std::to_string(take);
    source->overview = std::move(overview);
    source->clip = std::move(clip);
    const std::int64_t total = source->clip->frames();

    // The pieces of the take on the timeline: one, or one per pass of a loop over a range.
    struct Piece {
        std::int64_t start;
        std::int64_t offset;
        std::int64_t length;
    };
    std::vector<Piece> pieces;
    const bool ranged = loop_end_ > loop_start_;
    if (loop_ && ranged && start < loop_end_) {
        const std::int64_t pass = loop_end_ - loop_start_;
        std::int64_t offset = std::min(total, loop_end_ - start);
        pieces.push_back(Piece{start, 0, offset});
        while (offset < total) {
            const std::int64_t length = std::min(pass, total - offset);
            pieces.push_back(Piece{loop_start_, offset, length});
            offset += length;
        }
    } else {
        pieces.push_back(Piece{start, 0, total});
    }

    std::vector<RegionInfo> regions;
    for (const Piece& piece : pieces) {
        RegionInfo region;
        region.source = source;
        region.offset = piece.offset;
        region.length = piece.length;
        // Place the take earlier by the time the sound needed to pass through the sound card.
        region.start = piece.start - record_offset_;
        if (region.start < 0) {
            const std::int64_t cut = std::min(-region.start, region.length - 1);
            region.offset += cut;
            region.length -= cut;
            region.start = 0;
        }
        if (punch_ && ranged) {
            // Punch in and out: only what lies inside the range is kept.
            const std::int64_t from = std::max(region.start, loop_start_);
            const std::int64_t to = std::min(region.end(), loop_end_);
            if (to <= from) {
                continue;
            }
            region.offset += from - region.start;
            region.length = to - from;
            region.start = from;
        }
        if (region.length > 0) {
            regions.push_back(std::move(region));
        }
    }
    if (regions.empty()) {
        return;
    }
    if (punch_ && ranged) {
        // The new take replaces what the track had in the range.
        Range range;
        range.first_track = record_track_;
        range.last_track = record_track_;
        range.begin = loop_start_;
        range.end = loop_end_;
        cut_range(range);
    }
    // The last pass stays on the track (a pass cut short by stopping does not count); the
    // others go to new tracks, muted, to choose from.
    std::size_t kept = regions.size() - 1;
    if (ranged) {
        const std::int64_t pass = loop_end_ - loop_start_;
        while (kept > 0 && regions[kept].length * 2 < pass) {
            --kept;
        }
    }
    for (std::size_t k = 0; k < regions.size(); ++k) {
        if (k == kept || tracks_.size() >= core::Engine::kMaxTracks) {
            continue;
        }
        TrackInfo info;
        info.name = tracks_[record_track_].name + " - take " + std::to_string(take) + ", pass " +
                    std::to_string(k + 1);
        info.regions.push_back(regions[k]);
        info.mute = true;
        tracks_.push_back(std::move(info));
        sync_engine_tracks(tracks_.size() - 1);
    }
    tracks_[record_track_].regions.push_back(std::move(regions[kept]));
    std::stable_sort(tracks_[record_track_].regions.begin(), tracks_[record_track_].regions.end(),
                     [](const RegionInfo& a, const RegionInfo& b) { return a.start < b.start; });
    sync_track_regions(record_track_);
    sync_mutes();
    update_length();
}

// ---- Project -----------------------------------------------------------------------------
//
// A project file (*.tohp) is a zip archive: "project.txt" describes the project, one value
// per line, and "audio/audio-N.f32" hold its audio as raw float samples (the format of the
// cache files). When a project is opened its audio is unpacked into the cache folder and
// played from there.

namespace {

constexpr const char* kProjectHeader = "Tone of Hell project";
constexpr const char* kProjectEntry = "project.txt";

// A project file holds one value per line, so line breaks inside names become spaces.
std::string one_line(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return text;
}

// Audio entries are plain names: nothing that could point outside the archive's audio folder.
bool valid_audio_name(const std::string& name) {
    return !name.empty() && name.find_first_of("/\\:") == std::string::npos &&
           name.find("..") == std::string::npos;
}

// A zip archive together with its file, closed in every case.
struct ZipFile {
    mz_zip_archive zip;
    std::FILE* file = nullptr;
    bool reading = false;
    bool writing = false;

    ZipFile() { std::memset(&zip, 0, sizeof(zip)); }
    ~ZipFile() {
        if (reading) {
            mz_zip_reader_end(&zip);
        }
        if (writing) {
            mz_zip_writer_end(&zip);
        }
        if (file != nullptr) {
            std::fclose(file);
        }
    }
    ZipFile(const ZipFile&) = delete;
    ZipFile& operator=(const ZipFile&) = delete;
};

}  // namespace

std::vector<std::shared_ptr<const Source>> Session::used_sources() const {
    std::vector<std::shared_ptr<const Source>> sources;
    for (const TrackInfo& track : tracks_) {
        for (const RegionInfo& region : track.regions) {
            if (std::find(sources.begin(), sources.end(), region.source) == sources.end()) {
                sources.push_back(region.source);
            }
        }
    }
    return sources;
}

std::string Session::project_text(const std::vector<std::shared_ptr<const Source>>& sources,
                                  const std::vector<std::string>& files,
                                  const std::map<std::string, std::string>* icons) const {
    std::map<const Source*, std::size_t> index_of;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        index_of[sources[i].get()] = i;
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(17);
    out << kProjectHeader << "\n";
    out << "version=1\n";
    out << "sample_rate=" << config_.sample_rate << "\n";
    out << "channels=" << config_.channels << "\n";
    out << "loop=" << (loop_ ? 1 : 0) << "\n";
    if (loop_end_ > loop_start_) {
        out << "loop_range=" << loop_start_ << ":" << loop_end_ << "\n";
    }
    out << "metronome=" << (metronome_.enabled ? 1 : 0) << "\n";
    out << "bpm=" << metronome_.bpm << "\n";
    if (!tempo_changes_.empty()) {
        out << "tempo_changes=";
        for (std::size_t t = 0; t < tempo_changes_.size(); ++t) {
            out << (t > 0 ? "," : "") << tempo_changes_[t].frame << ":" << tempo_changes_[t].bpm;
        }
        out << "\n";
    }
    out << "beats_per_bar=" << metronome_.beats_per_bar << "\n";
    out << "beat_unit=" << metronome_.beat_unit << "\n";
    out << "metronome_volume=" << metronome_.volume << "\n";
    out << "grid_step=" << static_cast<int>(grid_step_) << "\n";
    for (const GroupInfo& group : groups_) {
        out << "\n[group]\n";
        out << "name=" << one_line(group.name) << "\n";
        out << "gain=" << group.gain << "\n";
        out << "mute=" << (group.mute ? 1 : 0) << "\n";
        out << "collapsed=" << (group.collapsed ? 1 : 0) << "\n";
    }
    for (const Marker& marker : markers_) {
        out << "\n[marker]\n";
        out << "frame=" << marker.frame << "\n";
        out << "name=" << one_line(marker.name) << "\n";
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        out << "\n[source]\n";
        out << "file=" << files[i] << "\n";
        out << "name=" << one_line(sources[i]->name) << "\n";
    }
    for (const TrackInfo& track : tracks_) {
        out << "\n[track]\n";
        out << "name=" << one_line(track.name) << "\n";
        std::string icon = track.icon;
        if (icons != nullptr) {
            const auto found = icons->find(icon);
            if (found != icons->end()) {
                icon = found->second;  // a picture that travels inside the project
            }
        }
        out << "icon=" << one_line(icon) << "\n";
        out << "instrument=" << static_cast<int>(track.instrument) << "\n";
        out << "program=" << track.program << "\n";
        if (track.plugin_instrument.plugin) {
            const auto& info = track.plugin_instrument.plugin->info();
            const auto index = static_cast<std::size_t>(&track - tracks_.data());
            out << "plugin_instrument_kind=" << info.format << "\n";
            out << "plugin_instrument_path=" << one_line(info.path.u8string()) << "\n";
            out << "plugin_instrument_id=" << one_line(info.id) << "\n";
            out << "plugin_instrument_name=" << one_line(track.plugin_instrument.name) << "\n";
            out << "plugin_instrument_state=" << instrument_state_entry(index) << "\n";
        }
        if (track.sampler) {
            out << "sampler_name=" << one_line(track.sampler->name) << "\n";
            out << "sampler_rate=" << track.sampler->sample_rate << "\n";
            out << "sampler_root=" << track.sampler->root << "\n";
        }
        out << "gain=" << track.gain << "\n";
        out << "pan=" << track.pan << "\n";
        // "slot|parameter|frame:value,..." with slot -1 for the instrument plugin.
        for (const ParamCurveInfo& curve : track.param_curves) {
            long long slot = -2;
            if (curve.plugin && curve.plugin == track.plugin_instrument.plugin) {
                slot = -1;
            }
            for (std::size_t e = 0; e < track.effects.size(); ++e) {
                if (curve.plugin && curve.plugin == track.effects[e].plugin) {
                    slot = static_cast<long long>(e);
                }
            }
            if (slot == -2 || curve.points.empty()) {
                continue;  // its plugin is gone
            }
            out << "param_curve=" << slot << "|" << curve.param << "|";
            for (std::size_t p = 0; p < curve.points.size(); ++p) {
                out << (p > 0 ? "," : "") << curve.points[p].frame << ":" << curve.points[p].value;
            }
            out << "\n";
        }
        if (!track.pan_curve.empty()) {
            out << "pan_curve=";
            for (std::size_t p = 0; p < track.pan_curve.size(); ++p) {
                out << (p > 0 ? "," : "") << track.pan_curve[p].frame << ":" << track.pan_curve[p].value;
            }
            out << "\n";
        }
        if (!track.volume_curve.empty()) {
            out << "volume_curve=";
            for (std::size_t p = 0; p < track.volume_curve.size(); ++p) {
                out << (p > 0 ? "," : "") << track.volume_curve[p].frame << ":" << track.volume_curve[p].value;
            }
            out << "\n";
        }
        out << "mute=" << (track.mute ? 1 : 0) << "\n";
        out << "solo=" << (track.solo ? 1 : 0) << "\n";
        if (track.group >= 0) {
            out << "group=" << track.group << "\n";
        }
        for (const RegionInfo& region : track.regions) {
            out << "[region]\n";
            out << "source=" << index_of[region.source.get()] << "\n";
            out << "start=" << region.start << "\n";
            out << "offset=" << region.offset << "\n";
            out << "length=" << region.length << "\n";
            out << "fade_in=" << region.fade_in << "\n";
            out << "fade_out=" << region.fade_out << "\n";
            out << "fade_shape=" << static_cast<int>(region.fade_shape) << "\n";
            out << "rate=" << region.rate << "\n";
        }
        const auto track_index = static_cast<std::size_t>(&track - tracks_.data());
        for (std::size_t e = 0; e < track.effects.size(); ++e) {
            const EffectSlot& slot = track.effects[e];
            if (!slot.plugin) {
                continue;
            }
            out << "[effect]\n";
            out << "kind=" << slot.plugin->info().format << "\n";
            out << "path=" << one_line(slot.plugin->info().path.u8string()) << "\n";
            out << "id=" << one_line(slot.plugin->info().id) << "\n";
            out << "name=" << one_line(slot.name) << "\n";
            out << "enabled=" << (slot.enabled ? 1 : 0) << "\n";
            out << "state=" << effect_state_entry(track_index, e) << "\n";
        }
    }
    return out.str();
}

// The project as it is now, with the sources named by their address: equal texts mean equal
// projects.
std::string Session::state_text() const {
    const std::vector<std::shared_ptr<const Source>> sources = used_sources();
    std::vector<std::string> names;
    names.reserve(sources.size());
    for (const auto& source : sources) {
        char text[32];
        std::snprintf(text, sizeof(text), "%p", static_cast<const void*>(source.get()));
        names.emplace_back(text);
    }
    return project_text(sources, names);
}

bool Session::modified() const { return state_text() != saved_state_; }

bool Session::save_project(const std::filesystem::path& file, std::string* error) {
    wait_for_background_save();
    if (!write_project(file, error, std::string(), MZ_BEST_SPEED)) {
        return false;
    }
    project_file_ = file;
    saved_state_ = state_text();
    autosaved_state_ = saved_state_;
    return true;
}

bool Session::needs_recovery_save() const {
    const std::string state = state_text();
    return state != saved_state_ && state != autosaved_state_;
}

bool Session::save_recovery(const std::filesystem::path& file, std::string* error) {
    wait_for_background_save();
    std::error_code code;
    std::filesystem::create_directories(file.parent_path(), code);
    // Fast rather than small: an autosave should not hold up the program.
    if (!write_project(file, error, "original_file=" + project_file_.u8string() + "\n",
                       MZ_NO_COMPRESSION)) {
        return false;
    }
    autosaved_state_ = state_text();
    return true;
}

bool Session::restore_recovery(const std::filesystem::path& file, std::string* error) {
    if (!open_project(file, error)) {
        return false;
    }
    project_file_ = recovered_original_;
    saved_state_.clear();  // not saved yet
    autosaved_state_ = state_text();
    return true;
}

// Everything a project file holds, taken on the main thread (plugins give their states only
// there); writing it out can then happen on any thread.
struct Session::ProjectSnapshot {
    struct Entry {
        std::string name;
        std::string bytes;
        int level = MZ_DEFAULT_LEVEL;
    };
    struct Audio {
        std::string name;
        std::shared_ptr<const Source> source;  // keeps its cache file alive
    };
    std::string text;
    std::vector<Entry> entries;
    std::vector<Audio> audio;
    int audio_level = MZ_BEST_SPEED;
};

std::shared_ptr<Session::ProjectSnapshot> Session::snapshot_project(const std::string& head, int audio_level) const {
    auto snapshot = std::make_shared<ProjectSnapshot>();
    snapshot->audio_level = audio_level;
    const std::vector<std::shared_ptr<const Source>> sources = used_sources();
    std::vector<std::string> names;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        names.push_back(sources[i]->clip->notes != nullptr
                            ? "notes-" + std::to_string(i + 1) + ".txt"
                            : "audio-" + std::to_string(i + 1) + ".f32");
    }
    // Pictures of tracks that come from files travel inside the project, so they are there on
    // another computer too and when the file is moved.
    std::map<std::string, std::string> icons;
    std::size_t picture_count = 0;
    std::vector<ProjectSnapshot::Entry> pictures;
    for (const TrackInfo& track : tracks_) {
        if (track.icon.rfind("file:", 0) != 0 || icons.count(track.icon) != 0) {
            continue;
        }
        const std::filesystem::path path = std::filesystem::u8path(track.icon.substr(5));
        std::ifstream picture(path, std::ios::binary);
        if (!picture) {
            continue;  // a picture that is gone stays a reference
        }
        std::string bytes((std::istreambuf_iterator<char>(picture)), std::istreambuf_iterator<char>());
        std::string extension = path.extension().u8string();
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::string name = "picture-" + std::to_string(++picture_count) + extension;
        icons[track.icon] = "embedded:" + name;
        pictures.push_back(ProjectSnapshot::Entry{"pictures/" + name, std::move(bytes), MZ_NO_COMPRESSION});
    }
    snapshot->text = project_text(sources, names, &icons);
    snapshot->text.insert(snapshot->text.find('\n') + 1, head);

    // Sampler sounds: the mono samples as 32-bit floats.
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        if (!tracks_[t].sampler || !tracks_[t].sampler->data) {
            continue;
        }
        const std::vector<float>& samples = *tracks_[t].sampler->data;
        snapshot->entries.push_back(ProjectSnapshot::Entry{
            "samples/sample-" + std::to_string(t + 1) + ".f32",
            std::string(reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(float)),
            MZ_DEFAULT_LEVEL});
    }
    // The states of the instrument plugins and effects, as the plugins write them.
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        const auto& instrument = tracks_[t].plugin_instrument.plugin;
        std::vector<char> instrument_state;
        if (instrument && instrument->save_state(instrument_state)) {
            snapshot->entries.push_back(ProjectSnapshot::Entry{
                instrument_state_entry(t), std::string(instrument_state.begin(), instrument_state.end()),
                MZ_DEFAULT_LEVEL});
        }
    }
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        for (std::size_t e = 0; e < tracks_[t].effects.size(); ++e) {
            const auto& plugin = tracks_[t].effects[e].plugin;
            std::vector<char> state;
            if (!plugin || !plugin->save_state(state)) {
                continue;
            }
            snapshot->entries.push_back(
                ProjectSnapshot::Entry{effect_state_entry(t, e), std::string(state.begin(), state.end()), MZ_DEFAULT_LEVEL});
        }
    }
    for (auto& picture : pictures) {
        snapshot->entries.push_back(std::move(picture));
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const core::ClipData& clip = *sources[i]->clip;
        if (clip.notes != nullptr) {
            // Notes: one line per note, "start length pitch velocity".
            std::ostringstream notes_text;
            notes_text.imbue(std::locale::classic());
            notes_text.precision(9);
            notes_text << "frames=" << clip.notes->frames << "\n";
            for (const core::Note& note : clip.notes->notes) {
                notes_text << note.start << " " << note.length << " " << static_cast<int>(note.pitch) << " "
                           << note.velocity << "\n";
            }
            snapshot->entries.push_back(ProjectSnapshot::Entry{"notes/" + names[i], notes_text.str(), MZ_DEFAULT_LEVEL});
            continue;
        }
        snapshot->audio.push_back(ProjectSnapshot::Audio{"audio/" + names[i], sources[i]});
    }
    return snapshot;
}

// Writes a snapshot as a project file. Any thread: it reads only the snapshot and the clips,
// which never change.
bool Session::write_snapshot(const ProjectSnapshot& snapshot, const std::filesystem::path& file,
                             std::string* error) {
    // The archive is written under another name first, so a failed save never destroys the
    // project that was there.
    std::filesystem::path temporary = file;
    temporary += ".saving";
    std::error_code code;
    bool written = false;
    {
        ZipFile zip;
        zip.file = core::open_file(temporary, "wb");
        if (zip.file == nullptr) {
            set_error(error, "cannot create " + file.u8string());
            return false;
        }
        zip.writing = mz_zip_writer_init_cfile(&zip.zip, zip.file, MZ_ZIP_FLAG_WRITE_ZIP64) != 0;
        bool ok = zip.writing && mz_zip_writer_add_mem(&zip.zip, kProjectEntry, snapshot.text.data(),
                                                       snapshot.text.size(), MZ_DEFAULT_LEVEL) != 0;
        for (std::size_t i = 0; ok && i < snapshot.entries.size(); ++i) {
            const ProjectSnapshot::Entry& entry = snapshot.entries[i];
            ok = mz_zip_writer_add_mem(&zip.zip, entry.name.c_str(), entry.bytes.data(), entry.bytes.size(),
                                       static_cast<mz_uint>(entry.level)) != 0;
        }
        for (std::size_t i = 0; ok && i < snapshot.audio.size(); ++i) {
            const core::ClipData& clip = *snapshot.audio[i].source->clip;
            const std::string& entry = snapshot.audio[i].name;
            const auto bytes = static_cast<mz_uint64>(clip.frames()) * clip.channels * sizeof(float);
            if (clip.disk != nullptr) {
                std::FILE* audio = core::open_file(clip.disk->path(), "rb");
                ok = audio != nullptr &&
                     mz_zip_writer_add_cfile(&zip.zip, entry.c_str(), audio, bytes, nullptr, nullptr, 0,
                                             static_cast<mz_uint>(snapshot.audio_level), nullptr, 0, nullptr,
                                             0) != 0;
                if (audio != nullptr) {
                    std::fclose(audio);
                }
            } else {
                ok = mz_zip_writer_add_mem(&zip.zip, entry.c_str(), clip.samples.data(),
                                           static_cast<std::size_t>(bytes),
                                           static_cast<mz_uint>(snapshot.audio_level)) != 0;
            }
        }
        ok = ok && mz_zip_writer_finalize_archive(&zip.zip) != 0;
        if (zip.writing) {
            mz_zip_writer_end(&zip.zip);
            zip.writing = false;
        }
        const bool closed = std::fclose(zip.file) == 0;
        zip.file = nullptr;
        written = ok && closed;
    }
    if (!written) {
        std::filesystem::remove(temporary, code);
        set_error(error, "cannot write " + file.u8string() + " (is the disk full?)");
        return false;
    }

    std::filesystem::rename(temporary, file, code);
    if (code) {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        code.clear();
        std::filesystem::rename(temporary, file, code);
    }
    if (code) {
        set_error(error, "cannot replace " + file.u8string() + ": " + code.message());
        return false;
    }
    return true;
}

bool Session::write_project(const std::filesystem::path& file, std::string* error,
                            const std::string& head, int audio_level) {
    return write_snapshot(*snapshot_project(head, audio_level), file, error);
}

bool Session::start_background_save(const std::filesystem::path& file, bool recovery) {
    if (save_running_) {
        return false;
    }
    std::shared_ptr<ProjectSnapshot> snapshot;
    if (recovery) {
        std::error_code code;
        std::filesystem::create_directories(file.parent_path(), code);
        snapshot = snapshot_project("original_file=" + project_file_.u8string() + "\n", MZ_NO_COMPRESSION);
    } else {
        snapshot = snapshot_project(std::string(), MZ_BEST_SPEED);
    }
    save_state_ = state_text();
    save_generation_ = project_generation_;
    save_result_ = SaveResult();
    save_result_.file = file;
    save_result_.recovery = recovery;
    save_done_.store(false, std::memory_order_relaxed);
    save_running_ = true;
    save_thread_ = std::thread([this, snapshot, file] {
        std::string error;
        save_ok_ = write_snapshot(*snapshot, file, &error);
        save_error_ = error;
        save_done_.store(true, std::memory_order_release);
    });
    return true;
}

bool Session::save_project_in_background(const std::filesystem::path& file) {
    return start_background_save(file, false);
}

bool Session::save_recovery_in_background(const std::filesystem::path& file) {
    return start_background_save(file, true);
}

void Session::finish_background_save() {
    if (!save_running_ || !save_done_.load(std::memory_order_acquire)) {
        return;
    }
    if (save_thread_.joinable()) {
        save_thread_.join();
    }
    save_running_ = false;
    save_result_.done = true;
    save_result_.ok = save_ok_;
    save_result_.error = save_error_;
    // Edits made while it was written stay unsaved; another project opened meanwhile is not
    // touched.
    if (save_ok_ && save_generation_ == project_generation_) {
        if (save_result_.recovery) {
            autosaved_state_ = save_state_;
        } else {
            project_file_ = save_result_.file;
            saved_state_ = save_state_;
            autosaved_state_ = save_state_;
        }
    }
}

Session::SaveResult Session::take_save_result() {
    if (!save_result_.done) {
        return SaveResult();
    }
    SaveResult result = save_result_;
    save_result_ = SaveResult();
    return result;
}

void Session::wait_for_background_save() {
    if (save_running_) {
        if (save_thread_.joinable()) {
            save_thread_.join();
        }
        finish_background_save();
    }
}

bool Session::open_project(const std::filesystem::path& file, std::string* error) {
    if (recording_) {
        set_error(error, "stop the recording first");
        return false;
    }
    ZipFile zip;
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(file, code);
    zip.file = code ? nullptr : core::open_file(file, "rb");
    if (zip.file == nullptr) {
        set_error(error, "cannot open " + file.u8string());
        return false;
    }
    zip.reading = mz_zip_reader_init_cfile(&zip.zip, zip.file, static_cast<mz_uint64>(size), 0) != 0;
    const int text_index =
        zip.reading ? mz_zip_reader_locate_file(&zip.zip, kProjectEntry, nullptr, 0) : -1;
    std::string text;
    if (text_index >= 0) {
        std::size_t text_size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip.zip, static_cast<mz_uint>(text_index),
                                                   &text_size, 0);
        if (data != nullptr) {
            text.assign(static_cast<const char*>(data), text_size);
            mz_free(data);
        }
    }

    std::istringstream in(text);
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != kProjectHeader) {
        set_error(error, "this is not a Tone of Hell project");
        return false;
    }

    struct SourceEntry {
        std::string file;
        std::string name;
    };
    struct RegionEntry {
        long long source = -1;
        std::int64_t start = 0;
        std::int64_t offset = 0;
        std::int64_t length = 0;
        std::int64_t fade_in = 0;
        std::int64_t fade_out = 0;
        long long fade_shape = static_cast<long long>(core::FadeShape::Curve);
        double rate = 1.0;
    };
    struct EffectEntry {
        std::string kind = "clap";
        std::string path;
        std::string id;
        std::string name;
        std::string state;
        bool enabled = true;
    };
    struct TrackEntry {
        std::string name;
        std::string icon;
        float gain = 1.0f;
        float pan = 0.0f;
        std::string volume_curve;
        std::string pan_curve;
        std::vector<std::string> param_curves;
        bool mute = false;
        bool solo = false;
        long long group = -1;
        long long instrument = 0;
        long long program = -1;
        bool sampler = false;
        std::string sampler_name;
        double sampler_rate = 0.0;
        double sampler_root = -1.0;
        std::vector<RegionEntry> regions;
        std::vector<EffectEntry> effects;
        EffectEntry plugin_instrument;
    };
    std::vector<SourceEntry> sources;
    std::vector<TrackEntry> tracks;
    long long rate = 0;
    long long channels = 0;
    bool loop = false;
    long long loop_from = 0;
    long long loop_to = -1;
    MetronomeSettings metronome = metronome_;
    std::string tempo_text;
    long long grid = static_cast<long long>(grid_step_);
    std::filesystem::path original;  // in recovery files: the project file they belong to
    enum class Section { Head, Source, Track, Region, Effect, Marker, Group } section = Section::Head;
    std::vector<Marker> markers;
    std::vector<GroupInfo> groups;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line == "[source]") {
            sources.emplace_back();
            section = Section::Source;
            continue;
        }
        if (line == "[track]") {
            tracks.emplace_back();
            section = Section::Track;
            continue;
        }
        if (line == "[region]") {
            if (tracks.empty()) {
                tracks.emplace_back();
            }
            tracks.back().regions.emplace_back();
            section = Section::Region;
            continue;
        }
        if (line == "[group]") {
            groups.emplace_back();
            section = Section::Group;
            continue;
        }
        if (line == "[marker]") {
            markers.emplace_back();
            section = Section::Marker;
            continue;
        }
        if (line == "[effect]") {
            if (tracks.empty()) {
                tracks.emplace_back();
            }
            tracks.back().effects.emplace_back();
            section = Section::Effect;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        const long long integer = std::strtoll(value.c_str(), nullptr, 10);
        const double real = std::strtod(value.c_str(), nullptr);
        switch (section) {
            case Section::Head:
                if (key == "sample_rate") {
                    rate = integer;
                } else if (key == "channels") {
                    channels = integer;
                } else if (key == "loop") {
                    loop = integer != 0;
                } else if (key == "loop_range") {
                    const std::size_t colon = value.find(':');
                    if (colon != std::string::npos) {
                        loop_from = std::strtoll(value.c_str(), nullptr, 10);
                        loop_to = std::strtoll(value.c_str() + colon + 1, nullptr, 10);
                    }
                } else if (key == "metronome") {
                    metronome.enabled = integer != 0;
                } else if (key == "tempo_changes") {
                    tempo_text = value;
                } else if (key == "bpm") {
                    metronome.bpm = real;
                } else if (key == "beats_per_bar") {
                    metronome.beats_per_bar = static_cast<int>(integer);
                } else if (key == "beat_unit") {
                    metronome.beat_unit = static_cast<int>(integer);
                } else if (key == "metronome_volume") {
                    metronome.volume = static_cast<float>(real);
                } else if (key == "grid_step") {
                    grid = integer;
                } else if (key == "original_file") {
                    original = std::filesystem::u8path(value);
                }
                break;
            case Section::Source:
                if (key == "file") {
                    sources.back().file = value;
                } else if (key == "name") {
                    sources.back().name = value;
                }
                break;
            case Section::Track:
                if (key == "name") {
                    tracks.back().name = value;
                } else if (key == "icon") {
                    tracks.back().icon = value;
                } else if (key == "gain") {
                    tracks.back().gain = std::clamp(static_cast<float>(real), 0.0f, 2.0f);
                } else if (key == "param_curve") {
                    tracks.back().param_curves.push_back(value);
                } else if (key == "pan_curve") {
                    tracks.back().pan_curve = value;
                } else if (key == "volume_curve") {
                    tracks.back().volume_curve = value;
                } else if (key == "pan") {
                    tracks.back().pan = std::clamp(static_cast<float>(real), -1.0f, 1.0f);
                } else if (key == "mute") {
                    tracks.back().mute = integer != 0;
                } else if (key == "solo") {
                    tracks.back().solo = integer != 0;
                } else if (key == "group") {
                    tracks.back().group = integer;
                } else if (key == "instrument") {
                    tracks.back().instrument = integer;
                } else if (key == "program") {
                    tracks.back().program = integer;
                } else if (key == "plugin_instrument_kind") {
                    tracks.back().plugin_instrument.kind = value;
                } else if (key == "plugin_instrument_path") {
                    tracks.back().plugin_instrument.path = value;
                } else if (key == "plugin_instrument_id") {
                    tracks.back().plugin_instrument.id = value;
                } else if (key == "plugin_instrument_name") {
                    tracks.back().plugin_instrument.name = value;
                } else if (key == "plugin_instrument_state") {
                    tracks.back().plugin_instrument.state = value;
                } else if (key == "sampler_name") {
                    tracks.back().sampler = true;
                    tracks.back().sampler_name = value;
                } else if (key == "sampler_rate") {
                    tracks.back().sampler_rate = real;
                } else if (key == "sampler_root") {
                    tracks.back().sampler_root = real;
                }
                break;
            case Section::Region: {
                RegionEntry& region = tracks.back().regions.back();
                if (key == "source") {
                    region.source = integer;
                } else if (key == "start") {
                    region.start = integer;
                } else if (key == "offset") {
                    region.offset = integer;
                } else if (key == "length") {
                    region.length = integer;
                } else if (key == "fade_in") {
                    region.fade_in = integer;
                } else if (key == "fade_out") {
                    region.fade_out = integer;
                } else if (key == "fade_shape") {
                    region.fade_shape = integer;
                } else if (key == "rate") {
                    region.rate = real;
                }
                break;
            }
            case Section::Group:
                if (key == "name") {
                    groups.back().name = value;
                } else if (key == "gain") {
                    groups.back().gain = std::clamp(static_cast<float>(real), 0.0f, 8.0f);
                } else if (key == "mute") {
                    groups.back().mute = integer != 0;
                } else if (key == "collapsed") {
                    groups.back().collapsed = integer != 0;
                }
                break;
            case Section::Marker:
                if (key == "frame") {
                    markers.back().frame = std::max<long long>(integer, 0);
                } else if (key == "name") {
                    markers.back().name = value;
                }
                break;
            case Section::Effect: {
                EffectEntry& effect = tracks.back().effects.back();
                if (key == "kind") {
                    effect.kind = value;
                } else if (key == "path") {
                    effect.path = value;
                } else if (key == "id") {
                    effect.id = value;
                } else if (key == "name") {
                    effect.name = value;
                } else if (key == "enabled") {
                    effect.enabled = integer != 0;
                } else if (key == "state") {
                    effect.state = value;
                }
                break;
            }
        }
    }

    if (rate != static_cast<long long>(config_.sample_rate) ||
        channels != static_cast<long long>(config_.channels)) {
        set_error(error, "the project was saved at " + std::to_string(rate) + " Hz with " +
                             std::to_string(channels) + " channels, but the program runs at " +
                             std::to_string(config_.sample_rate) + " Hz with " +
                             std::to_string(config_.channels));
        return false;
    }

    // Unpack the audio into the cache folder. The cache files go when nothing uses them.
    std::vector<std::shared_ptr<const Source>> loaded;
    for (const SourceEntry& entry : sources) {
        if (!valid_audio_name(entry.file)) {
            set_error(error, "the project names an invalid audio file: " + entry.file);
            return false;
        }
        if (entry.file.size() > 4 && entry.file.compare(entry.file.size() - 4, 4, ".txt") == 0) {
            // The notes of a note region.
            const int note_index =
                mz_zip_reader_locate_file(&zip.zip, ("notes/" + entry.file).c_str(), nullptr, 0);
            std::string body;
            if (note_index >= 0) {
                std::size_t body_size = 0;
                void* data = mz_zip_reader_extract_to_heap(&zip.zip, static_cast<mz_uint>(note_index),
                                                           &body_size, 0);
                if (data != nullptr) {
                    body.assign(static_cast<const char*>(data), body_size);
                    mz_free(data);
                }
            }
            std::istringstream note_in(body);
            note_in.imbue(std::locale::classic());
            std::string note_line;
            std::int64_t note_frames = 0;
            std::vector<core::Note> notes;
            while (std::getline(note_in, note_line)) {
                if (note_line.rfind("frames=", 0) == 0) {
                    note_frames = std::strtoll(note_line.c_str() + 7, nullptr, 10);
                    continue;
                }
                std::istringstream fields(note_line);
                fields.imbue(std::locale::classic());
                long long note_start = 0;
                long long note_length = 0;
                int pitch = 0;
                float velocity = 0.0f;
                if (fields >> note_start >> note_length >> pitch >> velocity) {
                    core::Note note;
                    note.start = note_start;
                    note.length = note_length;
                    note.pitch = static_cast<std::uint8_t>(std::clamp(pitch, 0, 255));
                    note.velocity = velocity;
                    notes.push_back(note);
                }
            }
            if (note_frames <= 0) {
                set_error(error, "the notes of the project are missing or damaged: " + entry.file);
                return false;
            }
            loaded.push_back(note_source(entry.name, std::move(notes), note_frames,
                                         config_.channels, config_.sample_rate));
            continue;
        }
        const int index =
            mz_zip_reader_locate_file(&zip.zip, ("audio/" + entry.file).c_str(), nullptr, 0);
        const std::filesystem::path path = index >= 0 ? new_cache_file("project") : std::filesystem::path();
        std::FILE* out = path.empty() ? nullptr : core::open_file(path, "wb");
        bool ok = out != nullptr &&
                  mz_zip_reader_extract_to_cfile(&zip.zip, static_cast<mz_uint>(index), out, 0) != 0;
        if (out != nullptr) {
            ok = std::fclose(out) == 0 && ok;
        }
        std::shared_ptr<core::PagedFile> disk = ok ? core::PagedFile::open(path, config_.channels) : nullptr;
        if (!disk) {
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
            set_error(error, "an audio file of the project is missing or damaged: " + entry.file);
            return false;
        }
        disk->set_remove_on_close(true);
        auto clip = std::make_shared<core::ClipData>();
        clip->channels = config_.channels;
        clip->sample_rate = config_.sample_rate;
        clip->disk = std::move(disk);
        auto source = std::make_shared<Source>();
        source->name = entry.name;
        source->overview = make_overview(*clip);
        source->clip = std::move(clip);
        loaded.push_back(std::move(source));
    }

    // Pictures inside the project are unpacked into the cache folder as well.
    std::map<std::string, std::string> unpacked;  // entry name -> icon
    const auto picture_icon = [&](const std::string& name) {
        const auto found = unpacked.find(name);
        if (found != unpacked.end()) {
            return found->second;
        }
        std::string icon;
        if (valid_audio_name(name)) {
            const int index =
                mz_zip_reader_locate_file(&zip.zip, ("pictures/" + name).c_str(), nullptr, 0);
            const std::string extension = std::filesystem::u8path(name).extension().u8string();
            const std::filesystem::path path =
                index >= 0 ? new_cache_file("picture", extension.c_str()) : std::filesystem::path();
            std::FILE* out = path.empty() ? nullptr : core::open_file(path, "wb");
            bool ok = out != nullptr &&
                      mz_zip_reader_extract_to_cfile(&zip.zip, static_cast<mz_uint>(index), out, 0) != 0;
            if (out != nullptr) {
                ok = std::fclose(out) == 0 && ok;
            }
            if (ok) {
                icon = "file:" + path.u8string();
            }
        }
        unpacked[name] = icon;
        return icon;
    };

    open_warnings_.clear();
    if (groups.size() > kMaxGroups) {
        groups.resize(kMaxGroups);
    }
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (trimmed(groups[g].name).empty()) {
            groups[g].name = "Group " + std::to_string(g + 1);
        }
    }
    std::vector<TrackInfo> fresh;
    for (const TrackEntry& entry : tracks) {
        if (fresh.size() >= core::Engine::kMaxTracks) {
            break;
        }
        TrackInfo track;
        track.name = entry.name.empty() ? "Track " + std::to_string(fresh.size() + 1) : entry.name;
        track.icon = entry.icon.rfind("embedded:", 0) == 0 ? picture_icon(entry.icon.substr(9))
                                                           : entry.icon;
        track.gain = entry.gain;
        track.pan = entry.pan;
        track.group = entry.group >= 0 && entry.group < static_cast<long long>(groups.size())
                          ? static_cast<int>(entry.group)
                          : -1;
        track.pan_curve = parse_curve(entry.pan_curve, -1.0f, 1.0f);
        // "frame:value,frame:value"
        for (const char* cursor = entry.volume_curve.c_str(); *cursor != '\0';) {
            char* after = nullptr;
            core::CurvePoint point;
            point.frame = std::max<long long>(std::strtoll(cursor, &after, 10), 0);
            if (after == cursor || *after != ':') {
                break;
            }
            cursor = after + 1;
            point.value = std::clamp(static_cast<float>(std::strtod(cursor, &after)), 0.0f, 2.0f);
            if (after == cursor) {
                break;
            }
            track.volume_curve.push_back(point);
            cursor = *after == ',' ? after + 1 : after;
        }
        std::stable_sort(track.volume_curve.begin(), track.volume_curve.end(),
                         [](const core::CurvePoint& a, const core::CurvePoint& b) { return a.frame < b.frame; });
        track.mute = entry.mute;
        track.solo = entry.solo;
        if (entry.instrument >= 0 && entry.instrument < core::kInstrumentCount) {
            track.instrument = static_cast<core::Instrument>(entry.instrument);
        }
        if (entry.program >= -1 && entry.program <= core::SoundFont::kDrums) {
            track.program = static_cast<int>(entry.program);
        }
        if (entry.sampler) {
            const std::string sample_entry = "samples/sample-" + std::to_string(fresh.size() + 1) + ".f32";
            const int sample_index = mz_zip_reader_locate_file(&zip.zip, sample_entry.c_str(), nullptr, 0);
            std::vector<float> samples;
            if (sample_index >= 0) {
                std::size_t sample_bytes = 0;
                void* data = mz_zip_reader_extract_to_heap(&zip.zip, static_cast<mz_uint>(sample_index),
                                                           &sample_bytes, 0);
                if (data != nullptr) {
                    samples.resize(sample_bytes / sizeof(float));
                    std::memcpy(samples.data(), data, samples.size() * sizeof(float));
                    mz_free(data);
                }
            }
            if (!samples.empty()) {
                const double sample_rate =
                    entry.sampler_rate > 0.0 ? entry.sampler_rate : static_cast<double>(config_.sample_rate);
                const double root = entry.sampler_root >= 0.0 && entry.sampler_root <= 127.0
                                        ? entry.sampler_root
                                        : -1.0;
                track.sampler = core::make_sampler_sound(entry.sampler_name, std::move(samples),
                                                         sample_rate, root);
            }
        }
        for (const RegionEntry& region : entry.regions) {
            if (region.source < 0 || static_cast<std::size_t>(region.source) >= loaded.size()) {
                continue;
            }
            const auto& source = loaded[static_cast<std::size_t>(region.source)];
            if (region.start < 0 || region.offset < 0 || region.length <= 0 ||
                !(region.rate >= core::Engine::kMinSpeed && region.rate <= core::Engine::kMaxSpeed) ||
                region.offset + core::clip_frames_used(region.length, region.rate) >
                    source->clip->frames()) {
                continue;  // does not fit its audio: left out
            }
            RegionInfo info;
            info.source = source;
            info.start = region.start;
            info.offset = region.offset;
            info.rate = region.rate;
            info.length = region.length;
            info.fade_in = region.fade_in;
            info.fade_out = region.fade_out;
            if (region.fade_shape >= 0 &&
                region.fade_shape <= static_cast<long long>(core::FadeShape::SCurve)) {
                info.fade_shape = static_cast<core::FadeShape>(region.fade_shape);
            }
            limit_fades(info);
            track.regions.push_back(std::move(info));
        }
        // Plugins: they are loaded again and get back their state.
        const auto load_plugin = [&](const EffectEntry& effect) -> std::shared_ptr<plugins::Plugin> {
            std::string problem;
            auto plugin = plugins::create_plugin(effect.kind, std::filesystem::u8path(effect.path), effect.id,
                                                 static_cast<double>(config_.sample_rate), config_.channels,
                                                 &problem);
            if (!plugin) {
                open_warnings_.push_back((effect.name.empty() ? effect.id : effect.name) + ": " + problem);
                return nullptr;
            }
            const int index = effect.state.empty()
                                  ? -1
                                  : mz_zip_reader_locate_file(&zip.zip, effect.state.c_str(), nullptr, 0);
            if (index >= 0) {
                std::size_t bytes = 0;
                void* data = mz_zip_reader_extract_to_heap(&zip.zip, static_cast<mz_uint>(index), &bytes, 0);
                if (data != nullptr) {
                    const std::vector<char> state(static_cast<const char*>(data),
                                                  static_cast<const char*>(data) + bytes);
                    mz_free(data);
                    plugin->load_state(state);
                }
            }
            return plugin;
        };
        if (!entry.plugin_instrument.path.empty()) {
            if (auto plugin = load_plugin(entry.plugin_instrument)) {
                track.plugin_instrument.name =
                    entry.plugin_instrument.name.empty() ? plugin->info().name : entry.plugin_instrument.name;
                track.plugin_instrument.plugin = std::move(plugin);
            }
        }
        std::vector<std::shared_ptr<plugins::Plugin>> by_slot;  // as saved; null where one is missing
        for (const EffectEntry& effect : entry.effects) {
            if (track.effects.size() >= kMaxEffects) {
                break;
            }
            auto plugin = load_plugin(effect);
            by_slot.push_back(plugin);
            if (!plugin) {
                continue;
            }
            EffectSlot slot;
            slot.name = effect.name.empty() ? plugin->info().name : effect.name;
            slot.enabled = effect.enabled;
            slot.plugin = std::move(plugin);
            track.effects.push_back(std::move(slot));
        }
        for (const std::string& saved : entry.param_curves) {
            char* after = nullptr;
            const long long slot = std::strtoll(saved.c_str(), &after, 10);
            if (*after != '|') {
                continue;
            }
            const char* rest = after + 1;
            const unsigned long long param = std::strtoull(rest, &after, 10);
            if (after == rest || *after != '|') {
                continue;
            }
            std::shared_ptr<plugins::Plugin> plugin;
            if (slot == -1) {
                plugin = track.plugin_instrument.plugin;
            } else if (slot >= 0 && static_cast<std::size_t>(slot) < by_slot.size()) {
                plugin = by_slot[static_cast<std::size_t>(slot)];
            }
            if (!plugin) {
                continue;
            }
            double low = 0.0;
            double high = 1.0;
            for (const plugins::Plugin::Param& info : plugin->params()) {
                if (info.id == static_cast<std::uint32_t>(param)) {
                    low = info.min;
                    high = info.max;
                }
            }
            std::vector<core::CurvePoint> points =
                parse_curve(after + 1, static_cast<float>(low), static_cast<float>(high));
            if (!points.empty()) {
                track.param_curves.push_back(
                    ParamCurveInfo{std::move(plugin), static_cast<std::uint32_t>(param), std::move(points)});
            }
        }
        fresh.push_back(std::move(track));
    }

    // Everything checked: replace the session.
    engine_.stop();
    const std::size_t old_count = tracks_.size();
    tracks_ = std::move(fresh);
    groups_ = std::move(groups);
    sync_groups();
    sync_engine_tracks(0);
    for (std::size_t slot = tracks_.size() + 1; slot < old_count; ++slot) {
        clear_engine_slot(slot);
    }
    update_length();
    loop_start_ = loop_to > loop_from ? std::max<long long>(loop_from, 0) : 0;
    loop_end_ = loop_to > loop_from ? loop_to : -1;
    set_loop(loop);
    set_metronome(metronome);  // ignored if a value is out of range
    markers_ = std::move(markers);
    sort_markers();
    tempo_changes_.clear();
    for (const core::CurvePoint& point : parse_curve(tempo_text, static_cast<float>(core::TempoMap::kMinBpm),
                                                     static_cast<float>(core::TempoMap::kMaxBpm))) {
        if (point.frame > 0) {
            tempo_changes_.push_back(core::TempoChange{point.frame, static_cast<double>(point.value)});
        }
    }
    rebuild_tempo();
    if (grid >= 0 && grid <= static_cast<long long>(GridStep::None)) {
        grid_step_ = static_cast<GridStep>(grid);
    }
    seek_frames(0);
    ++project_generation_;
    project_file_ = file;
    saved_state_ = state_text();
    autosaved_state_ = saved_state_;
    recovered_original_ = original;
    clear_history();
    return true;
}

void Session::new_project() {
    stop_recording();  // a take that is still running is dropped: its track goes away
    engine_.stop();
    const std::size_t old_count = tracks_.size();
    tracks_.clear();
    for (std::size_t slot = 0; slot < old_count; ++slot) {
        clear_engine_slot(slot);
    }
    update_length();
    update_streaming();
    seek_frames(0);
    tempo_changes_.clear();
    markers_.clear();
    groups_.clear();
    sync_groups();
    set_loop_range(0, -1);
    rebuild_tempo();
    ++project_generation_;
    project_file_.clear();
    saved_state_ = state_text();
    autosaved_state_ = saved_state_;
    clear_history();
}

// ---- Undo --------------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxUndoSteps = 200;

std::size_t region_count(const std::vector<TrackInfo>& tracks) {
    std::size_t count = 0;
    for (const TrackInfo& track : tracks) {
        count += track.regions.size();
    }
    return count;
}

// A short name for what changed between two states, for the Edit menu.
std::string describe_change(const std::vector<TrackInfo>& before,
                            const std::vector<TrackInfo>& after) {
    if (after.size() > before.size()) {
        return "Add a track";
    }
    if (after.size() < before.size()) {
        return "Remove a track";
    }
    const std::size_t regions_before = region_count(before);
    const std::size_t regions_after = region_count(after);
    if (regions_after > regions_before) {
        return "Split or record";
    }
    if (regions_after < regions_before) {
        return "Remove or join regions";
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i].name != after[i].name) {
            return "Rename a track";
        }
        if (before[i].instrument != after[i].instrument || before[i].program != after[i].program) {
            return "Change the instrument";
        }
        if (before[i].icon != after[i].icon) {
            return "Change a track picture";
        }
        if (before[i].gain != after[i].gain || before[i].pan != after[i].pan) {
            return "Volume or pan";
        }
    }
    return "Edit regions";
}

}  // namespace

std::string Session::edit_text() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(9);
    for (const TrackInfo& track : tracks_) {
        out << "T|" << track.name << "|" << track.icon << "|" << track.gain << "|" << track.pan << "|"
            << static_cast<int>(track.instrument) << "|" << track.program << "|"
            << static_cast<const void*>(track.sampler.get()) << "|" << track.group << "\n";
        for (const EffectSlot& slot : track.effects) {
            out << "E|" << static_cast<const void*>(slot.plugin.get()) << "|" << (slot.enabled ? 1 : 0)
                << "\n";
        }
        for (const ParamCurveInfo& curve : track.param_curves) {
            out << "C|" << static_cast<const void*>(curve.plugin.get()) << "|" << curve.param;
            for (const core::CurvePoint& point : curve.points) {
                out << "|" << point.frame << ":" << point.value;
            }
            out << "\n";
        }
        out << "I|" << static_cast<const void*>(track.plugin_instrument.plugin.get()) << "\n";
        out << "V";
        for (const core::CurvePoint& point : track.volume_curve) {
            out << "|" << point.frame << ":" << point.value;
        }
        out << "\nP";
        for (const core::CurvePoint& point : track.pan_curve) {
            out << "|" << point.frame << ":" << point.value;
        }
        out << "\n";
        for (const RegionInfo& region : track.regions) {
            out << "R|" << static_cast<const void*>(region.source.get()) << "|" << region.start
                << "|" << region.offset << "|" << region.length << "|" << region.fade_in << "|"
                << region.fade_out << "|" << static_cast<int>(region.fade_shape) << "|"
                << region.rate << "\n";
        }
    }
    for (const GroupInfo& group : groups_) {
        out << "G|" << group.name << "|" << group.gain << "\n";
    }
    // Markers and the tempo are part of the edits, too, so their changes can be taken back.
    for (const Marker& marker : markers_) {
        out << "K|" << marker.frame << "|" << marker.name << "\n";
    }
    out << "M|" << metronome_.bpm;
    for (const core::TempoChange& change : tempo_changes_) {
        out << "|" << change.frame << ":" << change.bpm;
    }
    out << "\n";
    return out.str();
}

Session::Snapshot Session::capture(std::string text, std::string label) const {
    Snapshot snapshot;
    snapshot.tracks = tracks_;
    snapshot.text = std::move(text);
    snapshot.label = std::move(label);
    snapshot.tempo = tempo_changes_;
    snapshot.markers = markers_;
    snapshot.groups = groups_;
    snapshot.bpm = metronome_.bpm;
    return snapshot;
}

void Session::restore_tempo(const Snapshot& snapshot) {
    // Mute and folding are not edits: they stay as they are for the groups that remain.
    std::vector<GroupInfo> groups = snapshot.groups;
    for (std::size_t g = 0; g < groups.size() && g < groups_.size(); ++g) {
        groups[g].mute = groups_[g].mute;
        groups[g].collapsed = groups_[g].collapsed;
    }
    groups_ = std::move(groups);
    sync_groups();
    markers_ = snapshot.markers;
    tempo_changes_ = snapshot.tempo;
    if (snapshot.bpm != metronome_.bpm) {
        MetronomeSettings settings = metronome_;
        settings.bpm = snapshot.bpm;
        if (set_metronome(settings)) {
            return;  // the tempo map was built again there
        }
    }
    rebuild_tempo();
}

void Session::clear_history() {
    undo_.clear();
    redo_.clear();
    committed_ = capture(edit_text(), {});
}

void Session::commit_edits() {
    if (recording_) {
        return;  // the take is not finished yet
    }
    const std::string label = std::move(pending_label_);
    pending_label_.clear();
    std::string text = edit_text();
    if (text == committed_.text) {
        return;
    }
    Snapshot previous = std::move(committed_);
    const auto same_tempo = [this](const Snapshot& snapshot) {
        if (snapshot.bpm != metronome_.bpm || snapshot.tempo.size() != tempo_changes_.size()) {
            return false;
        }
        for (std::size_t t = 0; t < tempo_changes_.size(); ++t) {
            if (snapshot.tempo[t].frame != tempo_changes_[t].frame || snapshot.tempo[t].bpm != tempo_changes_[t].bpm) {
                return false;
            }
        }
        return true;
    };
    if (label.empty()) {
        bool same_markers = previous.markers.size() == markers_.size();
        for (std::size_t m = 0; same_markers && m < markers_.size(); ++m) {
            same_markers = previous.markers[m].frame == markers_[m].frame && previous.markers[m].name == markers_[m].name;
        }
        previous.label = !same_markers         ? "Change the markers"
                         : !same_tempo(previous) ? "Change the tempo"
                                                 : describe_change(previous.tracks, tracks_);
    } else {
        previous.label = label;
    }
    committed_ = capture(std::move(text), {});
    undo_.push_back(std::move(previous));
    if (undo_.size() > kMaxUndoSteps) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
}

std::string Session::undo_label() const { return undo_.empty() ? std::string() : undo_.back().label; }

std::string Session::redo_label() const { return redo_.empty() ? std::string() : redo_.back().label; }

bool Session::undo() {
    if (recording_) {
        return false;
    }
    commit_edits();  // an edit that was not committed yet is the step to take back
    if (undo_.empty()) {
        return false;
    }
    Snapshot target = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(capture(committed_.text, target.label));
    restore_tracks(target.tracks);
    restore_tempo(target);
    committed_ = capture(edit_text(), {});
    return true;
}

bool Session::redo() {
    if (recording_) {
        return false;
    }
    commit_edits();
    if (redo_.empty()) {
        return false;
    }
    Snapshot target = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(capture(committed_.text, target.label));
    restore_tracks(target.tracks);
    restore_tempo(target);
    committed_ = capture(edit_text(), {});
    return true;
}

void Session::restore_tracks(const std::vector<TrackInfo>& tracks) {
    const std::size_t old_count = tracks_.size();
    tracks_ = tracks;
    sync_engine_tracks(0);
    for (std::size_t slot = tracks_.size() + 1; slot < old_count; ++slot) {
        clear_engine_slot(slot);
    }
    sync_mutes();
    update_length();
}

void Session::clear_engine_slot(std::size_t slot) {
    if (slot >= core::Engine::kMaxTracks) {
        return;
    }
    const auto index = static_cast<std::uint32_t>(slot);
    engine_.set_track_regions(index, {});
    engine_.set_track_gain(index, 1.0f);
    engine_.set_track_pan(index, 0.0f);
    engine_.set_track_mute(index, false);
    engine_.set_track_solo(index, false);
}

// ---- MIDI files --------------------------------------------------------------------------

bool Session::import_midi(const std::filesystem::path& path, std::string* error) {
    MidiSong song;
    if (!read_midi_file(path, config_.sample_rate, song, error)) {
        return false;
    }
    MetronomeSettings metronome = metronome_;
    metronome.bpm = std::clamp(song.bpm, 20.0, 400.0);
    metronome.beats_per_bar = song.beats_per_bar;
    metronome.beat_unit = song.beat_unit;
    set_metronome(metronome);  // ignored if the file's values do not fit

    // One second more than the last note, so its release is heard too.
    const std::int64_t length = song.frames + static_cast<std::int64_t>(config_.sample_rate);
    std::size_t added = 0;
    for (const MidiPart& part : song.parts) {
        std::string name = part.name;
        if (name.empty()) {
            name = path.stem().u8string() + " " + std::to_string(part.channel + 1);
        }
        const std::size_t index =
            add_instrument_track(name, core::Instrument::Keys, 0, length, part.program);
        if (index >= tracks_.size()) {
            break;  // the track limit is reached
        }
        set_region_notes(index, 0, part.notes);
        ++added;
    }
    if (added == 0) {
        set_error(error, "no track could be added (the track limit is reached)");
        return false;
    }
    return true;
}

bool Session::export_midi(const std::filesystem::path& path, std::string* error) const {
    MidiSong song;
    song.bpm = metronome_.bpm;
    song.beats_per_bar = metronome_.beats_per_bar;
    song.beat_unit = metronome_.beat_unit;
    int next_channel = 0;
    for (const TrackInfo& track : tracks_) {
        MidiPart part;
        part.name = track.name;
        for (const RegionInfo& region : track.regions) {
            if (!region.has_notes()) {
                continue;
            }
            // The notes that the region plays, where they sound on the timeline.
            for (const core::Note& note : region.source->clip->notes->notes) {
                if (note.start < region.offset || note.start >= region.end() - region.start + region.offset) {
                    continue;
                }
                core::Note placed = note;
                placed.start = region.start + (note.start - region.offset);
                placed.length = std::min(note.length, region.end() - placed.start);
                part.notes.push_back(placed);
            }
        }
        if (part.notes.empty()) {
            continue;
        }
        std::stable_sort(part.notes.begin(), part.notes.end(),
                         [](const core::Note& a, const core::Note& b) { return a.start < b.start; });
        if (track.program == core::SoundFont::kDrums) {
            part.channel = 9;
            part.program = 0;
        } else {
            if (next_channel == 9) {
                ++next_channel;  // channel 10 is for drums
            }
            part.channel = next_channel % 16;
            next_channel = (next_channel + 1) % 16;
            // The simple synthesizers become the General MIDI sounds closest to them.
            static const int closest[] = {4, 16, 89, 24};  // keys, organ, pad, pluck
            part.program = track.program >= 0 ? track.program
                                              : closest[static_cast<int>(track.instrument)];
        }
        song.parts.push_back(std::move(part));
    }
    if (song.parts.empty()) {
        set_error(error, "there are no notes to export");
        return false;
    }
    return write_midi_file(path, song, config_.sample_rate, error);
}

// ---- Export ------------------------------------------------------------------------------

bool Session::export_wav(const std::filesystem::path& path, audio::SampleFormat format,
                         std::string* error) const {
    if (length_frames_ <= 0) {
        set_error(error, "there is nothing to export");
        return false;
    }

    audio::WavWriter writer;
    if (!writer.open(path, config_.sample_rate, config_.channels, format, error)) {
        return false;
    }
    const bool written = render_project(
        [&writer](const float* data, std::uint32_t frames) { return writer.write(data, frames); });
    if (!written) {
        set_error(error, "cannot write the file (is the disk full?)");
        return false;
    }
    writer.close();
    return true;
}

bool Session::export_mp3(const std::filesystem::path& path, int kbps, std::string* error) const {
    if (length_frames_ <= 0) {
        set_error(error, "there is nothing to export");
        return false;
    }
    audio::Mp3Writer writer;
    if (!writer.open(path, config_.sample_rate, config_.channels, kbps, error)) {
        return false;
    }
    const bool written = render_project(
        [&writer](const float* data, std::uint32_t frames) { return writer.write(data, frames); });
    if (!writer.close() || !written) {
        set_error(error, "cannot write the file (is the disk full?)");
        return false;
    }
    return true;
}

bool Session::render_project(const std::function<bool(const float*, std::uint32_t)>& sink) const {
    // A second engine renders the same project offline. A plugin cannot work for two engines at
    // once, so while the file is written the plugins are lent to the second engine: playback
    // stops, the playing engine lets go of them, and it gets them back afterwards.
    Session& self = const_cast<Session&>(*this);
    bool lent = false;
    for (const TrackInfo& track : tracks_) {
        lent = lent || track.plugin_instrument.plugin || !track.effects.empty();
    }
    struct Return {
        Session& session;
        bool active;
        ~Return() {
            if (active) {
                for (const TrackInfo& track : session.tracks_) {
                    const TrackPlugins plugins = track_plugins(track);
                    if (plugins.instrument) {
                        plugins.instrument->reset();
                    }
                    for (const auto& effect : plugins.effects) {
                        effect->reset();
                    }
                }
                session.sync_engine_tracks(0);
            }
        }
    };
    if (lent) {
        self.engine_.stop();
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            self.engine_.set_track_regions(static_cast<std::uint32_t>(i), engine_regions(tracks_[i]));
        }
        if (device_.running()) {
            // Wait until the audio thread has let go of the plugins.
            const std::int64_t token = ++self.sync_token_;
            self.engine_.request_sync(token);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (engine_.synced() < token && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (engine_.synced() < token) {
                self.sync_engine_tracks(0);
                return false;  // the sound device does not answer
            }
        }
    }
    const auto offline = std::make_unique<core::Engine>(config_.sample_rate, config_.channels);
    const Return give_back{self, lent};  // at the end the playing engine gets the plugins back
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const TrackInfo& track = tracks_[i];
        const auto slot = static_cast<std::uint32_t>(i);
        const TrackPlugins plugins = track_plugins(track);
        if (plugins.instrument) {
            plugins.instrument->reset();
        }
        for (const auto& effect : plugins.effects) {
            effect->reset();
        }
        offline->set_track_regions(slot, engine_regions(track), plugins.effects, plugins.instrument,
                                   plugins.latency, track.volume_curve, track.pan_curve, plugins.params);
        offline->set_track_gain(slot, track.gain);
        offline->set_track_pan(slot, track.pan);
        offline->set_track_mute(slot, silenced(track));
        offline->set_track_solo(slot, track.solo);
        offline->set_track_output(slot, track.group);
    }
    for (std::size_t g = 0; g < groups_.size(); ++g) {
        offline->set_bus_gain(static_cast<std::uint32_t>(g), groups_[g].gain);
        offline->set_bus_mute(static_cast<std::uint32_t>(g), groups_[g].mute);
    }
    offline->set_gain(master_gain_);
    offline->set_blocking_reads(true);  // an export may wait for the disk
    offline->set_soundfont(soundfont_);
    offline->play(false);  // no fade-in: the file starts exactly like the project

    const std::uint32_t block = 4096;
    std::vector<float> buffer(static_cast<std::size_t>(block) * config_.channels);
    std::int64_t remaining = length_frames_;
    while (remaining > 0) {
        const auto frames = static_cast<std::uint32_t>(std::min<std::int64_t>(remaining, block));
        offline->render(buffer.data(), frames);
        if (!sink(buffer.data(), frames)) {
            return false;
        }
        remaining -= frames;
    }
    return true;
}

// ---- Grid --------------------------------------------------------------------------------

double Session::beat_frames() const noexcept {
    return static_cast<double>(config_.sample_rate) * 60.0 / metronome_.bpm * 4.0 /
           static_cast<double>(metronome_.beat_unit);
}

double Session::bar_frames() const noexcept {
    return beat_frames() * static_cast<double>(metronome_.beats_per_bar);
}

std::int64_t Session::snap_frame(std::int64_t frame) const noexcept {
    if (!snap_ || grid_step_ == GridStep::None) {
        return frame;
    }
    // The grid is counted in beats, so it follows the tempo map.
    double step = 1.0;
    switch (grid_step_) {
        case GridStep::Bar:
            step = static_cast<double>(metronome_.beats_per_bar);
            break;
        case GridStep::Beat:
            break;
        case GridStep::HalfBeat:
            step *= 0.5;
            break;
        case GridStep::QuarterBeat:
            step *= 0.25;
            break;
        case GridStep::None:
            break;
    }
    if (!(step > 0.0)) {
        return frame;
    }
    const double snapped = std::round(beats_at(frame) / step) * step;
    return std::max<std::int64_t>(std::llround(frame_at(snapped)), 0);
}

// ---- Master and transport ----------------------------------------------------------------

bool Session::set_master_gain(float linear) {
    if (!engine_.set_gain(linear)) {
        return false;
    }
    master_gain_ = linear;
    return true;
}

void Session::play() {
    if (!engine_.is_playing()) {
        prefetch_now(engine_.position_frames());  // the first pages are there from the start
    }
    engine_.play();
}

void Session::pause() { engine_.pause(); }

void Session::toggle_play() {
    if (engine_.is_playing()) {
        engine_.pause();
    } else {
        play();
    }
}

void Session::stop() { engine_.stop(); }

void Session::seek_frames(std::int64_t frame) {
    const std::int64_t target = std::clamp<std::int64_t>(frame, 0, length_frames_);
    engine_.seek(target);
    if (engine_.is_playing()) {
        prefetch_now(target);  // a jump during playback should not leave a gap
    }
    {
        const std::lock_guard<std::mutex> lock(loader_mutex_);
        loader_hint_ = target;
        loader_wake_ = true;
    }
    loader_cv_.notify_one();
}

void Session::set_loop(bool enabled) {
    if (engine_.set_loop(enabled, loop_start_, loop_end_ > loop_start_ ? loop_end_ : -1)) {
        loop_ = enabled;
        update_streaming();
    }
}

void Session::set_loop_range(std::int64_t start, std::int64_t end) {
    loop_start_ = end > start ? std::max<std::int64_t>(start, 0) : 0;
    loop_end_ = end > start ? end : -1;
    set_loop(loop_);
}

void Session::set_reverse(bool reverse) {
    if (engine_.set_reverse(reverse)) {
        reverse_ = reverse;
        update_streaming();
    }
}

bool Session::set_speed(double speed) {
    return engine_.set_speed(std::clamp(speed, core::Engine::kMinSpeed, core::Engine::kMaxSpeed));
}

void Session::faster() { set_speed(std::round(speed() * 100.0 + 5.0) / 100.0); }

void Session::slower() { set_speed(std::round(speed() * 100.0 - 5.0) / 100.0); }

void Session::toggle_reverse() {
    set_reverse(!reverse_);
    if (reverse_starts_playback_) {
        play();  // no effect when it is already playing
    }
}

bool Session::set_metronome(const MetronomeSettings& settings) {
    if (settings.beats_per_bar < 1 || settings.beat_unit < 1 ||
        !engine_.set_metronome(settings.enabled, settings.bpm,
                               static_cast<std::uint32_t>(settings.beats_per_bar),
                               static_cast<std::uint32_t>(settings.beat_unit), settings.volume)) {
        return false;
    }
    metronome_ = settings;
    rebuild_tempo();
    return true;
}

void Session::rebuild_tempo() {
    tempo_map_ = std::make_shared<const core::TempoMap>(static_cast<double>(config_.sample_rate),
                                                        metronome_.bpm, metronome_.beat_unit, tempo_changes_);
    engine_.set_tempo_map(tempo_changes_.empty() ? nullptr : tempo_map_);
}

bool Session::set_tempo_change(std::int64_t frame, double bpm) {
    if (!(bpm >= core::TempoMap::kMinBpm && bpm <= core::TempoMap::kMaxBpm)) {
        return false;
    }
    if (frame <= 0) {
        MetronomeSettings settings = metronome_;
        settings.bpm = bpm;
        return set_metronome(settings);
    }
    auto at = tempo_changes_.begin();
    while (at != tempo_changes_.end() && at->frame < frame) {
        ++at;
    }
    if (at != tempo_changes_.end() && at->frame == frame) {
        at->bpm = bpm;
    } else {
        tempo_changes_.insert(at, core::TempoChange{frame, bpm});
    }
    rebuild_tempo();
    return true;
}

bool Session::rewrite_region_sound(RegionInfo& region, const std::function<void(std::vector<float>&)>& process,
                                   double length_factor, std::string* error) {
    const std::size_t channels = config_.channels;
    const std::int64_t count = region.source_frames();
    if (count <= 0 || count > static_cast<std::int64_t>(config_.sample_rate) * 60 * 30) {
        set_error(error, "the region is empty or longer than 30 minutes");
        return false;
    }
    std::vector<float> data(static_cast<std::size_t>(count) * channels);
    const std::size_t got =
        core::read_clip_frames(*region.source->clip, region.offset, static_cast<std::size_t>(count), data.data());
    data.resize(got * channels);
    process(data);
    if (data.size() < channels) {
        set_error(error, "nothing was left of the sound");
        return false;
    }
    CacheWriter writer;
    const std::filesystem::path cache = new_cache_file("edit");
    if (cache.empty() || !writer.open(cache, config_.channels, config_.sample_rate) ||
        !writer.write(data.data(), data.size() / channels)) {
        writer.discard();
        set_error(error, "cannot write a cache file (is the disk full?)");
        return false;
    }
    Overview overview;
    auto clip = writer.finish(&overview, error);
    if (!clip) {
        return false;
    }
    auto source = std::make_shared<Source>();
    source->name = region.source->name;
    source->overview = std::move(overview);
    source->clip = std::move(clip);
    const std::int64_t frames = source->clip->frames();
    region.source = std::move(source);
    region.offset = 0;
    if (length_factor != 1.0) {
        const auto scale = [length_factor](std::int64_t value) -> std::int64_t {
            return static_cast<std::int64_t>(std::llround(static_cast<double>(value) * length_factor));
        };
        region.length = std::max<std::int64_t>(scale(region.length), 1);
        region.fade_in = std::min(scale(region.fade_in), region.length);
        region.fade_out = std::min(scale(region.fade_out), region.length);
    }
    // The region must stay inside its new sound.
    const auto longest = static_cast<std::int64_t>(std::floor(static_cast<double>(frames) / region.rate));
    region.length = std::max<std::int64_t>(std::min(region.length, longest), 1);
    return true;
}

bool Session::edit_region_sound(std::size_t track, std::size_t region, SoundEdit edit, double amount,
                                std::string* error) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr || info->has_notes()) {
        set_error(error, "choose a region with recorded sound");
        return false;
    }
    const std::size_t channels = config_.channels;
    std::function<void(std::vector<float>&)> process;
    double length_factor = 1.0;
    const char* label = "Edit the sound";
    switch (edit) {
        case SoundEdit::Normalize:
            label = "Normalize";
            process = [](std::vector<float>& data) {
                float peak = 0.0f;
                for (const float value : data) {
                    peak = std::max(peak, std::fabs(value));
                }
                if (peak > 0.0f) {
                    const float gain = 0.98f / peak;
                    for (float& value : data) {
                        value *= gain;
                    }
                }
            };
            break;
        case SoundEdit::Louder:
        case SoundEdit::Quieter: {
            label = edit == SoundEdit::Louder ? "Louder" : "Quieter";
            const float gain = edit == SoundEdit::Louder ? 1.41253754f : 0.70794578f;  // +-3 dB
            process = [gain](std::vector<float>& data) {
                for (float& value : data) {
                    value *= gain;
                }
            };
            break;
        }
        case SoundEdit::Reverse:
            label = "Reverse";
            process = [channels](std::vector<float>& data) {
                const std::size_t frames = data.size() / channels;
                for (std::size_t i = 0; i < frames / 2; ++i) {
                    for (std::size_t c = 0; c < channels; ++c) {
                        std::swap(data[i * channels + c], data[(frames - 1 - i) * channels + c]);
                    }
                }
            };
            break;
        case SoundEdit::Invert:
            label = "Invert the polarity";
            process = [](std::vector<float>& data) {
                for (float& value : data) {
                    value = -value;
                }
            };
            break;
        case SoundEdit::Silence:
            label = "Silence";
            process = [](std::vector<float>& data) { std::fill(data.begin(), data.end(), 0.0f); };
            break;
        case SoundEdit::Stretch:
            if (!(amount >= 0.25 && amount <= 4.0)) {
                set_error(error, "the stretch must be from 25% to 400%");
                return false;
            }
            label = "Stretch";
            length_factor = amount;
            process = [channels, amount](std::vector<float>& data) {
                data = core::time_stretch(data.data(), data.size() / channels, channels, amount);
            };
            break;
        case SoundEdit::Pitch:
            if (!(amount >= -24.0 && amount <= 24.0)) {
                set_error(error, "the pitch can move by 24 semitones at most");
                return false;
            }
            label = "Change the pitch";
            process = [channels, amount](std::vector<float>& data) {
                data = core::pitch_shift(data.data(), data.size() / channels, channels, amount);
            };
            break;
    }
    if (!rewrite_region_sound(*info, process, length_factor, error)) {
        return false;
    }
    pending_label_ = label;
    sync_track_regions(track);
    update_length();
    return true;
}

bool Session::change_track_speed(std::size_t index, double factor, std::string* error, bool keep_pitch) {
    if (index >= tracks_.size()) {
        set_error(error, "the track does not exist");
        return false;
    }
    if (!(factor >= 0.1 && factor <= 10.0)) {
        set_error(error, "the speed must be from 10% to 1000%");
        return false;
    }
    TrackInfo& track = tracks_[index];
    if (track.regions.empty() || factor == 1.0) {
        return true;
    }
    for (const RegionInfo& region : track.regions) {
        const double rate = region.rate * factor;
        if (!keep_pitch && !region.has_notes() &&
            !(rate >= core::Engine::kMinSpeed - 1e-9 && rate <= core::Engine::kMaxSpeed + 1e-9)) {
            set_error(error, "a region would play its sound slower than 25% or faster than 400%");
            return false;
        }
    }
    std::int64_t anchor = track.regions.front().start;
    for (const RegionInfo& region : track.regions) {
        anchor = std::min(anchor, region.start);
    }
    // Times from the anchor on are squeezed or stretched; earlier ones stay.
    // (std::int64_t is long on Linux and long long on Windows: the casts keep one type.)
    const auto map = [anchor, factor](std::int64_t frame) -> std::int64_t {
        return frame <= anchor
                   ? frame
                   : anchor + static_cast<std::int64_t>(std::llround(static_cast<double>(frame - anchor) / factor));
    };
    const auto scale = [factor](std::int64_t frames) -> std::int64_t {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(frames) / factor));
    };
    for (RegionInfo& region : track.regions) {
        const std::int64_t new_start = map(region.start);
        const std::int64_t new_end = std::max(map(region.end()), new_start + 1);
        if (region.has_notes()) {
            // Notes keep their pitch: their times change instead.
            std::vector<core::Note> notes = region.source->clip->notes->notes;
            for (core::Note& note : notes) {
                note.start = scale(note.start);
                note.length = std::max<std::int64_t>(scale(note.length), 1);
            }
            const std::int64_t clip_frames = std::max<std::int64_t>(scale(region.source->clip->frames()), 1);
            region.offset = std::min<std::int64_t>(scale(region.offset), clip_frames - 1);
            region.source = note_source(region.source->name, std::move(notes), clip_frames, config_.channels,
                                        config_.sample_rate);
            region.length = std::min(new_end - new_start, clip_frames - region.offset);
        } else if (keep_pitch) {
            // The same pitch: the sound itself is made longer or shorter.
            const std::size_t channels = config_.channels;
            const double stretch = 1.0 / factor;
            if (!rewrite_region_sound(
                    region,
                    [channels, stretch](std::vector<float>& data) {
                        data = core::time_stretch(data.data(), data.size() / channels, channels, stretch);
                    },
                    1.0, error)) {
                return false;
            }
            region.length = std::max<std::int64_t>(std::min(new_end - new_start,
                                                            static_cast<std::int64_t>(std::floor(
                                                                static_cast<double>(region.source->clip->frames()) /
                                                                region.rate))),
                                                   1);
        } else {
            region.rate = std::clamp(region.rate * factor, core::Engine::kMinSpeed, core::Engine::kMaxSpeed);
            region.length = new_end - new_start;
            // It must stay inside its sound.
            const auto available = static_cast<double>(region.source->clip->frames() - region.offset);
            const auto longest = static_cast<std::int64_t>(std::floor(available / region.rate));
            region.length = std::max<std::int64_t>(std::min(region.length, longest), 1);
        }
        region.start = new_start;
        region.fade_in = std::min(scale(region.fade_in), region.length);
        region.fade_out = std::min(scale(region.fade_out), region.length);
    }
    const auto map_points = [&map](std::vector<core::CurvePoint>& points) {
        for (core::CurvePoint& point : points) {
            point.frame = map(point.frame);
        }
    };
    map_points(track.volume_curve);
    map_points(track.pan_curve);
    for (ParamCurveInfo& curve : track.param_curves) {
        map_points(curve.points);
    }
    pending_label_ = factor > 1.0 ? "Speed up a track" : "Slow down a track";
    sync_track_regions(index);
    update_length();
    return true;
}

bool Session::comp_range(const Range& range, std::size_t track) {
    if (range.empty() || range.first_track >= tracks_.size() || track >= tracks_.size() ||
        track == range.first_track) {
        return false;
    }
    Range from = range;
    from.last_track = from.first_track;
    const Clipboard copied = copy_range(from);
    if (copied.empty()) {
        return false;
    }
    Range to = from;
    to.first_track = track;
    to.last_track = track;
    cut_range(to);
    const bool pasted = paste(copied, track, range.begin);
    pending_label_ = "Use a take";  // after paste(), which names its own step
    std::stable_sort(tracks_[track].regions.begin(), tracks_[track].regions.end(),
                     [](const RegionInfo& a, const RegionInfo& b) { return a.start < b.start; });
    sync_track_regions(track);
    return pasted;
}

void Session::sort_markers() {
    std::stable_sort(markers_.begin(), markers_.end(),
                     [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
}

std::size_t Session::add_marker(std::int64_t frame, std::string name) {
    Marker marker;
    marker.frame = std::max<std::int64_t>(frame, 0);
    marker.name = one_line(name);
    auto at = std::upper_bound(markers_.begin(), markers_.end(), marker.frame,
                               [](std::int64_t f, const Marker& m) { return f < m.frame; });
    at = markers_.insert(at, std::move(marker));
    return static_cast<std::size_t>(at - markers_.begin());
}

bool Session::remove_marker(std::size_t index) {
    if (index >= markers_.size()) {
        return false;
    }
    markers_.erase(markers_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool Session::rename_marker(std::size_t index, std::string name) {
    if (index >= markers_.size()) {
        return false;
    }
    markers_[index].name = one_line(name);
    return true;
}

std::size_t Session::move_marker(std::size_t index, std::int64_t frame) {
    if (index >= markers_.size()) {
        return index;
    }
    Marker marker = markers_[index];
    if (marker.frame == std::max<std::int64_t>(frame, 0)) {
        return index;
    }
    markers_.erase(markers_.begin() + static_cast<std::ptrdiff_t>(index));
    return add_marker(frame, std::move(marker.name));
}

std::int64_t Session::previous_marker_frame(std::int64_t frame) const noexcept {
    std::int64_t found = -1;
    for (const Marker& marker : markers_) {
        if (marker.frame < frame) {
            found = marker.frame;
        }
    }
    return found;
}

std::int64_t Session::next_marker_frame(std::int64_t frame) const noexcept {
    for (const Marker& marker : markers_) {
        if (marker.frame > frame) {
            return marker.frame;
        }
    }
    return -1;
}

bool Session::remove_tempo_change(std::size_t index) {
    if (index >= tempo_changes_.size()) {
        return false;
    }
    tempo_changes_.erase(tempo_changes_.begin() + static_cast<std::ptrdiff_t>(index));
    rebuild_tempo();
    return true;
}

double Session::bpm_at(std::int64_t frame) const noexcept {
    return tempo_map_ ? tempo_map_->bpm_at(static_cast<double>(frame)) : metronome_.bpm;
}

double Session::beats_at(std::int64_t frame) const noexcept {
    return tempo_map_ ? tempo_map_->beats_at(static_cast<double>(frame))
                      : static_cast<double>(frame) / beat_frames();
}

double Session::frame_at(double beats) const noexcept {
    return tempo_map_ ? tempo_map_->frame_at(beats) : beats * beat_frames();
}

double Session::beat_frames_at(std::int64_t frame) const noexcept {
    return tempo_map_ ? tempo_map_->beat_frames_at(static_cast<double>(frame)) : beat_frames();
}

}  // namespace toh::app
