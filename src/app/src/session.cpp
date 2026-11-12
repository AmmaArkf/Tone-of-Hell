#include "toh/app/session.h"

#include <algorithm>
#include <utility>

#include "toh/audio/clip_loader.h"

namespace toh::app {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void render_callback(void* user_data, float* out, std::uint32_t frames) {
    static_cast<core::Engine*>(user_data)->render(out, frames);
}

}  // namespace

// ---- Overview ----------------------------------------------------------------------------

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

// ---- Session -----------------------------------------------------------------------------

Session::Session(const audio::DeviceConfig& config)
    : config_(config), engine_(config.sample_rate, config.channels) {}

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
}

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
    track.name = std::move(name);
    track.overview = make_overview(*clip);
    track.clip = std::move(clip);
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
    sync_engine_tracks(index);  // every later track moves up one slot
    update_length();
    return true;
}

// Copies tracks_[first..] into the engine's track slots and empties the slot after the last one.
void Session::sync_engine_tracks(std::size_t first) {
    for (std::size_t i = first; i < tracks_.size(); ++i) {
        const TrackInfo& track = tracks_[i];
        const auto slot = static_cast<std::uint32_t>(i);
        engine_.set_track_clip(slot, track.clip);
        engine_.set_track_gain(slot, track.gain);
        engine_.set_track_pan(slot, track.pan);
        engine_.set_track_mute(slot, track.mute);
        engine_.set_track_solo(slot, track.solo);
    }
    if (tracks_.size() < core::Engine::kMaxTracks) {
        const auto slot = static_cast<std::uint32_t>(tracks_.size());
        engine_.clear_track(slot);
        engine_.set_track_gain(slot, 1.0f);
        engine_.set_track_pan(slot, 0.0f);
        engine_.set_track_mute(slot, false);
        engine_.set_track_solo(slot, false);
    }
}

void Session::update_length() {
    length_frames_ = 0;
    for (const TrackInfo& track : tracks_) {
        length_frames_ = std::max(length_frames_, track.clip->frames());
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
