#include "toh/app/log.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <system_error>
#include <vector>

#include "toh/core/paged_file.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace toh::app::logging {
namespace {

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::FILE* g_file = nullptr;  // guarded by log_mutex(), except in the crash handler
std::filesystem::path g_path;

// The local time, as "2026-11-13 11:23:45.123" or, for file names, "2026-11-13_11-23-45".
std::string timestamp(bool for_file_name) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char base[32];
    std::strftime(base, sizeof(base), for_file_name ? "%Y-%m-%d_%H-%M-%S" : "%Y-%m-%d %H:%M:%S",
                  &local);
    if (for_file_name) {
        return base;
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%s.%03d", base, millis);
    return text;
}

#ifdef _WIN32
LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    // No lock: the crashing thread may be the one that holds it.
    if (g_file != nullptr) {
        const EXCEPTION_RECORD* record = info != nullptr ? info->ExceptionRecord : nullptr;
        std::fprintf(g_file, "%s [CRASH] exception 0x%08lX at address %p\n", timestamp(false).c_str(),
                     record != nullptr ? static_cast<unsigned long>(record->ExceptionCode) : 0ul,
                     record != nullptr ? record->ExceptionAddress : nullptr);
        std::fflush(g_file);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // Windows goes on as usual (error report, exit)
}
#else
void on_signal(int number) {
    if (g_file != nullptr) {
        std::fprintf(g_file, "[CRASH] stopped by signal %d\n", number);
        std::fflush(g_file);
    }
    std::signal(number, SIG_DFL);
    std::raise(number);
}
#endif

}  // namespace

bool start(const std::filesystem::path& folder, std::size_t keep) {
    const std::lock_guard<std::mutex> lock(log_mutex());
    if (g_file != nullptr) {
        return true;
    }
    std::error_code code;
    std::filesystem::create_directories(folder, code);

    // The names hold the date and time, so sorting them puts the oldest first.
    std::vector<std::filesystem::path> logs;
    for (std::filesystem::directory_iterator it(folder, code), end; !code && it != end;
         it.increment(code)) {
        const std::string name = it->path().filename().u8string();
        if (name.rfind("session-", 0) == 0 && it->path().extension() == ".log") {
            logs.push_back(it->path());
        }
    }
    std::sort(logs.begin(), logs.end());
    while (!logs.empty() && logs.size() >= std::max<std::size_t>(keep, 1)) {
        std::error_code ignored;
        std::filesystem::remove(logs.front(), ignored);
        logs.erase(logs.begin());
    }

    g_path = folder / ("session-" + timestamp(true) + ".log");
    g_file = core::open_file(g_path, "wb");
    if (g_file == nullptr) {
        g_path.clear();
        return false;
    }
    return true;
}

void stop() {
    const std::lock_guard<std::mutex> lock(log_mutex());
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_path.clear();
}

bool running() {
    const std::lock_guard<std::mutex> lock(log_mutex());
    return g_file != nullptr;
}

std::filesystem::path file() {
    const std::lock_guard<std::mutex> lock(log_mutex());
    return g_path;
}

void write(const char* level, const std::string& message) {
    const std::lock_guard<std::mutex> lock(log_mutex());
    if (g_file == nullptr) {
        return;
    }
    const std::string line = timestamp(false) + " [" + level + "] " + message + "\n";
    std::fwrite(line.data(), 1, line.size(), g_file);
    std::fflush(g_file);  // a crash right after must not lose the line
}

void install_crash_handler() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(&on_crash);
#else
    for (const int number : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
        std::signal(number, &on_signal);
    }
#endif
}

}  // namespace toh::app::logging
