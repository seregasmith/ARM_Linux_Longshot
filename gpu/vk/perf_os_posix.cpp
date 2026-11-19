// (longshot) Linux side of perf_os.h: posix_spawn with every child stream at /dev/null, waited for off the frame path.
#ifndef _WIN32

#include "perf_os.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" char** environ;

namespace mkwvk::perf_os {

std::string argv_path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool run(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    const std::string exe = argv_path(executable);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : arguments) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int err = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err) return false;
    int status = 0;
    return waitpid(pid, &status, 0) == pid;
}

}   // namespace mkwvk::perf_os

#endif   // !_WIN32
