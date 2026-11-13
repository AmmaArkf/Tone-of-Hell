// Tests for delay compensation: a track whose effects delay its sound reads ahead.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "doctest.h"
#include "toh/core/clip.h"
#include "toh/core/effect.h"
#include "toh/core/engine.h"

using toh::core::ClipData;
using toh::core::Engine;

namespace {

constexpr std::uint32_t kRate = 48000;

// An effect that only delays the sound by a number of frames, like a look-ahead limiter.
class DelayEffect final : public toh::core::AudioEffect {
public:
    explicit DelayEffect(std::size_t frames) : line_(frames, 0.0f) {}
    void process(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept override {
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * channels; ++i) {
            const float delayed = line_[position_];
            line_[position_] = interleaved[i];
            interleaved[i] = delayed;
            position_ = (position_ + 1) % line_.size();
        }
    }

private:
    std::vector<float> line_;
    std::size_t position_ = 0;
};

// A mono clip whose sample number n has the value n.
std::shared_ptr<ClipData> index_clip(std::size_t frames) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = 1;
    clip->sample_rate = kRate;
    clip->samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        clip->samples[i] = static_cast<float>(i);
    }
    return clip;
}

std::vector<float> play_with(std::int64_t latency, bool compensate) {
    Engine engine(kRate, 1);
    toh::core::Region region;
    region.clip = index_clip(4096);
    region.start = 0;
    region.offset = 0;
    region.length = 4096;
    const std::vector<std::shared_ptr<toh::core::AudioEffect>> effects = {
        std::make_shared<DelayEffect>(static_cast<std::size_t>(latency))};
    REQUIRE(engine.set_track_regions(0, {region}, effects, nullptr, compensate ? latency : 0));
    REQUIRE(engine.play(false));
    std::vector<float> out(1024);
    engine.render(out.data(), 512);
    engine.render(out.data() + 512, 512);
    return out;
}

}  // namespace

TEST_CASE("Without compensation an effect's latency puts the track late") {
    const std::vector<float> out = play_with(100, false);
    CHECK(out[600] == doctest::Approx(500.0f));
}

TEST_CASE("With compensation the track reads ahead and stays on time") {
    const std::vector<float> out = play_with(100, true);
    for (const std::size_t frame : {std::size_t{100}, std::size_t{300}, std::size_t{600}, std::size_t{1000}}) {
        CHECK(out[frame] == doctest::Approx(static_cast<float>(frame)));
    }
}
