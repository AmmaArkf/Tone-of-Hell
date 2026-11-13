// Tests for SoundFont (.sf2) instruments. The test builds a tiny SoundFont in memory: one preset
// with a looped sine wave whose pitch is A4 (441 Hz at 44.1 kHz).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/soundfont.h"

using toh::core::Note;
using toh::core::SoundFont;

namespace {

void put16(std::vector<char>& b, unsigned value) {
    b.push_back(static_cast<char>(value & 0xFF));
    b.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put32(std::vector<char>& b, unsigned value) {
    put16(b, value & 0xFFFF);
    put16(b, (value >> 16) & 0xFFFF);
}

void put_name(std::vector<char>& b, const char* name, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        b.push_back(i < std::strlen(name) ? name[i] : '\0');
    }
}

std::vector<char> chunk(const char* tag, const std::vector<char>& body) {
    std::vector<char> b(tag, tag + 4);
    put32(b, static_cast<unsigned>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() % 2 != 0) {
        b.push_back('\0');
    }
    return b;
}

std::vector<char> list(const char* type, const std::vector<std::vector<char>>& chunks) {
    std::vector<char> body(type, type + 4);
    for (const auto& c : chunks) {
        body.insert(body.end(), c.begin(), c.end());
    }
    return chunk("LIST", body);
}

std::vector<char> tiny_soundfont() {
    std::vector<char> smpl;
    for (int i = 0; i < 2000; ++i) {
        const double value = std::sin(6.283185307179586 * i / 100.0) * 16000.0;
        put16(smpl, static_cast<unsigned>(static_cast<std::int16_t>(value)) & 0xFFFF);
    }
    for (int i = 0; i < 46; ++i) {
        put16(smpl, 0);
    }

    std::vector<char> phdr;
    put_name(phdr, "Test sine", 20);
    put16(phdr, 0);  // program
    put16(phdr, 0);  // bank
    put16(phdr, 0);  // first bag
    put32(phdr, 0);
    put32(phdr, 0);
    put32(phdr, 0);
    put_name(phdr, "EOP", 20);
    put16(phdr, 0);
    put16(phdr, 0);
    put16(phdr, 1);
    put32(phdr, 0);
    put32(phdr, 0);
    put32(phdr, 0);

    std::vector<char> pbag;
    put16(pbag, 0);
    put16(pbag, 0);
    put16(pbag, 1);
    put16(pbag, 0);
    std::vector<char> pmod(10, '\0');
    std::vector<char> pgen;
    put16(pgen, 41);  // instrument 0
    put16(pgen, 0);
    put16(pgen, 0);
    put16(pgen, 0);

    std::vector<char> inst;
    put_name(inst, "Sine", 20);
    put16(inst, 0);
    put_name(inst, "EOI", 20);
    put16(inst, 1);
    std::vector<char> ibag;
    put16(ibag, 0);
    put16(ibag, 0);
    put16(ibag, 2);
    put16(ibag, 0);
    std::vector<char> imod(10, '\0');
    std::vector<char> igen;
    put16(igen, 54);  // sample modes: loop
    put16(igen, 1);
    put16(igen, 53);  // sample 0
    put16(igen, 0);
    put16(igen, 0);
    put16(igen, 0);

    std::vector<char> shdr;
    put_name(shdr, "Sine", 20);
    put32(shdr, 0);      // start
    put32(shdr, 2000);   // end
    put32(shdr, 100);    // loop start
    put32(shdr, 1900);   // loop end
    put32(shdr, 44100);  // sample rate
    shdr.push_back(69);  // original pitch: A4
    shdr.push_back(0);   // pitch correction
    put16(shdr, 0);
    put16(shdr, 1);      // mono
    put_name(shdr, "EOS", 46);

    std::vector<char> ifil;
    put16(ifil, 2);
    put16(ifil, 1);
    const std::vector<char> body_info = list("INFO", {chunk("ifil", ifil)});
    const std::vector<char> body_sdta = list("sdta", {chunk("smpl", smpl)});
    const std::vector<char> body_pdta =
        list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod),
                      chunk("pgen", pgen), chunk("inst", inst), chunk("ibag", ibag),
                      chunk("imod", imod), chunk("igen", igen), chunk("shdr", shdr)});
    std::vector<char> riff_body{'s', 'f', 'b', 'k'};
    for (const auto* part : {&body_info, &body_sdta, &body_pdta}) {
        riff_body.insert(riff_body.end(), part->begin(), part->end());
    }
    return chunk("RIFF", riff_body);
}

// Upward zero crossings of the left channel of a note over `frames` frames at 48 kHz.
int crossings(const SoundFont& font, const SoundFont::Preset& preset, int pitch, int frames) {
    Note note;
    note.pitch = static_cast<std::uint8_t>(pitch);
    note.length = 48000;
    int count = 0;
    float previous = 0.0f;
    for (int f = 0; f < frames; ++f) {
        float left = 0.0f;
        float right = 0.0f;
        font.add_note(preset, note, 2000.0 + f, 48000.0, left, right);
        if (previous < 0.0f && left >= 0.0f) {
            ++count;
        }
        previous = left;
    }
    return count;
}

}  // namespace

TEST_CASE("A SoundFont is read and plays its sample at the right pitch") {
    std::string error;
    const auto font = SoundFont::parse(tiny_soundfont(), &error);
    REQUIRE_MESSAGE(font, error);
    REQUIRE(font->presets().size() == 1);
    const SoundFont::Preset* preset = font->find(0);
    REQUIRE(preset != nullptr);
    CHECK(preset->name == "Test sine");
    CHECK(font->find(5) == nullptr);
    CHECK(font->find(SoundFont::kDrums) == nullptr);

    // A4 plays the sample as recorded (441 Hz), A5 an octave higher. The loop keeps it going
    // far past the 2000 recorded frames.
    CHECK(crossings(*font, *preset, 69, 4800) == doctest::Approx(44).epsilon(0.08));
    CHECK(crossings(*font, *preset, 81, 4800) == doctest::Approx(88).epsilon(0.08));

    Note note;
    note.pitch = 69;
    note.length = 4800;
    float left = 0.0f;
    float right = 0.0f;
    font->add_note(*preset, note, 30000.0, 48000.0, left, right);  // long after the note
    CHECK(left == 0.0f);
    CHECK(right == 0.0f);

    CHECK_FALSE(SoundFont::parse(std::vector<char>(100, 'x'), &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Instrument tracks play a program of the SoundFont") {
    const auto font = SoundFont::parse(tiny_soundfont());
    REQUIRE(font);
    toh::app::Session session;
    session.set_soundfont(font);
    REQUIRE(session.add_instrument_track("Sine", toh::core::Instrument::Keys, 0, 48000, 0) == 0);
    CHECK(session.tracks()[0].program == 0);
    Note note;
    note.pitch = 69;
    note.length = 24000;
    REQUIRE(session.set_region_notes(0, 0, {note}));
    session.play();
    std::vector<float> out(2 * 4000);
    session.engine().render(out.data(), 4000);
    float loudest = 0.0f;
    for (const float sample : out) {
        loudest = std::max(loudest, std::fabs(sample));
    }
    CHECK(loudest > 0.05f);

    REQUIRE(session.set_track_program(0, -1));  // back to the built-in synthesizer
    CHECK(session.tracks()[0].program == -1);
    CHECK_FALSE(session.set_track_program(0, 300));
}
