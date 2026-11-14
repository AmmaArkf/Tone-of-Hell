#include "toh/core/engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace toh::core {
namespace {

constexpr double kFadeSeconds = 0.005;           // click-free play, pause, stop and seek
constexpr double kGainSmoothingSeconds = 0.010;  // click-free fader, pan, mute and solo changes
constexpr double kMeterReleaseSeconds = 0.3;     // how fast the peak meters fall back
constexpr float kMaxGain = 8.0f;
constexpr double kClickSeconds = 0.030;       // length of one metronome click
constexpr double kClickDecaySeconds = 0.008;  // how fast it dies away
constexpr double kClickFrequency = 880.0;
constexpr double kClickAccentFrequency = 1760.0;  // first beat of the bar
constexpr double kTwoPi = 6.283185307179586476925286766559;

bool valid_gain(float linear) noexcept {
    return linear >= 0.0f && linear <= kMaxGain;  // false for NaN as well
}

// Moves `current` a small step towards `target`; lands exactly on it when close enough.
inline void smooth(float& current, float target, float coef) noexcept {
    const float difference = target - current;
    if (difference > -1.0e-6f && difference < 1.0e-6f) {
        current = target;
    } else {
        current += difference * coef;
    }
}

inline float decayed_meter(float meter, float decay, float peak) noexcept {
    const float value = std::max(peak, meter * decay);
    return value < 1.0e-9f ? 0.0f : value;
}

}  // namespace

Engine::Engine(std::uint32_t sample_rate, std::uint32_t channels)
    : sample_rate_(std::max<std::uint32_t>(sample_rate, 1)),
      channels_(std::max<std::uint32_t>(channels, 1)),
      recorded_(static_cast<std::size_t>(sample_rate_) * channels_ * 4) {  // about 4 seconds
    const double rate = static_cast<double>(sample_rate_);
    fade_frames_ =
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(kFadeSeconds * rate)));
    fade_step_ = 1.0f / static_cast<float>(fade_frames_);
    gain_coef_ = static_cast<float>(1.0 - std::exp(-1.0 / (kGainSmoothingSeconds * rate)));

    const std::size_t block_samples = static_cast<std::size_t>(kBlockFrames) * channels_;
    frame_pos_.assign(kBlockFrames, -1);
    shifted_pos_.assign(kBlockFrames, -1);
    track_pos_ = frame_pos_.data();
    frame_fade_.assign(kBlockFrames, 0.0f);
    frame_frac_.assign(kBlockFrames, 0.0f);
    master_mix_.assign(block_samples, 0.0f);
    track_mix_.assign(block_samples, 0.0f);
    input_frame_.assign(channels_, 0.0f);
    bus_mix_.assign(block_samples * kMaxBuses, 0.0f);

    for (auto& peak : track_peaks_) {
        peak.store(0.0f, std::memory_order_relaxed);
    }
    for (auto& peak : bus_peaks_) {
        peak.store(0.0f, std::memory_order_relaxed);
    }
}

// ---- Control thread ----------------------------------------------------------------------

// Hands a track's new content to the audio thread. The engine keeps owning it (and through it
// the clips) until the audio thread has let go of it; see collect_retired.
bool Engine::push_content(std::uint32_t track, std::unique_ptr<TrackContent> content, bool stop) {
    Command command(CommandType::SetTrackContent);
    command.index = static_cast<std::int32_t>(track);
    command.flag = stop;
    command.content = content.get();
    const bool owned = content != nullptr;
    if (owned) {
        owned_.push_back(std::move(content));
    }
    if (!commands_.try_push(command)) {
        if (owned) {
            owned_.pop_back();
        }
        return false;
    }
    return true;
}

bool Engine::set_track_regions(std::uint32_t track, const std::vector<Region>& regions,
                               const std::vector<std::shared_ptr<AudioEffect>>& effects,
                               std::shared_ptr<AudioEffect> instrument, std::int64_t latency,
                               std::vector<CurvePoint> volume, std::vector<CurvePoint> pan,
                               std::vector<ParamCurve> params) {
    if (track >= kMaxTracks) {
        return false;
    }
    if (regions.empty() && effects.empty() && !instrument) {
        return push_content(track, nullptr, false);
    }

    constexpr std::int64_t kMaxStart = std::int64_t{1} << 40;  // far beyond any real project
    auto content = std::make_unique<TrackContent>();
    content->instrument = std::move(instrument);
    content->latency = std::clamp<std::int64_t>(latency, 0, static_cast<std::int64_t>(sample_rate_) * 2);
    std::stable_sort(volume.begin(), volume.end(),
                     [](const CurvePoint& a, const CurvePoint& b) { return a.frame < b.frame; });
    content->volume = std::move(volume);
    std::stable_sort(pan.begin(), pan.end(),
                     [](const CurvePoint& a, const CurvePoint& b) { return a.frame < b.frame; });
    content->pan = std::move(pan);
    for (ParamCurve& curve : params) {
        if (curve.effect && !curve.points.empty()) {
            std::stable_sort(curve.points.begin(), curve.points.end(),
                             [](const CurvePoint& a, const CurvePoint& b) { return a.frame < b.frame; });
            curve.sent = false;
            content->params.push_back(std::move(curve));
        }
    }
    for (const auto& effect : effects) {
        if (effect) {
            content->effects.push_back(effect);
        }
    }
    content->regions.reserve(regions.size());
    for (const Region& region : regions) {
        if (!region.clip || region.clip->channels != channels_ ||
            region.clip->sample_rate != sample_rate_ || region.start < 0 ||
            region.start > kMaxStart || region.offset < 0 || region.length <= 0 ||
            !(region.rate >= kMinSpeed && region.rate <= kMaxSpeed) ||
            region.offset > region.clip->frames() - clip_frames_used(region.length, region.rate)) {
            return false;
        }
        Region copy = region;
        copy.fade_in = std::clamp<std::int64_t>(copy.fade_in, 0, copy.length);
        copy.fade_out = std::clamp<std::int64_t>(copy.fade_out, 0, copy.length - copy.fade_in);
        content->end = std::max(content->end, copy.start + copy.length);
        content->regions.push_back(std::move(copy));
    }
    return push_content(track, std::move(content), false);
}

bool Engine::set_track_clip(std::uint32_t track, std::shared_ptr<const ClipData> clip) {
    if (track >= kMaxTracks || !clip || clip->channels != channels_ ||
        clip->sample_rate != sample_rate_) {
        return false;
    }
    const std::int64_t frames = clip->frames();
    if (frames <= 0) {
        return push_content(track, nullptr, true);
    }
    auto content = std::make_unique<TrackContent>();
    Region region;
    region.clip = std::move(clip);
    region.length = frames;
    content->end = frames;
    content->regions.push_back(std::move(region));
    return push_content(track, std::move(content), true);
}

