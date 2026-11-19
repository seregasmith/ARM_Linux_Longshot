// (longshot) Windows side of perf_os.h: CreateProcessW with no window and no inherited handles, waited for off the
// frame path. Quoting is cmd.exe's and all paths are UTF-8, never the system's ANSI code page.
#ifdef _WIN32

#include "perf_os.h"

#include <cstring>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mkwvk::perf_os {

std::string argv_path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

namespace {

// A backslash before a quote, and runs of them before one, are escaped.
std::string quote(const std::string& s) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        size_t bs = 0;
        while (i < s.size() && s[i] == '\\') bs++, i++;
        if (i == s.size()) { o.append(2 * bs, '\\'); break; }
        if (s[i] == '"') { o.append(2 * bs + 1, '\\'), o += '"'; }
        else { o.append(bs, '\\'), o += s[i]; }
    }
    return o + "\"";
}

}   // namespace

bool run(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::string line = quote(argv_path(executable));
    for (const auto& a : arguments) line += " " + quote(a);
    const int need = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
    if (need <= 0) return false;
    std::wstring wide(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), wide.data(), need);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si), memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    const DWORD waited = WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return waited == WAIT_OBJECT_0;
}

}   // namespace mkwvk::perf_os

#endif   // _WIN32
