#pragma once

namespace toh::app {

/// Which build of the program this is, for status bars and "About" windows.
struct BuildInfo {
    const char* version;  ///< Project version, for example "0.0.1".
    const char* commit;   ///< Short Git commit the build was configured from, or "unknown".
    const char* date;     ///< Date this information was compiled, for example "Nov 12 2026".
};

const BuildInfo& build_info() noexcept;

}  // namespace toh::app