bool Engine::clear_track(std::uint32_t track) {
    if (track >= kMaxTracks) {
        return false;
    }
    return push_content(track, nullptr, true);
}

bool Engine::set_track_gain(std::uint32_t track, float linear) {
    if (track >= kMaxTracks || !valid_gain(linear)) {
        return false;
    }
    Command command(CommandType::SetTrackGain);
    command.index = static_cast<std::int32_t>(track);
    command.value = linear;
    return commands_.try_push(command);
}

bool Engine::set_track_pan(std::uint32_t track, float pan) {
    if (track >= kMaxTracks || !(pan >= -1.0f && pan <= 1.0f)) {
        return false;
    }
    // Constant-power curve, scaled so that the centre leaves both channels at full level.
    constexpr double kQuarterPi = 0.78539816339744830962;
    constexpr double kSqrt2 = 1.41421356237309504880;
    const double angle = (static_cast<double>(pan) + 1.0) * kQuarterPi;
    Command command(CommandType::SetTrackPan);
    command.index = static_cast<std::int32_t>(track);
    command.value = static_cast<float>(std::min(1.0, kSqrt2 * std::cos(angle)));
    command.value2 = static_cast<float>(std::min(1.0, kSqrt2 * std::sin(angle)));
    if (pan == -1.0f) {
        command.value2 = 0.0f;
    } else if (pan == 1.0f) {
        command.value = 0.0f;
    }
    return commands_.try_push(command);
}

bool Engine::set_track_mute(std::uint32_t track, bool mute) {
    if (track >= kMaxTracks) {
        return false;
    }
    Command command(CommandType::SetTrackMute);
    command.index = static_cast<std::int32_t>(track);
    command.flag = mute;
    return commands_.try_push(command);
}

bool Engine::set_track_solo(std::uint32_t track, bool solo) {
    if (track >= kMaxTracks) {
        return false;
    }
    Command command(CommandType::SetTrackSolo);
    command.index = static_cast<std::int32_t>(track);
    command.flag = solo;
    return commands_.try_push(command);
}

bool Engine::set_track_output(std::uint32_t track, int bus) {
    if (track >= kMaxTracks || bus < kMasterOutput || bus >= static_cast<int>(kMaxBuses)) {
        return false;
    }
    Command command(CommandType::SetTrackOutput);
    command.index = static_cast<std::int32_t>(track);
    command.a = bus;
    return commands_.try_push(command);
}

bool Engine::set_bus_gain(std::uint32_t bus, float linear) {
    if (bus >= kMaxBuses || !valid_gain(linear)) {
        return false;
    }
    Command command(CommandType::SetBusGain);
    command.index = static_cast<std::int32_t>(bus);
    command.value = linear;
    return commands_.try_push(command);
}

bool Engine::set_bus_mute(std::uint32_t bus, bool mute) {
    if (bus >= kMaxBuses) {
        return false;
    }
    Command command(CommandType::SetBusMute);
    command.index = static_cast<std::int32_t>(bus);
    command.flag = mute;
    return commands_.try_push(command);
}

bool Engine::set_gain(float linear) {
    if (!valid_gain(linear)) {
        return false;
    }
    Command command(CommandType::SetGain);
    command.value = linear;
    return commands_.try_push(command);
}

bool Engine::play(bool fade_in) {
    Command command(CommandType::Play);
    command.flag = !fade_in;
    return commands_.try_push(command);
}

bool Engine::pause() { return commands_.try_push(Command(CommandType::Pause)); }

bool Engine::stop() { return commands_.try_push(Command(CommandType::Stop)); }

bool Engine::seek(std::int64_t frame) {
    if (frame < 0) {
        return false;
    }
    Command command(CommandType::Seek);
    command.a = frame;
    return commands_.try_push(command);
}

bool Engine::set_loop(bool enabled, std::int64_t start_frame, std::int64_t end_frame) {
    if (start_frame < 0 || (end_frame >= 0 && end_frame <= start_frame)) {
        return false;
    }
    Command command(CommandType::SetLoop);
    command.flag = enabled;
    command.a = start_frame;
    command.b = end_frame;
    return commands_.try_push(command);
}

bool Engine::set_reverse(bool reverse) {
    Command command(CommandType::SetReverse);
    command.flag = reverse;
    return commands_.try_push(command);
}

bool Engine::set_metronome(bool enabled, double bpm, std::uint32_t beats_per_bar,
                           std::uint32_t beat_unit, float volume) {
    const bool unit_ok =
        beat_unit == 1 || beat_unit == 2 || beat_unit == 4 || beat_unit == 8 || beat_unit == 16;
    if (!(bpm >= 20.0 && bpm <= 400.0) || beats_per_bar < 1 || beats_per_bar > 32 || !unit_ok ||
        !(volume >= 0.0f && volume <= 2.0f)) {
        return false;
    }
    Command command(CommandType::SetMetronome);
    command.flag = enabled;
    // Length of one beat in frames: bpm counts quarter notes, the beat unit may be shorter.
    command.number = static_cast<double>(sample_rate_) * 60.0 / bpm * 4.0 /
                     static_cast<double>(beat_unit);
    command.a = beats_per_bar;
    command.value = volume;
    return commands_.try_push(command);
}

bool Engine::set_monitoring(bool enabled, float gain) {
    if (!valid_gain(gain)) {
        return false;
    }
    Command command(CommandType::SetMonitoring);
    command.flag = enabled;
    command.value = gain;
    return commands_.try_push(command);
}

bool Engine::set_input_gain(float linear) {
    if (!valid_gain(linear)) {
        return false;
    }
    Command command(CommandType::SetInputGain);
    command.value = linear;
    return commands_.try_push(command);
}

bool Engine::start_recording() { return commands_.try_push(Command(CommandType::StartRecording)); }

bool Engine::stop_recording() { return commands_.try_push(Command(CommandType::StopRecording)); }

bool Engine::live_note_on(int pitch, float velocity, int program, Instrument instrument,
                          std::shared_ptr<const SamplerSound> sampler) {
    if (pitch < 0 || pitch > 127) {
        return false;
    }
    Command command(CommandType::LiveNoteOn);
    command.index = pitch;
    command.value = std::clamp(velocity, 0.0f, 1.0f);
    command.a = program;
    command.b = static_cast<std::int64_t>(instrument);
    command.sampler = sampler.get();
    if (sampler && std::find(samplers_.begin(), samplers_.end(), sampler) == samplers_.end()) {
        samplers_.push_back(std::move(sampler));  // alive as long as a voice may use it
    }
    return commands_.try_push(command);
}

bool Engine::live_note_off(int pitch) {
    Command command(CommandType::LiveNoteOff);
    command.index = pitch;
    return commands_.try_push(command);
}

