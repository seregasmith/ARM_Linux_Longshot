// (longshot, RX_VK_LOG_PIPELINE_RAM) What a shader asks of each invocation's private memory, read from its SPIR-V: the Private
// and Function variables' sizes, its Workgroup (shared) variables' and its workgroup size. On the AYN Odin 3, Turnip's private
// memory is one buffer for the whole device, sized for the hungriest shader made so far times every invocation the GPU can hold
// at once (rounded up to a power of two a fiber), grown and never shrunk: one compute pipeline of the title screen, a 4.2 MB
// SPIR-V, took 1.5 GiB of the driver's memory, and a near twin made after it 2.5 MB. The game's local memory (a compute
// program's control section, a stage's header: up to 64 KB a thread) is a Private array (Eden's DefineLocalMemory); the
// driver's register spills come on top and are not in the SPIR-V. Sizes are the types' own, packed, without the driver's
// alignment. No file reading, no Vulkan: tests/test_pipeline_ram.py reads made-up modules
#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace spirv_shape {

struct Shape {
    bool ok = false;               // (a SPIR-V module, read to its end)
    uint64_t private_bytes = 0;    // (Private and Function variables: an invocation's own)
    uint64_t shared_bytes = 0;     // (Workgroup variables: a workgroup's)
    uint32_t function_vars = 0;    // (Function variables counted in private_bytes)
    uint32_t local[3] = {0, 0, 0}; // (LocalSize or LocalSizeId; 0: not given)
};

inline Shape read(const uint32_t* w, size_t words) {
    Shape s;
    if (!w || words < 5 || w[0] != 0x07230203u) return s;
    enum : uint32_t {
        OpExecutionMode = 16, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
        OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpSpecConstant = 50, OpVariable = 59,
        OpExecutionModeId = 331,
    };
    enum : uint32_t { LocalSize = 17, LocalSizeId = 38, Workgroup = 4, Private = 6, Function = 7 };
    std::unordered_map<uint32_t, uint64_t> size;       // (a type's bytes)
    std::unordered_map<uint32_t, uint64_t> konst;      // (an integer constant's value)
    std::unordered_map<uint32_t, uint32_t> pointee;    // (a pointer type: the type it points to)
    auto bytes = [&](uint32_t id) -> uint64_t {
        const auto it = size.find(id);
        return it == size.end() ? 0 : it->second;
    };
    uint32_t local_ids[3] = {0, 0, 0};
    size_t at = 5;
    while (at < words) {
        const uint32_t op = w[at] & 0xFFFFu, n = w[at] >> 16;
        if (n == 0 || at + n > words) return s;   // (cut short, or not SPIR-V: ok stays false)
        const uint32_t* a = w + at + 1;   // (the operands: n - 1 words)
        switch (op) {
        case OpExecutionMode:
            if (n >= 6 && a[1] == LocalSize) s.local[0] = a[2], s.local[1] = a[3], s.local[2] = a[4];
            break;
        case OpExecutionModeId:
            if (n >= 6 && a[1] == LocalSizeId) local_ids[0] = a[2], local_ids[1] = a[3], local_ids[2] = a[4];
            break;
        case OpTypeBool:
            if (n >= 2) size[a[0]] = 4;
            break;
        case OpTypeInt:
        case OpTypeFloat:
            if (n >= 3) size[a[0]] = a[1] / 8;
            break;
        case OpTypeVector:
        case OpTypeMatrix:
            if (n >= 4) size[a[0]] = bytes(a[1]) * a[2];
            break;
        case OpTypeArray:
            if (n >= 4) {
                const auto len = konst.find(a[2]);
                size[a[0]] = bytes(a[1]) * (len == konst.end() ? 0 : len->second);
            }
            break;
        case OpTypeStruct:
            if (n >= 2) {
                uint64_t total = 0;
                for (uint32_t i = 1; i < n - 1; i++) total += bytes(a[i]);
                size[a[0]] = total;
            }
            break;
        case OpTypePointer:
            if (n >= 4) size[a[0]] = 8, pointee[a[0]] = a[2];
            break;
        case OpConstant:
        case OpSpecConstant:
            if (n >= 4) konst[a[1]] = a[2];   // (the low word: lengths and sizes fit)
            break;
        case OpVariable:
            if (n >= 4) {
                const auto p = pointee.find(a[0]);
                const uint64_t b = p == pointee.end() ? 0 : bytes(p->second);
                if (a[2] == Private || a[2] == Function) {
                    s.private_bytes += b;
                    s.function_vars += a[2] == Function;
                } else if (a[2] == Workgroup) {
                    s.shared_bytes += b;
                }
            }
            break;
        default:
            break;
        }
        at += n;
    }
    for (int i = 0; i < 3; i++)
        if (local_ids[i]) {
            const auto v = konst.find(local_ids[i]);
            s.local[i] = v == konst.end() ? 0 : (uint32_t)v->second;
        }
    s.ok = true;
    return s;
}

}  // namespace spirv_shape
