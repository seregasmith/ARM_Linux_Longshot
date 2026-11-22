"""Execute native presentation's actual Vulkan result-routing sites without a GPU.

The existing test_device_lost_exit.py checks the shared report-once/log-flush/exit
contract. This check injects device loss into each WSI call site, including both
present queues, and checks that execution cannot pass the loss unreported.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']

ROOT = Path(__file__).resolve().parents[1]
CALLS = ("vkWaitForFences", "vkAcquireNextImageKHR", "vkResetFences", "vkResetCommandPool",
         "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkQueueSubmit2", "vkQueuePresentKHR")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    text = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    entry = re.search(r"^void present_image_with_trial\([^\n]*?\) \{$", text, re.M)
    if not entry:
        entry = re.search(r"^void present_image\([^\n]*?\) \{$", text, re.M)
    if not entry:
        raise SystemExit("FAIL: native present routine is missing")
    present = text[entry.start():]
    present = present[:present.index("\nnamespace {\n/// What follows a present")]
    lines = present.splitlines()
    # (the present's submit goes through submit_or_retry since #2081: its vkQueueSubmit2, made again only while the driver
    # refuses it for want of memory (submit_retry.h), and the result returned as it came)
    calls = CALLS
    if not re.search(r"\bvkQueueSubmit2\(", present) and re.search(r"\bsubmit_or_retry\(", present):
        retry = re.search(r"^VkResult submit_or_retry\(const VkSubmitInfo2& si, VkFence fence\) \{$.*?^\}$", text, re.M | re.S)
        if not retry or not re.search(r"\bvkQueueSubmit2\(dev\.queue, 1, &si, fence\)", retry.group(0)) \
                or "    return res;\n}" not in retry.group(0):
            raise SystemExit("FAIL: submit_or_retry does not return its vkQueueSubmit2's result")
        calls = tuple("submit_or_retry" if call == "vkQueueSubmit2" else call for call in CALLS)
    sites = []
    for call in calls:
        found = [i for i, line in enumerate(lines) if re.search(r"\b" + call + r"\(", line)]
        # (two presents: the replay's queue and the presents' own; two acquires: at the present, and acquire ahead's on the
        # presenter thread right after one, acquire_ahead)
        if len(found) != (2 if call in ("vkQueuePresentKHR", "vkAcquireNextImageKHR") else 1):
            raise SystemExit("FAIL: presentation call sites changed: " + call)
        for i in found:
            snippet = lines[i].strip()
            if "device_lost();" not in snippet:
                following = lines[i + 1:]
                if call == "vkQueuePresentKHR":
                    following = following[:following.index("    reflex_presented();")]
                else:
                    following = following[:1]
                snippet += "\n" + "\n".join(line for line in following
                                                  if line.strip() == "if (res == VK_ERROR_DEVICE_LOST) device_lost();")
            if "VkResult res =" not in snippet:
                snippet = "VkResult res = VK_SUCCESS; (void)res;\n" + snippet
            sites.append((call, snippet))

    fixture = r'''
#include <cstdint>
#include <cstdio>
using VkResult = int;
enum { VK_SUCCESS=0, VK_ERROR_DEVICE_LOST=-4, VK_TRUE=1, VK_NULL_HANDLE=0 };
struct { int device=1, queue=2, present_queue=3, swapchain=4; } dev;
int sc=4;   // (acquire ahead's acquire: on the swapchain the presenter's present went to)
struct { int done=5, pool=7, cmd=8; } f;
bool was_held=false; int acquire_fence=6;   // (#1376: the acquire's fence, and no image held from an earlier present)
int bi=0, si=0, pi=0, index=0;
uint64_t acquire_timeout=UINT64_MAX;
int result=VK_SUCCESS, calls=0, reports=0, continued=0;
struct Lost {};
[[noreturn]] void device_lost() { ++reports; throw Lost{}; }
'''
    for call in calls:
        fixture +="template<class... A> VkResult " + call + "(A...) { ++calls; return result; }\n"
    for i, (_, snippet) in enumerate(sites):
        fixture += "void site_%d() {\n%s\n(void)res; ++continued;\n}\n" % (i, snippet)
    fixture += "int main() {\nint failed=0;\n"
    for i, (name, _) in enumerate(sites):
        for result in (0, -4, -1, 2, 1000001003, -1000001004):
            fixture += ("result=%d; calls=reports=continued=0; try { site_%d(); } catch (const Lost&) {}\n"
                        "if (calls!=1 || reports!=%d || continued!=%d) { "
                        "std::printf(\"FAIL: %s site%d result%%d reports%%d continued%%d\\n\",result,reports,continued); ++failed; }\n"
                        % (result, i, int(result == -4), int(result != -4), name, i))
    fixture += 'if (!failed) { std::puts("PASS: all ten native WSI sites route device loss before continuing; other results preserved"); }\nreturn failed ? 1 : 0;\n}\n'
    with tempfile.TemporaryDirectory(prefix="present-device-loss-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
