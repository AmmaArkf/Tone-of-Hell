// Tests for volume automation.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"
#include "toh/core/engine.h"

using toh::app::Session;
using toh::core::CurvePoint;
using toh::core::Engine;

namespace {

std::shared_ptr<toh::core::ClipData> ones(std::size_t frames, std::uint32_t channels) {
    auto clip = std::make_shared<toh::core::ClipData>();
    clip->channels = channels;
    clip->sample_rate = 48000;
    clip->samples.assign(frames * channels, 1.0f);
    return clip;
}

}  // namespace

TEST_CASE("A curve has straight lines between its points and stays level outside them") {
    const std::vector<CurvePoint> curve = {{100, 0.0f}, {200, 1.0f}, {400, 0.5f}};
    CHECK(toh::core::curve_at(curve, 0) == doctest::Approx(0.0f));
    CHECK(toh::core::curve_at(curve, 150) == doctest::Approx(0.5f));
    CHECK(toh::core::curve_at(curve, 200) == doctest::Approx(1.0f));
    CHECK(toh::core::curve_at(curve, 300) == doctest::Approx(0.75f));
    CHECK(toh::core::curve_at(curve, 9000) == doctest::Approx(0.5f));
}

TEST_CASE("The engine plays a track at the volume of its automation") {
    Engine engine(48000, 1);
    toh::core::Region region;
    region.clip = ones(4096, 1);
    region.length = 4096;
    REQUIRE(engine.set_track_regions(0, {region}, {}, nullptr, 0, {{0, 0.0f}, {1000, 1.0f}}));
    REQUIRE(engine.play(false));
    std::vector<float> out(2048);
    for (int block = 0; block < 4; ++block) {
        engine.render(out.data() + block * 512, 512);
    }
    CHECK(out[250] == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(out[500] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(out[1500] == doctest::Approx(1.0f));
}

TEST_CASE("Pan automation decides the pan instead of the pan control") {
    Engine engine(48000, 2);
    toh::core::Region region;
    region.clip = ones(4096, 2);
    region.length = 4096;
    REQUIRE(engine.set_track_pan(0, 1.0f));  // the control says right; the curve says left
    REQUIRE(engine.set_track_regions(0, {region}, {}, nullptr, 0, {}, {{0, -1.0f}, {1000, 0.0f}}));
    REQUIRE(engine.play(false));
    std::vector<float> out(2 * 2048);
    for (int block = 0; block < 4; ++block) {
        engine.render(out.data() + 2 * block * 512, 512);
    }
    CHECK(out[2 * 10] == doctest::Approx(1.0f).epsilon(0.02));      // left
    CHECK(out[2 * 10 + 1] == doctest::Approx(0.0f).epsilon(0.02));  // right
    CHECK(out[2 * 1500] == doctest::Approx(1.0f));                  // centre: both full
    CHECK(out[2 * 1500 + 1] == doctest::Approx(1.0f));

    Session session;
    REQUIRE(session.add_track("take", ones(48000, 2)));
    REQUIRE(session.set_track_pan_curve(0, {{0, -3.0f}, {1000, 0.25f}}));
    CHECK(session.tracks()[0].pan_curve[0].value == doctest::Approx(-1.0f));
    const auto folder = std::filesystem::temp_directory_path() / "toh_pan_automation_test";
    std::filesystem::create_directories(folder);
    std::string error;
    REQUIRE_MESSAGE(session.save_project(folder / "p.tohp", &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(folder / "p.tohp", &error), error);
    REQUIRE(reopened.tracks()[0].pan_curve.size() == 2);
    CHECK(reopened.tracks()[0].pan_curve[1].value == doctest::Approx(0.25f));
    std::filesystem::remove_all(folder);
}

TEST_CASE("Volume automation is saved, opened again and undone") {
    Session session;
    REQUIRE(session.add_track("take", ones(48000, 2)));
    session.commit_edits();
    REQUIRE(session.set_track_volume_curve(0, {{24000, 3.0f}, {0, 0.5f}}));  // unsorted, too loud
    const auto& curve = session.tracks()[0].volume_curve;
    REQUIRE(curve.size() == 2);
    CHECK(curve[0].frame == 0);
    CHECK(curve[1].value == doctest::Approx(2.0f));
    session.commit_edits();
    CHECK(session.undo_label() == "Volume automation");

    const auto folder = std::filesystem::temp_directory_path() / "toh_automation_test";
    std::filesystem::create_directories(folder);
    std::string error;
    REQUIRE_MESSAGE(session.save_project(folder / "a.tohp", &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(folder / "a.tohp", &error), error);
    REQUIRE(reopened.tracks()[0].volume_curve.size() == 2);
    CHECK(reopened.tracks()[0].volume_curve[0].value == doctest::Approx(0.5f));
    CHECK(reopened.tracks()[0].volume_curve[1].frame == 24000);
    std::filesystem::remove_all(folder);

    REQUIRE(session.undo());
    CHECK(session.tracks()[0].volume_curve.empty());
}
