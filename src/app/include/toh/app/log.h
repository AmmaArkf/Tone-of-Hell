#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace toh::app::logging {

/// A log file for every session of the program. When users report a problem, its log says what
/// happened (devices, files, errors, a crash). Every function may be called from any thread;
/// writing does nothing while no log is open.

/// Opens a new log file in `folder`, named after the date and time ("session-....log"), and
/// removes the oldest logs so that at most `keep` remain. Returns false if no file can be
/// created.
bool start(const std::filesystem::path& folder, std::size_t keep = 20);
/// Closes the log file.
void stop();
bool running();
/// The file of the running log; empty when there is none.
std::filesystem::path file();

/// Writes one line with the time and `level` ("INFO", "WARN", "ERROR").
void write(const char* level, const std::string& message);
inline void info(const std::string& message) { write("INFO", message); }
inline void warning(const std::string& message) { write("WARN", message); }
inline void error(const std::string& message) { write("ERROR", message); }

/// Makes a crash (Windows) or a fatal signal (Linux) leave a last line in the log.
void install_crash_handler();

}  // namespace toh::app::logging
