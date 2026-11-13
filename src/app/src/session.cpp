#include "toh/app/session.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "toh/audio/clip_loader.h"
#include "toh/audio/mp3_writer.h"

namespace toh::app {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void render_callback(void* user_data, const float* in, float* out, std::uint32_t frames) {
    static_cast<core::Engine*>(user_data)->render(in, out, frames);
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
        regions.push_back(std::move(region));
    }
    return regions;
}

// Keeps the fades of a region inside its length.
void limit_fades(RegionInfo& region) {
    region.fade_in = std::clamp<std::int64_t>(region.fade_in, 0, region.length);
    region.fade_out = std::clamp<std::int64_t>(region.fade_out, 0, region.length - region.fade_in);
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
    if (!clip || last_frame < first_frame) {
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
      record_offset_(2 * static_cast<std::int64_t>(config.buffer_frames)) {}

Session::~Session() {
    stop_audio();  // the audio callback must not run while the engine is destroyed
}

bool Session::start_audio(std::string* error) {
    return device_.start(config_, &render_callback, &engine_, error);
}

void Session::stop_audio() { device_.stop(); }

void Session::update() {
    if (!device_.running()) {
        // No audio thread is running: apply the queued control messages here instead.
        engine_.render(nullptr, 0);
    }
    engine_.collect_retired();

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
    auto clip = audio::load_clip(path, config_.sample_rate, config_.channels, error);
    if (!clip) {
        return false;
    }
    return add_track(path.filename().u8string(), std::move(clip), error);
}

bool Session::add_track(std::string name, std::shared_ptr<const core::ClipData> clip,
                        std::string* error) {
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
        source->overview = make_overview(*clip);
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
        regions.push_back(std::move(region));
    }
    engine_.set_track_regions(static_cast<std::uint32_t>(index), regions);
}

// Copies tracks_[first..] into the engine's track slots and empties the slot after the last one.
void Session::sync_engine_tracks(std::size_t first) {
    for (std::size_t i = first; i < tracks_.size(); ++i) {
        const TrackInfo& track = tracks_[i];
        const auto slot = static_cast<std::uint32_t>(i);
        sync_track_regions(i);
        engine_.set_track_gain(slot, track.gain);
        engine_.set_track_pan(slot, track.pan);
        engine_.set_track_mute(slot, track.mute);
        engine_.set_track_solo(slot, track.solo);
    }
    if (tracks_.size() < core::Engine::kMaxTracks) {
        const auto slot = static_cast<std::uint32_t>(tracks_.size());
        engine_.set_track_regions(slot, {});
        engine_.set_track_gain(slot, 1.0f);
        engine_.set_track_pan(slot, 0.0f);
        engine_.set_track_mute(slot, false);
        engine_.set_track_solo(slot, false);
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

bool Session::set_track_pan(std::size_t index, float pan) {
    if (index >= tracks_.size() || !engine_.set_track_pan(static_cast<std::uint32_t>(index), pan)) {
        return false;
    }
    tracks_[index].pan = pan;
    return true;
}

bool Session::set_track_mute(std::size_t index, bool mute) {
    if (index >= tracks_.size() ||
        !engine_.set_track_mute(static_cast<std::uint32_t>(index), mute)) {
        return false;
    }
    tracks_[index].mute = mute;
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

bool Session::trim_region_start(std::size_t track, std::size_t region, std::int64_t start) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr) {
        return false;
    }
    // Not before the start of the source or of the timeline, and at least one frame must stay.
    const std::int64_t earliest = std::max<std::int64_t>(info->start - info->offset, 0);
    const std::int64_t latest = info->end() - 1;
    const std::int64_t new_start = std::clamp(start, earliest, latest);
    const std::int64_t delta = new_start - info->start;
    info->start = new_start;
    info->offset += delta;
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
    const std::int64_t available = info->source->clip->frames() - info->offset;
    info->length = std::clamp<std::int64_t>(end - info->start, 1, available);
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

bool Session::split_region(std::size_t track, std::size_t region, std::int64_t frame) {
    RegionInfo* info = find_region(track, region);
    if (info == nullptr || frame <= info->start || frame >= info->end()) {
        return false;
    }
    RegionInfo second = *info;
    second.start = frame;
    second.offset = info->offset + (frame - info->start);
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
    if (!device_.start(config_, &render_callback, &engine_, error)) {
        return false;
    }
    if (enabled && !device_.has_input()) {
        config_.capture = false;
        set_error(error, "no input device could be opened");
        return false;
    }
    return true;
}

bool Session::set_devices(const std::string& output, const std::string& input,
                          std::string* error) {
    config_.output_name = output;
    config_.input_name = input;
    if (!device_.running()) {
        return true;  // takes effect when the audio is started
    }
    device_.stop();
    return device_.start(config_, &render_callback, &engine_, error);
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
    return static_cast<std::int64_t>(take_.size() / config_.channels);
}

bool Session::start_recording(std::size_t track) {
    if (recording_) {
        return false;
    }
    if (track >= tracks_.size()) {
        if (tracks_.size() >= core::Engine::kMaxTracks) {
            return false;
        }
        TrackInfo info;
        info.name = "Recording " + std::to_string(tracks_.size() + 1);
        tracks_.push_back(std::move(info));
        sync_engine_tracks(tracks_.size() - 1);
        track = tracks_.size() - 1;
    }

    take_.clear();
    drain_recording();  // throw away input left over from an earlier take
    take_.clear();

    record_track_ = track;
    record_seen_ = false;
    stop_requested_ = false;
    recording_ = true;
    engine_.start_recording();
    engine_.play();
    return true;
}

void Session::stop_recording() {
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
        take_.insert(take_.end(), drain_buffer_.begin(),
                     drain_buffer_.begin() + static_cast<std::ptrdiff_t>(frames * channels));
    }
}

// Turns the finished take into a region on the recorded track.
void Session::finish_take() {
    drain_recording();
    recording_ = false;

    const std::int64_t start = engine_.record_start_frame();
    const std::size_t channels = config_.channels;
    if (take_.size() < channels || start < 0 || record_track_ >= tracks_.size()) {
        take_.clear();
        return;  // nothing was recorded, or the track was removed meanwhile
    }

    auto clip = std::make_shared<core::ClipData>();
    clip->channels = config_.channels;
    clip->sample_rate = config_.sample_rate;
    clip->samples = std::move(take_);
    take_.clear();

    auto source = std::make_shared<Source>();
    source->name = "Take " + std::to_string(++take_count_);
    source->overview = make_overview(*clip);
    source->clip = std::move(clip);

    RegionInfo region;
    region.length = source->clip->frames();
    region.source = std::move(source);
    // Place the take earlier by the time the sound needed to pass through the sound card.
    region.start = start - record_offset_;
    if (region.start < 0) {
        const std::int64_t cut = std::min(-region.start, region.length - 1);
        region.offset = cut;
        region.length -= cut;
        region.start = 0;
    }
    tracks_[record_track_].regions.push_back(std::move(region));
    sync_track_regions(record_track_);
    update_length();
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
    // A second engine renders the same project offline, so playback is not disturbed.
    const auto offline = std::make_unique<core::Engine>(config_.sample_rate, config_.channels);
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const TrackInfo& track = tracks_[i];
        const auto slot = static_cast<std::uint32_t>(i);
        offline->set_track_regions(slot, engine_regions(track));
        offline->set_track_gain(slot, track.gain);
        offline->set_track_pan(slot, track.pan);
        offline->set_track_mute(slot, track.mute);
        offline->set_track_solo(slot, track.solo);
    }
    offline->set_gain(master_gain_);
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
    double step = beat_frames();
    switch (grid_step_) {
        case GridStep::Bar:
            step = bar_frames();
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
    if (!(step >= 1.0)) {
        return frame;
    }
    const double snapped = std::round(static_cast<double>(frame) / step) * step;
    return std::max<std::int64_t>(std::llround(snapped), 0);
}

// ---- Master and transport ----------------------------------------------------------------

bool Session::set_master_gain(float linear) {
    if (!engine_.set_gain(linear)) {
        return false;
    }
    master_gain_ = linear;
    return true;
}

void Session::play() { engine_.play(); }

void Session::pause() { engine_.pause(); }

void Session::toggle_play() {
    if (engine_.is_playing()) {
        engine_.pause();
    } else {
        engine_.play();
    }
}

void Session::stop() { engine_.stop(); }

void Session::seek_frames(std::int64_t frame) {
    engine_.seek(std::clamp<std::int64_t>(frame, 0, length_frames_));
}

void Session::set_loop(bool enabled) {
    if (engine_.set_loop(enabled)) {
        loop_ = enabled;
    }
}

void Session::set_reverse(bool reverse) {
    if (engine_.set_reverse(reverse)) {
        reverse_ = reverse;
    }
}

void Session::toggle_reverse() {
    set_reverse(!reverse_);
    engine_.play();  // no effect when it is already playing
}

bool Session::set_metronome(const MetronomeSettings& settings) {
    if (settings.beats_per_bar < 1 || settings.beat_unit < 1 ||
        !engine_.set_metronome(settings.enabled, settings.bpm,
                               static_cast<std::uint32_t>(settings.beats_per_bar),
                               static_cast<std::uint32_t>(settings.beat_unit), settings.volume)) {
        return false;
    }
    metronome_ = settings;
    return true;
}

}  // namespace toh::app
