"""The game-side performance sender's keep/drop/retry policy (gpu/vk/perf_report.cpp): a relay that is busy or cannot be
reached stops a startup retry pass, so a backlog does not get posted all at once (the old launcher sender's rule, issue
1265), and the exit send never queues behind an earlier curl: while a send is in flight the last summary stays in
perf_pending for the next start instead of holding the game's close for a second wait.

The test cuts send_summary() and send_pending() out of the real source and compiles them with a stand-in curl, so it
reads what this branch would send, not a copy of the fix.

python -I -B tests/test_perf_send.py [--cxx g++]
"""
import argparse
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def fixture():
    source = (ROOT / "gpu" / "vk" / "perf_report.cpp").read_text(encoding="utf-8")
    summary = re.search(r"^(?:void|pr::SendResult) send_summary\(const std::string& name\)\s*\{", source, re.M)
    pending = re.search(r"^void send_pending\([^)]*\)\s*\{", source, re.M)
    wrote = re.search(r"^void write_summary\(const std::filesystem::path& dir, const std::string& name, "
                      r"const std::string& json\)\s*\{", source, re.M)
    if not summary or not pending or not wrote:
        raise SystemExit("perf_report.cpp: send_summary/send_pending/write_summary were renamed: cut them by hand here")
    cut = "\n\n".join(block(source, m.group(0)[:-1].rstrip()) for m in (summary, pending, wrote))
    return r'''#include "perf_report.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pr = mkwvk::perf_report;
namespace fsys = std::filesystem;

/* stand-ins for what perf_report.cpp gives these functions */
static fsys::path folder;
static std::string send_url = "https://relay/telemetry/perf";
static std::mutex send_mu;
static bool keep_sent = false;
static std::atomic<bool> shutting_down{false};
constexpr int kMaxPending = 48, kDrainPerSummary = 12;
static int post_status = 429, posts = 0;
static void logf(const char*, ...) {}

namespace mkwvk { namespace perf_curl {
int post(const fsys::path&, const fsys::path&, const fsys::path&, const std::string&) { posts++; return post_status; }
}}

''' + "namespace mkwvk {\n" + cut + "\n}\nusing mkwvk::send_summary;\nusing mkwvk::send_pending;\nusing mkwvk::write_summary;\n" + r'''

static void queue(const char* name) {
    fsys::create_directory(folder / "perf_pending");
    std::ofstream f(folder / "perf_pending" / (std::string(name) + ".json"));
    f << "{}";
}
static int left() {
    int n = 0;
    std::error_code ec;
    for (fsys::directory_iterator it(folder / "perf_pending", ec), end; !ec && it != end; it.increment(ec))
        n += it->path().extension() == ".json";
    return n;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    logf("perf send fixture");   /* also keeps the stand-in used before the fix adds its own log line */
    folder = argv[1];
    for (const char* n : {"100.json", "200.json", "300.json"}) queue(n);
    post_status = 429, posts = 0;
    send_pending();
    if (posts != 1 || left() != 3) {
        std::fprintf(stderr, "FAIL: a busy relay got %d POSTs, %d summaries left (want 1, 3)\n", posts, left());
        return 1;
    }

    post_status = 200, posts = 0;
    send_pending();
    if (posts != 3 || left() != 0) {
        std::fprintf(stderr, "FAIL: a working relay got %d POSTs, %d summaries left (want 3, 0)\n", posts, left());
        return 1;
    }

    /* the exit's send with a curl still in flight: it must not wait for that curl's turn */
    queue("400");
    post_status = 200, posts = 0;
    shutting_down = true;
    std::unique_lock<std::mutex> held(send_mu);
    std::atomic<bool> done{false};
    std::thread t([&] { send_summary("400"); done = true; });
    for (int i = 0; i < 100 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!done) {
        std::fprintf(stderr, "FAIL: the exit send waited behind an in-flight curl\n");
        std::exit(1);   /* the thread is still blocked: end this check, not the game */
    }
    t.join();
    held.unlock();
    if (posts != 0 || left() != 1) {
        std::fprintf(stderr, "FAIL: the deferred exit send posted %d times, %d summaries left (want 0, 1)\n", posts, left());
        return 1;
    }

    /* a full queue with a recovered relay: the periodic writer drains older files and keeps the new one */
    {
        for (int i = 0; i < kMaxPending; i++) {
            char n[16];
            std::snprintf(n, sizeof n, "old%03d", i);
            queue(n);
        }
        post_status = 200, posts = 0;
        keep_sent = true;
        write_summary(folder, "fresh", "{}");
        keep_sent = false;
        const bool kept = fsys::exists(folder / "perf_sent" / "fresh.json");
        if (!kept || left() >= kMaxPending || posts <= kDrainPerSummary) {
            std::fprintf(stderr, "FAIL: a full queue kept %d waiting with %d POSTs, fresh kept=%d\n", left(), posts, kept);
            return 1;
        }
        fsys::remove_all(folder / "perf_sent");
    }

    /* the same full queue while the relay is busy: one attempt, not one per queued file */
    {
        for (int i = 0; left() < kMaxPending; i++) {
            char n[16];
            std::snprintf(n, sizeof n, "extra%03d", i);
            queue(n);
        }
        post_status = 429, posts = 0;
        write_summary(folder, "later", "{}");
        if (posts > 1) {
            std::fprintf(stderr, "FAIL: a busy relay got %d POSTs from the periodic drain (want 1)\n", posts);
            return 1;
        }
    }

    std::printf("PASS: a busy relay stops the retry pass, the exit send never waits, a full queue drains when the relay recovers\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    a = p.parse_args()
    cxx = shutil.which(a.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + a.cxx)
    with tempfile.TemporaryDirectory(prefix="longshot-perf-send-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        folder = Path(tmp) / "game"
        folder.mkdir()
        cpp.write_text(fixture(), encoding="utf-8")
        r = subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: the sender fixture does not compile\n" + r.stdout + r.stderr)
        r = subprocess.run([str(exe), str(folder)], capture_output=True, text=True, timeout=60)
        print(r.stdout, end="")
        print(r.stderr, end="", file=sys.stderr)
        if r.returncode:
            raise SystemExit("FAIL: the game-side sender's retry and exit policy")
    print("PASS: game-side sender policy (no game required)")


if __name__ == "__main__":
    main()
