// (longshot) The curl side of the perf summary: find the system's client, build its arguments, read the answer's status.
// Process spawning is perf_os's job; the keep/drop rule is perf_report.cpp's.
#include "perf_curl.h"

#include "perf_os.h"
#include "perf_report.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mkwvk::perf_curl {
namespace {

// Windows ships curl.exe in System32; Linux distributions put it on the usual PATH locations.
bool find_curl(std::filesystem::path& out) {
#ifdef RX_VK_PERF_TEST
    const char* stub = std::getenv("RX_VK_PERF_TEST_CURL");
    if (stub && *stub) { out = stub; return true; }
#endif
#ifdef _WIN32
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    const int len = WideCharToMultiByte(CP_UTF8, 0, dir, n, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir, n, utf8.data(), len, nullptr, nullptr);
    out = std::filesystem::u8path(utf8 + "\\curl.exe");
    std::error_code ec;
    return std::filesystem::exists(out, ec) && !ec;
#else
    static const char* const places[] = {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "/run/host/usr/bin/curl"};
    for (const char* p : places) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec && access(p, X_OK) == 0) { out = p; return true; }
    }
    return false;
#endif
}

}   // namespace

int post(const std::filesystem::path& summary, const std::filesystem::path& headers,
         const std::filesystem::path& answer, const std::string& url) {
    std::filesystem::path curl;
    if (!find_curl(curl)) return 0;
    // --disable first so a player's .curlrc cannot add a redirect, a proxy, --insecure or an --output of its own.
    const std::vector<std::string> args = {
        "--disable", "--silent", "--show-error", "--max-time", "8", "--connect-timeout", "3", "--proto", "=https",
        "--header", "Content-Type: application/json", "--user-agent", "longshot-perf",
        "--data-binary", "@" + perf_os::argv_path(summary), "--dump-header", perf_os::argv_path(headers),
        "--output", perf_os::argv_path(answer), url};
    if (!perf_os::run(curl, args)) return 0;
    std::ifstream f(headers, std::ios::binary);
    return perf_report::http_status(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
}

}   // namespace mkwvk::perf_curl