bool Engine::live_all_notes_off() { return commands_.try_push(Command(CommandType::LiveAllOff)); }

bool Engine::request_sync(std::int64_t token) {
    Command command(CommandType::Sync);
    command.a = token;
    return commands_.try_push(command);
}

bool Engine::live_plugin_note(std::shared_ptr<AudioEffect> instrument, bool on, int pitch, float velocity) {
    if (!instrument || pitch < 0 || pitch > 127) {
        return false;
    }
    Command command(CommandType::LivePluginNote);
    command.effect = instrument.get();
    command.flag = on;
    command.index = pitch;
    command.value = std::clamp(velocity, 0.0f, 1.0f);
    if (std::find(live_instruments_.begin(), live_instruments_.end(), instrument) == live_instruments_.end()) {
        live_instruments_.push_back(std::move(instrument));
    }
    return commands_.try_push(command);
}

// Instrument plugins sound also while playback is stopped: keys played live, and the release
// of the notes that were playing.
void Engine::render_stopped_instruments(float* out, std::uint32_t frames) noexcept {
    const std::size_t channels = channels_;
    const std::size_t samples = static_cast<std::size_t>(frames) * channels;
    for (Track& track : tracks_) {
        if (track.content == nullptr || !track.content->instrument) {
            continue;
        }
        AudioEffect& instrument = *track.content->instrument;
        if (track.notes_on) {
            instrument.add_note(0, false, -1, 0.0f);  // playback stopped: the notes end
            track.notes_on = false;
        }
        track.last_position = -1;
        float* scratch = track_mix_.data();
        std::fill(scratch, scratch + samples, 0.0f);
        instrument.process(scratch, frames, static_cast<std::uint32_t>(channels));
        for (const auto& effect : track.content->effects) {
            if (effect->enabled()) {
                effect->process(scratch, frames, static_cast<std::uint32_t>(channels));
            }
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float* src = scratch + static_cast<std::size_t>(i) * channels;
            float* dst = out + static_cast<std::size_t>(i) * channels;
            if (channels == 2) {
                dst[0] += src[0] * track.target_left * gain_;
                dst[1] += src[1] * track.target_right * gain_;
            } else {
                for (std::size_t c = 0; c < channels; ++c) {
                    dst[c] += src[c] * track.target_left * gain_;
                }
            }
        }
    }
}

// The notes of a region that start or end in this block, as events for an instrument plugin.
// A note starts where the playhead passes its beginning (note_prev_ < start <= position).
void Engine::schedule_notes(Track& track, const Region& region, const NoteData& notes,
                            AudioEffect& instrument, std::uint32_t frames, std::int64_t lowest,
                            std::int64_t highest) noexcept {
    const std::int64_t begin = region.start;
    const std::int64_t end = region.start + region.length;
    for (const Note& note : notes.notes) {
        const std::int64_t on_at = begin + (note.start - region.offset);
        if (on_at > highest || on_at >= end) {
            break;  // the notes are sorted by start
        }
        const std::int64_t off_at = std::min(end, on_at + note.length);
        const bool starts = on_at >= begin && on_at > lowest;
        const bool ends = off_at > lowest && off_at <= highest;
        if (!starts && !ends) {
            continue;
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            const std::int64_t position = track_pos_[i];
            const std::int64_t previous = note_prev_[i];
            if (position < 0) {
                continue;
            }
            if (starts && previous < on_at && on_at <= position) {
                instrument.add_note(i, true, note.pitch, note.velocity);
                track.notes_on = true;
            }
            if (ends && previous < off_at && off_at <= position) {
                instrument.add_note(i, false, note.pitch, 0.0f);
            }
        }
    }
}

void Engine::render_live(float* out, std::uint32_t frames) noexcept {
    const SoundFont* const font = soundfont_.load(std::memory_order_acquire);
    const double rate = static_cast<double>(sample_rate_);
    const std::size_t channels = channels_;
    for (LiveVoice& voice : live_) {
        if (!voice.active) {
            continue;
        }
        const SoundFont::Preset* const preset =
            font != nullptr && voice.program >= 0 && voice.sampler == nullptr ? font->find(voice.program)
                                                                              : nullptr;
        Note note;
        note.pitch = static_cast<std::uint8_t>(voice.pitch);
        note.velocity = voice.velocity;
        // Held: as good as endless; released: it ended when the key went up.
        note.length = voice.released ? static_cast<std::int64_t>(voice.released_at)
                                     : (std::int64_t{1} << 40);
        for (std::uint32_t i = 0; i < frames; ++i) {
            float left = 0.0f;
            float right = 0.0f;
            if (voice.sampler != nullptr) {
                left = right = sampler_note(*voice.sampler, note, voice.time, rate);
            } else if (preset != nullptr) {
                font->add_note(*preset, note, voice.time, rate, left, right);
            } else {
                left = right = note_sample(voice.instrument, note, voice.time, rate);
            }
            float* dst = out + static_cast<std::size_t>(i) * channels;
            if (channels >= 2) {
                dst[0] += left * gain_;
                dst[1] += right * gain_;
            } else {
                dst[0] += 0.5f * (left + right) * gain_;
            }
            voice.time += 1.0;
        }
        const double release =
            (voice.sampler != nullptr ? kSamplerReleaseSeconds
             : preset != nullptr      ? preset->release
                                      : instrument_release_seconds(voice.instrument)) *
            rate;
        if (voice.released && voice.time > voice.released_at + release + 1.0) {
            voice.active = false;
        }
    }
}

void Engine::set_soundfont(std::shared_ptr<const SoundFont> soundfont) {
    soundfont_.store(soundfont.get(), std::memory_order_release);
    if (soundfont) {
        soundfonts_.push_back(std::move(soundfont));
    }
}

bool Engine::set_speed(double speed) noexcept {
    if (!(speed >= kMinSpeed && speed <= kMaxSpeed)) {
        return false;
    }
    speed_.store(speed, std::memory_order_relaxed);
    return true;
}

std::size_t Engine::read_recorded(float* destination, std::size_t max_frames) {
    // The audio thread writes whole frames, so the amount read is always a whole number of them.
    return recorded_.read(destination, max_frames * channels_) / channels_;
}

void Engine::collect_retired() {
    const TrackContent* retired = nullptr;
    while (retired_.try_pop(retired)) {
        // Remove exactly one owner entry: every set_track_clip added one and every swap
        // retires one.
        const auto it = std::find_if(owned_.begin(), owned_.end(),
                                     [retired](const auto& owner) { return owner.get() == retired; });
        if (it != owned_.end()) {
            owned_.erase(it);
        }
    }
}

