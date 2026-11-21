"""Exercise the replay clear decision with guest packets and aliased memory mappings."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import between, block

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text()
    fixture = r"""
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>
#define _Static_assert static_assert
#include "rx_nvn.h"
uint8_t backing[4096];
struct Host {
    uint8_t* gpu_ptr(uint64_t addr, uint64_t* available) {
        if (!addr) return nullptr;
        *available = sizeof(backing) - addr % sizeof(backing);
        return backing + addr % sizeof(backing);
    }
} fake_host, *host = &fake_host;
bool pass_trace_on() { return false; }
void trace_line(const char*, ...) {}
struct State { bool render_enabled = true; nvn_p_set_rt rt{}; };
void dry_mark_written(State&, bool, bool) {}
int target;
int* depth_clear_before_copy(State&, const nvn_p_copy_tt&) { return &target; }
bool recorded;
void clear_depth_stencil(int, State&, float, bool, uint32_t, uint32_t) { recorded = true; }
template<class T> T payload(const uint8_t* p, size_t n) {
    T value{};
    std::memcpy(&value, p, std::min(n, sizeof(T)));
    return value;
}
"""
    fixture += block(replay, "bool depth_copy_report_disjoint(")
    fixture += r"""
bool candidate(std::vector<uint8_t>& packets, size_t limit) {
    const uint8_t* p = packets.data();
    const uint8_t* end = p + limit;
    nvn_op_hdr h = payload<nvn_op_hdr>(p, sizeof h);
    const auto* a = p + sizeof h;
    uint32_t avail = h.size - sizeof h;
    int cmd = 0;
    State s;
    bool worker = false, dry = false;
    int* cleared = nullptr;
    float clear_value = 0;
    recorded = false;
    switch (h.op) {
"""
    fixture += between(replay, "case NOP_CLEAR_DS:", "case NOP_DRAW:")
    fixture += r"""
    }
    assert(recorded == !cleared);
    return cleared;
}
template<class T> void packet(std::vector<uint8_t>& v, uint32_t op, const T& data) {
    nvn_op_hdr h{op, uint32_t(sizeof(nvn_op_hdr) + sizeof(T))};
    size_t at = v.size();
    v.resize(at + h.size);
    std::memcpy(v.data() + at, &h, sizeof h);
    std::memcpy(v.data() + at + sizeof h, &data, sizeof data);
}
int main() {
    nvn_tex src{}, dst{};
    src.gpu = 128; dst.gpu = 512;
    src.L.size = dst.L.size = 128;
    nvn_p_copy_tt copy{}; copy.src = &src; copy.dst = &dst;
    std::vector<uint8_t> clear;
    packet(clear, NOP_CLEAR_DS, nvn_p_clear_ds{1,1,0,0});
    auto test = [&](uint64_t type, uint64_t addr, uint32_t op, bool expected) {
        auto v = clear;
        packet(v, NOP_REPORT_COUNTER, nvn_p_counter{type, addr});
        size_t boundary = v.size();
        packet(v, op, copy);
        assert(candidate(v, v.size()) == expected);
        assert(!candidate(v, boundary));
        assert(!candidate(v, v.size()-1));
    };
    test(NVN_COUNTER_TYPE_TIMESTAMP, 64, NOP_COPY_T2T, true);
    test(NVN_COUNTER_TYPE_TIMESTAMP_TOP, 256, NOP_COPY_T2T, true);
    test(NVN_COUNTER_TYPE_TIMESTAMP, 113, NOP_COPY_T2T, false);
    test(NVN_COUNTER_TYPE_TIMESTAMP, 4096+512, NOP_COPY_T2T, false);
    test(NVN_COUNTER_TYPE_TIMESTAMP, 4090, NOP_COPY_T2T, false);
    test(NVN_COUNTER_TYPE_TIMESTAMP, 0, NOP_COPY_T2T, false);
    test(NVN_COUNTER_TYPE_TIMESTAMP | NVN_COUNTER_WRITTEN, 64, NOP_COPY_T2T, false);
    test(NVN_COUNTER_TYPE_SAMPLES_PASSED, 64, NOP_COPY_T2T, false);
    test(NVN_COUNTER_TYPE_TIMESTAMP, 64, NOP_BARRIER, false);
    auto through_enable = [&](int value, uint32_t following, uint64_t addr, bool expected) {
        auto v = clear;
        packet(v, NOP_REPORT_COUNTER, nvn_p_counter{NVN_COUNTER_TYPE_TIMESTAMP_TOP, addr});
        const size_t enable_at = v.size();
        packet(v, NOP_RENDER_ENABLE, nvn_p_flag{value, 0});
        const size_t copy_at = v.size();
        packet(v, following, copy);
        assert(candidate(v, v.size()) == expected);
        assert(!candidate(v, enable_at + sizeof(nvn_op_hdr)));
        assert(!candidate(v, copy_at));
        assert(!candidate(v, v.size()-1));
    };
    through_enable(1, NOP_COPY_T2T, 64, true);
    through_enable(0, NOP_COPY_T2T, 64, false);
    through_enable(1, NOP_BARRIER, 64, false);
    through_enable(1, NOP_COPY_T2T, 4096+512, false);
    dst.gpu = 4096+128;
    test(NVN_COUNTER_TYPE_TIMESTAMP, 64, NOP_COPY_T2T, false);
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture.exe"
        cpp.write_text(fixture)
        subprocess.run([args.cxx, "-std=c++20", "-I", str(ROOT / "gpu/render"), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
