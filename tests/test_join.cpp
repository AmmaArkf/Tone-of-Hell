// Tests for joining regions into one.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"

using toh::app::RegionInfo;
using toh::app::Session;
using toh::core::ClipData;

namespace {

// A stereo clip in which frame n holds the value (n + 1) / 1000 on both channels.
std::shared_ptr<ClipData> counting_clip(std::size_t frames) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = 2;
    clip->sample_rate = 48000;
    clip->samples.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        clip->samples[i * 2] = static_cast<float>(i + 1) / 1000.0f;
        clip->samples[i * 2 + 1] = static_cast<float>(i + 1) / 1000.0f;
    }
    return clip;
}

float joined_sample(const RegionInfo& region, std::int64_t frame) {
    float value[2] = {};
    toh::core::read_clip_frames(*region.source->clip, region.offset + frame, 1, value);
    return value[0];
}

}  // namespace

TEST_CASE("Joining two regions with a gap between them") {
    Session session;
    REQUIRE(session.add_track("take", counting_clip(1000)));
    REQUIRE(session.split_region(0, 0, 400));       // 0..400 and 400..1000
    REQUIRE(session.move_region(0, 1, 600));        // the second part now plays 600..1200
    REQUIRE(session.set_region_fades(0, 0, 50, 0));  // outer fade-in: kept as a fade

    std::size_t joined = 99;
    REQUIRE(session.join_regions(0, {1, 0}, &joined));
    CHECK(joined == 0);
    const auto& regions = session.tracks()[0].regions;
    REQUIRE(regions.size() == 1);
    const RegionInfo& region = regions[0];
    CHECK(region.start == 0);
    CHECK(region.length == 1200);
    CHECK(region.fade_in == 50);
    CHECK(region.fade_out == 0);
    CHECK(region.source->clip->frames() == 1200);

    CHECK(joined_sample(region, 10) == doctest::Approx(0.011f));   // not faded: still a fade
    CHECK(joined_sample(region, 399) == doctest::Approx(0.4f));
    CHECK(joined_sample(region, 500) == 0.0f);                     // the gap is silent
    CHECK(joined_sample(region, 600) == doctest::Approx(0.401f));  // source frame 400
    CHECK(joined_sample(region, 1199) == doctest::Approx(1.0f));
}

TEST_CASE("Joining applies the inner fades and mixes overlaps") {
    Session session;
    REQUIRE(session.add_track("take", counting_clip(1000)));
    REQUIRE(session.split_region(0, 0, 500));
    REQUIRE(session.set_region_fades(0, 0, 0, 100));  // inner fade-out of the first part
    REQUIRE(session.move_region(0, 1, 450));         // overlaps the first part by 50 frames

    REQUIRE(session.join_regions(0, {0, 1}));
    const RegionInfo& region = session.tracks()[0].regions[0];
    CHECK(region.length == 950);
    CHECK(region.fade_out == 0);
    // The last frame of the first part is faded to almost nothing, plus the second part.
    const float faded = 0.5f * (0.5f / 100.0f);
    CHECK(joined_sample(region, 499) == doctest::Approx(faded + 0.550f));
    CHECK(joined_sample(region, 100) == doctest::Approx(0.101f));
}

TEST_CASE("Joining refuses what it cannot do") {
    Session session;
    REQUIRE(session.add_track("take", counting_clip(1000)));
    CHECK_FALSE(session.join_regions(0, {0}));
    CHECK_FALSE(session.join_regions(0, {0, 0}));
    CHECK_FALSE(session.join_regions(0, {0, 3}));
    CHECK_FALSE(session.join_regions(4, {0, 1}));
    CHECK(session.tracks()[0].regions.size() == 1);
}
