#pragma once

#include <filesystem>
#include <string>

namespace mkwvk::perf_curl {

/// POST one JSON file to an https URL with the system's curl. The headers and answer paths are writer-owned scratch.
/// Returns the relay's HTTP status, 0 when curl is missing, could not run, or gave no answer. This is the only file that
/// knows about curl; spawning it goes through perf_os, and what to keep is decided by perf_report.cpp.
int post(const std::filesystem::path& summary, const std::filesystem::path& headers,
         const std::filesystem::path& answer, const std::string& url);

}   // namespace mkwvk::perf_curl