float Engine::track_peak(std::uint32_t track) const noexcept {
    return track < kMaxTracks ? track_peaks_[track].load(std::memory_order_relaxed) : 0.0f;
}

float Engine::bus_peak(std::uint32_t bus) const noexcept {
    return bus < kMaxBuses ? bus_peaks_[bus].load(std::memory_order_relaxed) : 0.0f;
}

// ---- Audio thread ------------------------------------------------------------------------

void Engine::update_targets() noexcept {
    bool any_solo = false;
    for (const Track& track : tracks_) {
        any_solo = any_solo || track.solo;
    }
    const bool stereo = channels_ == 2;
    const bool snap = state_ == State::Stopped;  // nothing is sounding: no need to glide

    for (Track& track : tracks_) {
        const bool audible = !track.mute && (!any_solo || track.solo);
        const float base = audible ? track.gain : 0.0f;
        // A track with pan automation gets its pan from the curve, not from the pan control.
        const bool own_pan = stereo && (track.content == nullptr || track.content->pan.empty());
        track.target_left = own_pan ? base * track.pan_left : base;
        track.target_right = own_pan ? base * track.pan_right : base;
        if (snap) {
            track.current_left = track.target_left;
            track.current_right = track.target_right;
        }
    }
    for (Bus& bus : buses_) {
        bus.target = bus.mute ? 0.0f : bus.gain;
        if (snap) {
            bus.current = bus.target;
        }
    }
    if (snap) {
        gain_ = gain_target_;
    }
}

void Engine::apply(const Command& command) noexcept {
    const auto index = static_cast<std::size_t>(command.index);

    switch (command.type) {
        case CommandType::SetTrackContent: {
            Track& track = tracks_[index];
            if (track.content != nullptr) {
                retired_.try_push(track.content);
            }
            track.content = command.content;
            track.frames = track.content != nullptr ? track.content->end : 0;
            update_targets();  // the content says whether the pan is automated
            project_frames_ = 0;
            for (const Track& other : tracks_) {
                project_frames_ = std::max(project_frames_, other.frames);
            }
            if (command.flag) {  // stop and return to the start
                state_ = State::Stopped;
                pending_ = Pending::None;
                reverse_ = wanted_reverse_;
                pos_ = 0;
                fade_ = 0.0f;
            }
            update_targets();
            break;
        }

        case CommandType::SetTrackGain:
            tracks_[index].gain = command.value;
            update_targets();
            break;

        case CommandType::SetTrackPan:
            tracks_[index].pan_left = command.value;
            tracks_[index].pan_right = command.value2;
            update_targets();
            break;

        case CommandType::SetTrackMute:
            tracks_[index].mute = command.flag;
            update_targets();
            break;

        case CommandType::SetTrackSolo:
            tracks_[index].solo = command.flag;
            update_targets();
            break;

        case CommandType::SetTrackOutput:
            tracks_[index].bus = static_cast<int>(command.a);
            break;

        case CommandType::SetBusGain:
            buses_[index].gain = command.value;
            update_targets();
            break;

        case CommandType::SetBusMute:
            buses_[index].mute = command.flag;
            update_targets();
            break;

        case CommandType::SetGain:
            gain_target_ = command.value;
            update_targets();
            break;

        case CommandType::Play:
            if (project_frames_ <= 0 && !metronome_enabled_ && !record_armed_) {
                break;
            }
            if (state_ == State::Stopped) {
                const bool endless =
                    metronome_enabled_ && metronome_endless_.load(std::memory_order_relaxed);
                if (!reverse_ && !record_armed_ && !endless && project_frames_ > 0 &&
                    pos_ >= project_frames_) {
                    pos_ = 0;  // the project had ended: start again
                } else if (reverse_ && pos_ <= 0) {
                    pos_ = project_frames_;  // at the start: play backwards from the end
                }
                fade_ = command.flag ? 1.0f : 0.0f;  // flag: start without the fade-in
                state_ = State::Playing;
                pending_ = Pending::None;
            } else if (state_ == State::FadingOut && pending_ != Pending::Seek) {
                if (wanted_reverse_ != reverse_) {
                    pending_ = Pending::Seek;  // finish the fade, turn around, keep playing
                    pending_seek_ = -1;
                } else {
                    state_ = State::Playing;  // cancels the pending pause or stop
                    pending_ = Pending::None;
                }
            }
            // A pending seek or change of direction ends in playback anyway.
            break;

        case CommandType::Pause:
            if (state_ != State::Stopped) {
                state_ = State::FadingOut;
                pending_ = Pending::Pause;
            }
            break;

        case CommandType::Stop:
            if (state_ == State::Stopped) {
                pos_ = 0;
            } else {
                state_ = State::FadingOut;
                pending_ = Pending::Stop;
            }
            break;

        case CommandType::Seek:
            if (state_ == State::Stopped) {
                pos_ = std::clamp<std::int64_t>(command.a, 0, project_frames_);
            } else if (state_ == State::Playing || pending_ == Pending::Seek) {
                state_ = State::FadingOut;
                pending_ = Pending::Seek;
                pending_seek_ = command.a;
            } else {
                // Already fading out for a pause or stop: the sound is dying away anyway.
                pos_ = std::clamp<std::int64_t>(command.a, 0, project_frames_);
            }
            break;

        case CommandType::SetLoop:
            loop_enabled_ = command.flag;
            loop_start_ = command.a;
            loop_end_ = command.b;
            break;

        case CommandType::SetReverse:
            wanted_reverse_ = command.flag;
            if (state_ == State::Stopped) {
                reverse_ = wanted_reverse_;
            } else if (state_ == State::Playing && wanted_reverse_ != reverse_) {
                // Turn around at the bottom of a short fade so that it does not click.
                state_ = State::FadingOut;
                pending_ = Pending::Seek;
                pending_seek_ = -1;  // no jump: turn around where the fade ends
            }
            break;

        case CommandType::SetMetronome:
            metronome_enabled_ = command.flag;
            metronome_beat_frames_ = command.number;
            metronome_beats_per_bar_ = command.a;
            metronome_volume_ = command.value;
            if (state_ == State::Stopped) {
                metronome_gain_ = metronome_enabled_ ? metronome_volume_ : 0.0f;
            }
            break;

        case CommandType::SetMonitoring:
            monitoring_ = command.flag;
            monitor_volume_ = command.value;
            break;

        case CommandType::SetInputGain:
            input_gain_target_ = command.value;
            break;

        case CommandType::StartRecording:
            if (!record_armed_) {
                record_armed_ = true;
                record_active_ = false;
                record_start_.store(-1, std::memory_order_relaxed);
            }
            break;

        case CommandType::StopRecording:
            record_armed_ = false;
            record_active_ = false;
            break;
        case CommandType::LiveNoteOn: {
            // The same key again ends the earlier note; otherwise a free voice, or the oldest.
            LiveVoice* target = nullptr;
            for (LiveVoice& voice : live_) {
                if (voice.active && !voice.released && voice.pitch == command.index) {
                    voice.released = true;
                    voice.released_at = voice.time;
                }
            }
            for (LiveVoice& voice : live_) {
                if (!voice.active) {
                    target = &voice;
                    break;
                }
            }
            if (target == nullptr) {
                target = &live_[0];
                for (LiveVoice& voice : live_) {
                    if (voice.time > target->time) {
                        target = &voice;
                    }
                }
            }
            *target = LiveVoice();
            target->active = true;
            target->pitch = command.index;
            target->velocity = command.value;
            target->program = static_cast<int>(command.a);
            target->instrument = static_cast<Instrument>(command.b);
            target->sampler = command.sampler;
            break;
        }
        case CommandType::LiveNoteOff:
            for (LiveVoice& voice : live_) {
                if (voice.active && !voice.released && voice.pitch == command.index) {
                    voice.released = true;
                    voice.released_at = voice.time;
                }
            }
            break;
        case CommandType::Sync:
            synced_.store(command.a, std::memory_order_release);
            break;
        case CommandType::LivePluginNote:
            if (command.effect != nullptr) {
                command.effect->add_note(0, command.flag, command.index, command.value);
            }
            break;
        case CommandType::LiveAllOff:
            for (LiveVoice& voice : live_) {
                if (voice.active && !voice.released) {
                    voice.released = true;
                    voice.released_at = voice.time;
                }
            }
            break;
    }
}

