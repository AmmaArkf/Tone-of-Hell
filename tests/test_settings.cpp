// Tests for the settings file, translations and the input gain.

#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/i18n.h"
#include "toh/app/settings.h"
#include "toh/core/engine.h"

using toh::app::Action;
using toh::app::Language;
using toh::app::Settings;

namespace {

std::filesystem::path temp_file(const char* name) {
    return std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
}

}  // namespace

TEST_CASE("Settings start with the default shortcuts") {
    const Settings settings;
    CHECK(settings.language == Language::English);
    CHECK(settings.shortcut(Action::PlayPause) == "Space");
    CHECK(settings.shortcut(Action::AddFiles) == "Ctrl+O");
    CHECK(settings.shortcut(Action::Stop).empty());
    CHECK(settings.output_device.empty());
    CHECK(settings.record_offset_frames < 0);

    // Every action has an id and a name, and no two share an id.
    for (std::size_t i = 0; i < toh::app::kActionCount; ++i) {
        const auto& info = toh::app::action_info(static_cast<Action>(i));
        CHECK(static_cast<std::size_t>(info.action) == i);
        CHECK(std::strlen(info.id) > 0);
        CHECK(std::strlen(info.name) > 0);
        for (std::size_t j = 0; j < i; ++j) {
            CHECK(std::strcmp(info.id, toh::app::action_info(static_cast<Action>(j)).id) != 0);
        }
    }
}

TEST_CASE("Settings survive saving and loading") {
    const auto path = temp_file("toh_test_\xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8.txt");

    Settings saved;
    saved.language = Language::Russian;
    saved.output_device = "\xD0\x94\xD0\xB8\xD0\xBD\xD0\xB0\xD0\xBC\xD0\xB8\xD0\xBA\xD0\xB8 (USB Audio = 2)";
    saved.input_device = "Microphone";
    saved.master_gain = 0.75f;
    saved.input_gain = 2.5f;
    saved.record_offset_frames = 1234;
    saved.shortcuts[static_cast<std::size_t>(Action::Split)] = "Ctrl+Shift+X";
    saved.shortcuts[static_cast<std::size_t>(Action::Record)] = "";
    REQUIRE(saved.save(path));

    Settings loaded;
    REQUIRE(loaded.load(path));
    CHECK(loaded.language == Language::Russian);
    CHECK(loaded.output_device == saved.output_device);  // a name with "=" and other letters
    CHECK(loaded.input_device == "Microphone");
    CHECK(loaded.master_gain == doctest::Approx(0.75f));
    CHECK(loaded.input_gain == doctest::Approx(2.5f));
    CHECK(loaded.record_offset_frames == 1234);
    CHECK(loaded.shortcut(Action::Split) == "Ctrl+Shift+X");
    CHECK(loaded.shortcut(Action::Record).empty());
    CHECK(loaded.shortcut(Action::PlayPause) == "Space");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("Settings ignore what they do not understand") {
    const auto path = temp_file("toh_test_settings_odd.txt");
    {
        std::ofstream file(path, std::ios::binary);
        file << "# a comment\n"
             << "nonsense line\n"
             << "future_option=42\n"
             << "master_gain=banana\n"
             << "input_gain=99\n"
             << "  language = ru  \r\n"
             << "shortcut.unknown_action=Q\n"
             << "shortcut.loop=Ctrl+L\n";
    }
    Settings settings;
    REQUIRE(settings.load(path));
    CHECK(settings.language == Language::Russian);
    CHECK(settings.master_gain == 1.0f);  // not a number: the default stays
    CHECK(settings.input_gain == 1.0f);   // out of range: the default stays
    CHECK(settings.shortcut(Action::Loop) == "Ctrl+L");
    CHECK(settings.shortcut(Action::PlayPause) == "Space");

    CHECK_FALSE(settings.load(temp_file("toh_test_no_such_file.txt")));

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("Translation falls back to English") {
    using toh::app::tr;
    const char* const play = "Play";
    const char* const unknown = "A text nobody translated";

    toh::app::set_language(Language::English);
    CHECK(tr(play) == play);
    CHECK(tr(nullptr) == nullptr);

    toh::app::set_language(Language::Russian);
    CHECK(toh::app::language() == Language::Russian);
    CHECK(std::strcmp(tr(play), play) != 0);
    CHECK(tr(unknown) == unknown);
    // Format texts keep their specifiers.
    CHECK(std::strstr(tr("Master: %.0f%%"), "%.0f%%") != nullptr);
    CHECK(std::strstr(tr("Record##record"), "##record") != nullptr);
    // Every action name has a translation.
    for (std::size_t i = 0; i < toh::app::kActionCount; ++i) {
        const char* name = toh::app::action_info(static_cast<Action>(i)).name;
        CHECK_MESSAGE(std::strcmp(tr(name), name) != 0, name);
    }

    toh::app::set_language(Language::English);
}

TEST_CASE("The input gain scales what is monitored, recorded and metered") {
    toh::core::Engine engine(48000, 1);
    std::vector<float> in(9600, 0.4f);
    std::vector<float> out(in.size());

    REQUIRE(engine.set_input_gain(0.5f));
    REQUIRE(engine.set_monitoring(true));
    engine.render(in.data(), out.data(), 9600);
    CHECK(out.back() == doctest::Approx(0.2f).epsilon(0.001));
    CHECK(engine.input_peak() == doctest::Approx(0.2f).epsilon(0.05));

    REQUIRE(engine.start_recording());
    REQUIRE(engine.play());
    engine.render(in.data(), out.data(), 1000);
    std::vector<float> recorded(1000);
    REQUIRE(engine.read_recorded(recorded.data(), 1000) == 1000);
    CHECK(recorded[999] == doctest::Approx(0.2f).epsilon(0.001));

    CHECK_FALSE(engine.set_input_gain(-1.0f));
    CHECK_FALSE(engine.set_input_gain(100.0f));
}
