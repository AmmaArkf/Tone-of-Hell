#pragma once

namespace toh::app {

/// Languages the user interfaces can be shown in.
enum class Language { English, Russian };

/// Chooses the language used by tr(). Call it from the UI thread only.
void set_language(Language language) noexcept;
Language language() noexcept;

/// Translates a text of the user interface. The English text itself is the key; texts without
/// a translation (and all texts while the language is English) are returned unchanged. The
/// returned pointer stays valid for the lifetime of the program.
///
/// printf-style texts keep their format specifiers in the same order in every language, and
/// "##id" suffixes used by Dear ImGui are part of the key.
const char* tr(const char* english) noexcept;

}  // namespace toh::app