void Engine::finish_fade() noexcept {
    reverse_ = wanted_reverse_;
    switch (pending_) {
        case Pending::Seek:
            if (pending_seek_ >= 0) {
                pos_ = std::clamp<std::int64_t>(pending_seek_, 0, project_frames_);
            }
            state_ = State::Playing;  // fade_ is 0 here, so playback fades in at the new position
            break;
        case Pending::Stop:
            pos_ = 0;
            state_ = State::Stopped;
            break;
        case Pending::Pause:
        case Pending::None:
            state_ = State::Stopped;
            break;
    }
    pending_ = Pending::None;
}

bool Engine::loop_region(std::int64_t& start, std::int64_t& end) const noexcept {
    if (!loop_enabled_ || project_frames_ <= 0) {
        return false;
    }
    end = (loop_end_ < 0 || loop_end_ > project_frames_) ? project_frames_ : loop_end_;
    if (end <= 0) {
        return false;
    }
    start = std::clamp<std::int64_t>(loop_start_, 0, end - 1);
    return true;
}

// Decides, for every output frame of the block, which timeline frame it plays (or silence) and
// how far the transport fade is open. Returns false when the whole block is silent.
bool Engine::run_transport(std::uint32_t frames, std::int64_t begin_pos, std::int64_t end_pos,
                           bool looping) noexcept {
    bool audible = false;
    // Recording keeps the normal speed, so the take lines up with the timeline.
    const double speed = record_armed_ ? 1.0 : speed_.load(std::memory_order_relaxed);
    if (record_armed_) {
        frac_ = 0.0;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        frame_pos_[i] = -1;
        frame_fade_[i] = 0.0f;
        frame_frac_[i] = 0.0f;

        if (state_ == State::Stopped) {
            continue;
        }

        if (!reverse_ && pos_ >= end_pos) {
            if (looping) {
                pos_ = begin_pos;
            } else {
                // The project ended. A pending stop still returns to the start.
                state_ = State::Stopped;
                pos_ = pending_ == Pending::Stop ? 0 : project_frames_;
                pending_ = Pending::None;
                reverse_ = wanted_reverse_;
                fade_ = 0.0f;
                continue;
            }
        } else if (reverse_ && pos_ <= begin_pos) {
            if (looping) {
                pos_ = end_pos;
            } else {
                // Playing backwards reached the start.
                state_ = State::Stopped;
                pos_ = 0;
                pending_ = Pending::None;
                reverse_ = wanted_reverse_;
                fade_ = 0.0f;
                continue;
            }
        }

        if (state_ == State::FadingOut) {
            fade_ -= fade_step_;
            if (fade_ <= 0.0f) {
                fade_ = 0.0f;
                finish_fade();
                continue;
            }
        } else if (fade_ < 1.0f) {
            fade_ = std::min(1.0f, fade_ + fade_step_);
        }

        // The playhead moves `speed` frames per output frame. At speed 1 this is exactly one.
        if (reverse_) {
            frac_ -= speed;
            while (frac_ < 0.0) {
                --pos_;
                frac_ += 1.0;
            }
            frame_pos_[i] = pos_;
            frame_frac_[i] = static_cast<float>(frac_);
        } else {
            frame_pos_[i] = pos_;
            frame_frac_[i] = static_cast<float>(frac_);
            frac_ += speed;
            while (frac_ >= 1.0) {
                ++pos_;
                frac_ -= 1.0;
            }
        }
        frame_fade_[i] = fade_;
        audible = true;
    }
    return audible;
}

// The metronome click at a timeline frame: a short decaying sine at the start of every beat.
// It is a pure function of the position, so it stays in step through seeks, loops and reverse.
float Engine::metronome_sample(std::int64_t frame) const noexcept {
    const double beats = static_cast<double>(frame) / metronome_beat_frames_;
    const double whole = std::floor(beats);
    const double seconds =
        (beats - whole) * metronome_beat_frames_ / static_cast<double>(sample_rate_);
    if (seconds >= kClickSeconds) {
        return 0.0f;
    }
    const bool accent = static_cast<std::int64_t>(whole) % metronome_beats_per_bar_ == 0;
    const double frequency = accent ? kClickAccentFrequency : kClickFrequency;
    const double envelope =
        std::exp(-seconds / kClickDecaySeconds) * (1.0 - seconds / kClickSeconds);
    return static_cast<float>(std::sin(kTwoPi * frequency * seconds) * envelope);
}

