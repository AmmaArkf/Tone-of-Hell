#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace toh::core {

/// From `frame` on the tempo is `bpm` (quarter notes per minute).
struct TempoChange {
    std::int64_t frame = 0;
    double bpm = 120.0;
};

/// The tempo along the timeline: a start tempo and changes at later frames. It turns timeline
/// frames into beats and back, for the grid, snapping and the metronome. Immutable, so one map
/// can be read from several threads.
class TempoMap {
public:
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 400.0;

    /// `beat_unit` is the note value of one beat (4: a quarter note, 8: an eighth note).
    /// Changes at frame 0 or earlier, or with a tempo out of range, are left out.
    TempoMap(double sample_rate, double start_bpm, int beat_unit, std::vector<TempoChange> changes)
        : sample_rate_(sample_rate), beat_unit_(std::max(beat_unit, 1)) {
        std::stable_sort(changes.begin(), changes.end(),
                         [](const TempoChange& a, const TempoChange& b) { return a.frame < b.frame; });
        segments_.push_back(Segment{0.0, 0.0, beat_frames_of(start_bpm), start_bpm});
        for (const TempoChange& change : changes) {
            if (change.frame <= 0 || !(change.bpm >= kMinBpm && change.bpm <= kMaxBpm)) {
                continue;
            }
            const Segment& previous = segments_.back();
            const auto frame = static_cast<double>(change.frame);
            if (frame <= previous.frame) {
                segments_.back().beat_frames = beat_frames_of(change.bpm);  // the same frame: the later one wins
                segments_.back().bpm = change.bpm;
                continue;
            }
            const double beat = previous.beat + (frame - previous.frame) / previous.beat_frames;
            segments_.push_back(Segment{frame, beat, beat_frames_of(change.bpm), change.bpm});
        }
    }

    /// How many beats lie before a timeline frame (with a fraction).
    double beats_at(double frame) const noexcept {
        const Segment& segment = segment_at_frame(frame);
        return segment.beat + (frame - segment.frame) / segment.beat_frames;
    }
    /// The timeline frame of a beat position (with a fraction).
    double frame_at(double beats) const noexcept {
        std::size_t index = segments_.size() - 1;
        while (index > 0 && segments_[index].beat > beats) {
            --index;
        }
        const Segment& segment = segments_[index];
        return segment.frame + (beats - segment.beat) * segment.beat_frames;
    }
    /// The length of one beat in frames at a timeline frame.
    double beat_frames_at(double frame) const noexcept { return segment_at_frame(frame).beat_frames; }
    double bpm_at(double frame) const noexcept { return segment_at_frame(frame).bpm; }

private:
    struct Segment {
        double frame;        // where it begins
        double beat;         // the beats before it
        double beat_frames;  // the length of a beat inside it
        double bpm;
    };

    double beat_frames_of(double bpm) const noexcept {
        return sample_rate_ * 60.0 / bpm * 4.0 / static_cast<double>(beat_unit_);
    }
    const Segment& segment_at_frame(double frame) const noexcept {
        std::size_t index = segments_.size() - 1;
        while (index > 0 && segments_[index].frame > frame) {
            --index;
        }
        return segments_[index];
    }

    double sample_rate_;
    int beat_unit_;
    std::vector<Segment> segments_;
};

}  // namespace toh::core
