#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mkwvk::perf_os {

/// Run an external program with its output nowhere and wait for it. True when it ran to completion within 10 s, false
/// when it could not start or hung. This is the whole Windows/Linux boundary: no caller needs an #ifdef for spawning.
/// The platform code is in perf_os_win.cpp / perf_os_posix.cpp; a new OS is one more file behind this interface.
bool run(const std::filesystem::path& executable, const std::vector<std::string>& arguments);

/// A path as UTF-8 bytes for a child process's argv (u8string is char8_t; argv is plain char on both platforms).
std::string argv_path(const std::filesystem::path& path);

}   // namespace mkwvk::perf_os