// Handles the input of one block: monitoring (input added to the output) and recording (input
// copied to the ring buffer together with its place on the timeline).
void Engine::process_input(const float* in, float* out, std::uint32_t frames) noexcept {
    if (in == nullptr) {
        return;
    }
    const std::size_t channels = channels_;
    std::uint64_t dropped = 0;
    float peak = 0.0f;

    for (std::uint32_t i = 0; i < frames; ++i) {
        // The input gain comes first: it affects monitoring, recording and the meter alike.
        const float* raw = in + static_cast<std::size_t>(i) * channels;
        smooth(input_gain_, input_gain_target_, gain_coef_);
        float* src = input_frame_.data();
        for (std::size_t c = 0; c < channels; ++c) {
            src[c] = raw[c] * input_gain_;
            peak = std::max(peak, std::fabs(src[c]));
        }

        smooth(monitor_gain_, monitoring_ ? monitor_volume_ : 0.0f, gain_coef_);
        if (monitor_gain_ > 0.0f) {
            float* dst = out + static_cast<std::size_t>(i) * channels;
            for (std::size_t c = 0; c < channels; ++c) {
                dst[c] += src[c] * monitor_gain_;
            }
        }

        if (!record_armed_) {
            continue;
        }
        const std::int64_t position = frame_pos_[i];
        if (!record_active_) {
            if (position < 0 || reverse_) {
                continue;  // waiting for playback to run forwards
            }
            record_active_ = true;
            record_next_ = position;
            record_start_.store(position, std::memory_order_relaxed);
        }
        if (position != record_next_) {
            // Playback stopped, jumped or turned around: the take ends here.
            record_armed_ = false;
            record_active_ = false;
            continue;
        }
        if (recorded_.write(src, channels) != channels) {
            ++dropped;  // the control thread did not fetch the data in time
        }
        ++record_next_;
    }

    const float decay = static_cast<float>(std::exp(
        -static_cast<double>(frames) / (kMeterReleaseSeconds * static_cast<double>(sample_rate_))));
    input_meter_ = decayed_meter(input_meter_, decay, peak);
    input_peak_.store(input_meter_, std::memory_order_relaxed);

    if (dropped != 0) {
        record_dropped_.store(record_dropped_.load(std::memory_order_relaxed) + dropped,
                              std::memory_order_relaxed);
    }
}

