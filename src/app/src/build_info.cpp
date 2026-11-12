#include "toh/app/build_info.h"

// TOH_VERSION and TOH_GIT_COMMIT are set for this file by src/app/CMakeLists.txt.
#ifndef TOH_VERSION
#define TOH_VERSION "0.0.0"
#endif
#ifndef TOH_GIT_COMMIT
#define TOH_GIT_COMMIT "unknown"
#endif

namespace toh::app {

const BuildInfo& build_info() noexcept {
    static const BuildInfo info{TOH_VERSION, TOH_GIT_COMMIT, __DATE__};
    return info;
}

}  // namespace toh::app