void Engine::mix_block(const float* in, float* out, std::uint32_t frames,
                       bool audible) noexcept {
    const std::size_t channels = channels_;
    const std::size_t samples = static_cast<std::size_t>(frames) * channels;
    const bool stereo = channels == 2;
    const float coef = gain_coef_;
    const float decay = static_cast<float>(std::exp(
        -static_cast<double>(frames) / (kMeterReleaseSeconds * static_cast<double>(sample_rate_))));

    if (!audible) {
        // Nothing sounds in this block: write silence and let the meters fall back.
        std::fill(out, out + samples, 0.0f);
        for (std::size_t t = 0; t < kMaxTracks; ++t) {
            tracks_[t].meter = decayed_meter(tracks_[t].meter, decay, 0.0f);
            track_peaks_[t].store(tracks_[t].meter, std::memory_order_relaxed);
        }
        for (std::size_t b = 0; b < kMaxBuses; ++b) {
            buses_[b].meter = decayed_meter(buses_[b].meter, decay, 0.0f);
            bus_peaks_[b].store(buses_[b].meter, std::memory_order_relaxed);
        }
        master_meter_ = decayed_meter(master_meter_, decay, 0.0f);
        master_peak_.store(master_meter_, std::memory_order_relaxed);
        render_stopped_instruments(out, frames);
        render_live(out, frames);
        process_input(in, out, frames);
        return;
    }

    const std::size_t bus_stride = static_cast<std::size_t>(kBlockFrames) * channels;
    std::array<bool, kMaxBuses> bus_used{};
    float* master = master_mix_.data();
    std::fill(master, master + samples, 0.0f);

    // The range of timeline frames this block touches, to skip regions that lie elsewhere.
    std::int64_t block_low = std::numeric_limits<std::int64_t>::max();
    std::int64_t block_high = -1;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::int64_t position = frame_pos_[i];
        if (position >= 0) {
            block_low = std::min(block_low, position);
            block_high = std::max(block_high, position);
        }
    }

    // Tracks -> buses or master.
    for (std::size_t t = 0; t < kMaxTracks; ++t) {
        Track& track = tracks_[t];
        float peak = 0.0f;

        if (track.content != nullptr) {
            // Collect the track's regions (with their fades) in a scratch buffer first.
            float* scratch = track_mix_.data();
            bool any_region = false;
            std::uint64_t misses = 0;
            // Delay compensation: a track whose plugins delay its sound reads its regions that
            // much ahead, so that what comes out of the plugins lines up with the other tracks.
            const std::int64_t ahead = track.content->latency;
            const std::int64_t* positions = frame_pos_.data();
            std::int64_t track_low = block_low;
            std::int64_t track_high = block_high;
            if (ahead > 0) {
                for (std::uint32_t i = 0; i < frames; ++i) {
                    shifted_pos_[i] = frame_pos_[i] >= 0 ? frame_pos_[i] + ahead : frame_pos_[i];
                }
                positions = shifted_pos_.data();
                if (block_high >= 0) {
                    track_low += ahead;
                    track_high += ahead;
                }
            }
            track_pos_ = positions;
            // Automated plugin parameters get the value of their curve at the start of the block.
            if (block_high >= 0) {
                for (const ParamCurve& curve : track.content->params) {
                    const float value = curve_at(curve.points, block_low);
                    if (!curve.sent || value != curve.last) {
                        curve.effect->set_param_rt(curve.id, static_cast<double>(value));
                        curve.sent = true;
                        curve.last = value;
                    }
                }
            }
            // An instrument plugin gets the notes as events. Where the playhead jumps (a seek,
            // a loop, playing backwards), the notes that were sounding end.
            AudioEffect* const instrument = track.content->instrument.get();
            std::int64_t lowest_previous = track_high;
            if (instrument != nullptr) {
                constexpr std::int64_t kMaxStep = 16;  // more than the fastest playback moves
                std::int64_t previous = track.last_position;
                for (std::uint32_t i = 0; i < frames; ++i) {
                    const std::int64_t position = positions[i];
                    if (position < 0) {
                        note_prev_[i] = position;
                        continue;
                    }
                    if (previous < 0 || position < previous || position - previous > kMaxStep) {
                        if (previous >= 0 && track.notes_on) {
                            instrument->add_note(i, false, -1, 0.0f);
                            track.notes_on = false;
                        }
                        note_prev_[i] = position - 1;
                    } else {
                        note_prev_[i] = previous;
                    }
                    lowest_previous = std::min(lowest_previous, note_prev_[i]);
                    previous = position;
                }
                track.last_position = previous;
            }
            for (const Region& region : track.content->regions) {
                const std::int64_t begin = region.start;
                const std::int64_t end = region.start + region.length;
                if (end <= track_low || begin > track_high) {
                    continue;
                }
                if (!any_region) {
                    std::fill(scratch, scratch + samples, 0.0f);
                    any_region = true;
                }
                const float* clip_samples = region.clip->samples.data();
                PagedFile* const disk = region.clip->disk.get();
                const std::int64_t source_end =
                    region.offset + clip_frames_used(region.length, region.rate);
                std::array<float, 16> from_disk{};
                std::array<float, 16> from_disk_next{};
                // Note regions: the notes that sound somewhere in this block.
                const NoteData* const note_data = region.clip->notes.get();
                if (note_data != nullptr && instrument != nullptr) {
                    schedule_notes(track, region, *note_data, *instrument, frames, lowest_previous,
                                   track_high);
                    continue;
                }
                std::array<std::uint32_t, 64> active{};
                std::size_t active_count = 0;
                // With a program and a SoundFont, recorded instruments play the notes.
                // A sampler plays them before anything else.
                const SamplerSound* const sampler =
                    note_data != nullptr ? region.sampler.get() : nullptr;
                const SoundFont* const font =
                    note_data != nullptr && region.program >= 0 && sampler == nullptr
                        ? soundfont_.load(std::memory_order_acquire)
                        : nullptr;
                const SoundFont::Preset* const preset =
                    font != nullptr ? font->find(region.program) : nullptr;
                if (note_data != nullptr) {
                    const double release =
                        (sampler != nullptr ? kSamplerReleaseSeconds
                         : preset != nullptr ? preset->release
                                             : instrument_release_seconds(region.instrument)) *
                        static_cast<double>(sample_rate_);
                    const std::int64_t low = region.offset + std::max<std::int64_t>(track_low - begin, 0);
                    const std::int64_t high = region.offset + (track_high - begin) + 1;
                    for (std::size_t k = 0; k < note_data->notes.size() && active_count < active.size(); ++k) {
                        const Note& note = note_data->notes[k];
                        if (note.start > high) {
                            break;  // the notes are sorted by start
                        }
                        if (static_cast<double>(note.start + note.length) + release >
                            static_cast<double>(low)) {
                            active[active_count++] = static_cast<std::uint32_t>(k);
                        }
                    }
                }
                for (std::uint32_t i = 0; i < frames; ++i) {
                    const std::int64_t position = positions[i];
                    if (position < begin || position >= end) {
                        continue;
                    }
                    const std::int64_t into = position - begin;
                    const std::int64_t remaining = end - 1 - position;
                    float region_fade = 1.0f;
                    if (into < region.fade_in) {
                        region_fade = fade_curve(
                            region.fade_shape,
                            (static_cast<float>(into) + 0.5f) / static_cast<float>(region.fade_in));
                    }
                    if (remaining < region.fade_out) {
                        region_fade *= fade_curve(region.fade_shape,
                                                  (static_cast<float>(remaining) + 0.5f) /
                                                      static_cast<float>(region.fade_out));
                    }
                    if (note_data != nullptr) {
                        const double note_pos = static_cast<double>(region.offset) +
                                                static_cast<double>(into) +
                                                static_cast<double>(frame_frac_[i]);
                        float* note_dst = scratch + static_cast<std::size_t>(i) * channels;
                        if (preset != nullptr) {
                            float left = 0.0f;
                            float right = 0.0f;
                            for (std::size_t k = 0; k < active_count; ++k) {
                                const Note& note = note_data->notes[active[k]];
                                font->add_note(*preset, note, note_pos - static_cast<double>(note.start),
                                               static_cast<double>(sample_rate_), left, right);
                            }
                            if (channels >= 2) {
                                note_dst[0] += left * region_fade;
                                note_dst[1] += right * region_fade;
                                for (std::size_t c = 2; c < channels; ++c) {
                                    note_dst[c] += 0.5f * (left + right) * region_fade;
                                }
                            } else {
                                note_dst[0] += 0.5f * (left + right) * region_fade;
                            }
                            continue;
                        }
                        float value = 0.0f;
                        for (std::size_t k = 0; k < active_count; ++k) {
                            const Note& note = note_data->notes[active[k]];
                            const double since = note_pos - static_cast<double>(note.start);
                            value += sampler != nullptr
                                         ? sampler_note(*sampler, note, since,
                                                        static_cast<double>(sample_rate_))
                                         : note_sample(region.instrument, note, since,
                                                       static_cast<double>(sample_rate_));
                        }
                        value *= region_fade;
                        for (std::size_t c = 0; c < channels; ++c) {
                            note_dst[c] += value;
                        }
                        continue;
                    }
                    // Where in the clip this frame is: a region may play its clip faster or
                    // slower (rate), and the playhead may be between two frames.
                    const double source_pos =
                        static_cast<double>(region.offset) +
                        (static_cast<double>(into) + static_cast<double>(frame_frac_[i])) * region.rate;
                    const std::int64_t source_frame =
                        std::min(static_cast<std::int64_t>(source_pos), source_end - 1);
                    const float frac =
                        static_cast<float>(source_pos - static_cast<double>(source_frame));
                    const float* src = nullptr;
                    if (disk != nullptr) {
                        // From disk: the page must be in memory already, unless this engine
                        // renders offline and may wait for it.
                        const std::int64_t frame = source_frame;
                        const bool read =
                            channels <= from_disk.size() &&
                            (blocking_reads_ ? disk->read_frame_blocking(frame, from_disk.data())
                                             : disk->read_frame(frame, from_disk.data()));
                        if (!read) {
                            ++misses;
                            continue;
                        }
                        src = from_disk.data();
                    } else {
                        src = clip_samples + static_cast<std::size_t>(source_frame) * channels;
                    }
                    // Between two frames (playback faster or slower than normal): blend them.
                    const float* next = nullptr;
                    if (frac > 0.0f && source_frame + 1 < source_end) {
                        if (disk != nullptr) {
                            const std::int64_t frame = source_frame + 1;
                            const bool read =
                                blocking_reads_
                                    ? disk->read_frame_blocking(frame, from_disk_next.data())
                                    : disk->read_frame(frame, from_disk_next.data());
                            if (read) {
                                next = from_disk_next.data();
                            }
                        } else {
                            next = src + channels;
                        }
                    }
                    float* dst = scratch + static_cast<std::size_t>(i) * channels;
                    if (next != nullptr) {
                        for (std::size_t c = 0; c < channels; ++c) {
                            dst[c] += (src[c] + (next[c] - src[c]) * frac) * region_fade;
                        }
                    } else {
                        for (std::size_t c = 0; c < channels; ++c) {
                            dst[c] += src[c] * region_fade;
                        }
                    }
                }
            }

            if (misses != 0) {
                disk_misses_.fetch_add(misses, std::memory_order_relaxed);
            }

            if (instrument != nullptr) {
                if (!any_region) {
                    std::fill(scratch, scratch + samples, 0.0f);
                    any_region = true;
                }
                instrument->process(scratch, frames, static_cast<std::uint32_t>(channels));
            }
            // The insert effects, in order, before the fader. They run also where no region
            // plays, so that echoes and reverbs ring out.
            if (!track.content->effects.empty()) {
                if (!any_region) {
                    std::fill(scratch, scratch + samples, 0.0f);
                    any_region = true;
                }
                for (const auto& effect : track.content->effects) {
                    if (effect->enabled()) {
                        effect->process(scratch, frames, static_cast<std::uint32_t>(channels));
                    }
                }
            }

            // Volume automation: the gain of the curve where the playhead is, times the fader.
            const std::vector<CurvePoint>* const curve =
                track.content->volume.empty() ? nullptr : &track.content->volume;
            float automation = curve != nullptr ? track.automation : 1.0f;
            const std::vector<CurvePoint>* const pan_curve =
                stereo && !track.content->pan.empty() ? &track.content->pan : nullptr;
            float pan_left = pan_curve != nullptr ? track.auto_pan_left : 1.0f;
            float pan_right = pan_curve != nullptr ? track.auto_pan_right : 1.0f;

            float* destination = master;
            if (track.bus >= 0) {
                const auto bus = static_cast<std::size_t>(track.bus);
                destination = bus_mix_.data() + bus * bus_stride;
                if (!bus_used[bus]) {
                    std::fill(destination, destination + samples, 0.0f);
                    bus_used[bus] = true;
                }
            }

            for (std::uint32_t i = 0; i < frames; ++i) {
                smooth(track.current_left, track.target_left, coef);
                smooth(track.current_right, track.target_right, coef);
                if (!any_region) {
                    continue;
                }
                const float* src = scratch + static_cast<std::size_t>(i) * channels;
                float* dst = destination + static_cast<std::size_t>(i) * channels;
                if (curve != nullptr && frame_pos_[i] >= 0) {
                    automation = curve_at(*curve, frame_pos_[i]);
                    track.automation = automation;
                }
                if (pan_curve != nullptr && frame_pos_[i] >= 0) {
                    // The same constant-power curve as the pan control.
                    const float pan = std::clamp(curve_at(*pan_curve, frame_pos_[i]), -1.0f, 1.0f);
                    const float angle = (pan + 1.0f) * 0.78539816f;
                    pan_left = pan >= 1.0f ? 0.0f : std::min(1.0f, 1.41421356f * std::cos(angle));
                    pan_right = pan <= -1.0f ? 0.0f : std::min(1.0f, 1.41421356f * std::sin(angle));
                    track.auto_pan_left = pan_left;
                    track.auto_pan_right = pan_right;
                }
                if (stereo) {
                    const float left = src[0] * track.current_left * automation * pan_left;
                    const float right = src[1] * track.current_right * automation * pan_right;
                    dst[0] += left;
                    dst[1] += right;
                    peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
                } else {
                    for (std::size_t c = 0; c < channels; ++c) {
                        const float value = src[c] * track.current_left * automation;
                        dst[c] += value;
                        peak = std::max(peak, std::fabs(value));
                    }
                }
            }
        } else {
            track.current_left = track.target_left;
            track.current_right = track.target_right;
        }

        track.meter = decayed_meter(track.meter, decay, peak);
        track_peaks_[t].store(track.meter, std::memory_order_relaxed);
    }

    // Buses -> master.
    for (std::size_t b = 0; b < kMaxBuses; ++b) {
        Bus& bus = buses_[b];
        float peak = 0.0f;

        if (bus_used[b]) {
            const float* source = bus_mix_.data() + b * bus_stride;
            for (std::uint32_t i = 0; i < frames; ++i) {
                smooth(bus.current, bus.target, coef);
                const std::size_t offset = static_cast<std::size_t>(i) * channels;
                for (std::size_t c = 0; c < channels; ++c) {
                    const float value = source[offset + c] * bus.current;
                    master[offset + c] += value;
                    peak = std::max(peak, std::fabs(value));
                }
            }
        } else {
            bus.current = bus.target;
        }

        bus.meter = decayed_meter(bus.meter, decay, peak);
        bus_peaks_[b].store(bus.meter, std::memory_order_relaxed);
    }

    // Master fader and transport fade -> output.
    float peak = 0.0f;
    std::uint64_t clips = 0;
    for (std::uint32_t i = 0; i < frames; ++i) {
        smooth(gain_, gain_target_, coef);
        const float level = frame_fade_[i] * gain_;
        // The metronome is added after the master fader, so the mix level does not change it.
        smooth(metronome_gain_, metronome_enabled_ ? metronome_volume_ : 0.0f, coef);
        float click = 0.0f;
        if (metronome_gain_ > 0.0f && frame_pos_[i] >= 0) {
            click = metronome_sample(frame_pos_[i]) * metronome_gain_ * frame_fade_[i];
        }
        const std::size_t offset = static_cast<std::size_t>(i) * channels;
        for (std::size_t c = 0; c < channels; ++c) {
            const float value = master[offset + c] * level + click;
            const float magnitude = std::fabs(value);
            peak = std::max(peak, magnitude);
            if (magnitude > 1.0f) {
                ++clips;
            }
            out[offset + c] = value;
        }
    }
    master_meter_ = decayed_meter(master_meter_, decay, peak);
    master_peak_.store(master_meter_, std::memory_order_relaxed);
    if (clips != 0) {
        master_clips_.store(master_clips_.load(std::memory_order_relaxed) + clips,
                            std::memory_order_relaxed);
    }
    render_live(out, frames);
    process_input(in, out, frames);
}

void Engine::render(const float* in, float* out, std::uint32_t frames) noexcept {
    Command command;
    while (commands_.try_pop(command)) {
        apply(command);
    }

    std::int64_t loop_start = 0;
    std::int64_t loop_end = 0;
    const bool looping = loop_region(loop_start, loop_end);
    const std::int64_t begin_pos = looping ? loop_start : 0;
    std::int64_t end_pos = looping ? loop_end : project_frames_;
    const bool endless_metronome =
        metronome_enabled_ &&
        (project_frames_ <= 0 || (!looping && metronome_endless_.load(std::memory_order_relaxed)));
    if (record_armed_ || endless_metronome) {
        // Recording, or the metronome is on (and set to go on past the end): run until stopped.
        end_pos = std::numeric_limits<std::int64_t>::max();
    }

    // Work through the request in blocks that fit the preallocated buffers.
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t count = std::min(frames - done, kBlockFrames);
        const bool audible = run_transport(count, begin_pos, end_pos, looping);
        const std::size_t offset = static_cast<std::size_t>(done) * channels_;
        mix_block(in != nullptr ? in + offset : nullptr, out + offset, count, audible);
        done += count;
    }

    published_position_.store(pos_, std::memory_order_relaxed);
    published_length_.store(project_frames_, std::memory_order_relaxed);
    published_playing_.store(state_ != State::Stopped, std::memory_order_relaxed);
    published_recording_.store(record_armed_, std::memory_order_relaxed);
}

}  // namespace toh::core
