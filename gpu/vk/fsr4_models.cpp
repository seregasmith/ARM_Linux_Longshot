// The FSR 4 model classes inside AMD's Vulkan upscaler (gpu/vk/fsr4_models.h): find the provider vtables by their MSVC
// RTTI type descriptors, check the 4.1.2 vtable shape, ask each provider's own IsSupported which model AMD picks for this
// GPU, and (Windows) install the delegating IsSupported that answers for Lite. The PE parser takes a mapped image and its
// size, so tests/test_fsr4_models.py can run it on a synthetic image without AMD's DLL or a GPU.
#include "fsr4_models.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef FSR4_MODELS_TEST
bool fsr4_test_protect(void* slot, bool writable, uintptr_t* protection);
#endif

namespace mkwvk {
namespace fsr4_models {

namespace {

constexpr uint16_t kDosMagic = 0x5A4D;
constexpr uint32_t kPeMagic = 0x00004550;
constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint16_t kPe32Plus = 0x20B;
constexpr uint32_t kScnCode = 0x00000020;
constexpr uint32_t kScnMemExecute = 0x20000000;
constexpr uint32_t kScnMemWrite = 0x80000000;
constexpr size_t kVtableEntries = 8;
constexpr size_t kColSize = 24;
constexpr size_t kNameLimit = 160;

struct Section {
    uintptr_t begin = 0;
    size_t size = 0;
    bool code = false;
    bool writable = false;
};

uint16_t read_u16(uintptr_t at) {
    uint16_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof v);
    return v;
}

uint32_t read_u32(uintptr_t at) {
    uint32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof v);
    return v;
}

uint64_t read_u64(uintptr_t at) {
    uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof v);
    return v;
}

struct Image {
    uintptr_t base = 0;
    size_t size = 0;
    std::vector<Section> sections;

    bool in_image(uintptr_t at, size_t len = 1) const {
        if (at < base) return false;
        const uintptr_t off = at - base;
        return off <= size && len <= size - off;
    }

    const Section* section_at(uintptr_t at) const {
        for (const Section& s : sections)
            if (at >= s.begin && at < s.begin + s.size) return &s;
        return nullptr;
    }

    bool in_code(uintptr_t at) const {
        const Section* s = section_at(at);
        return s && s->code;
    }

    bool in_non_code(uintptr_t at) const {
        const Section* s = section_at(at);
        return s && !s->code;
    }
};

struct TypeDesc {
    uint32_t rva = 0;
    Model model = MODEL_UNKNOWN;
    const char* name = nullptr;
};

struct Col {
    uint32_t rva = 0;
    size_t type = 0;
};

Model model_of(const char* name) {
    if (std::strcmp(name, ".?AVffxProvider_FSR4@@") == 0) return MODEL_FP8;
    if (std::strcmp(name, ".?AVffxProvider_FSR4_Int8@@") == 0) return MODEL_INT8;
    if (std::strcmp(name, ".?AVffxProvider_FSR4_Int8_Navi2@@") == 0) return MODEL_INT8_NAVI2;
    if (std::strcmp(name, ".?AVffxProvider_FSR4Lite_Int8@@") == 0) return MODEL_LITE;
    return MODEL_UNKNOWN;
}

bool is_common(const char* name) {
    return std::strcmp(name, ".?AVffxProvider_FSR4_Common@@") == 0;
}

/// The four provider classes' type descriptors, found by their mangled names in the non-code sections.
std::vector<TypeDesc> find_type_descriptors(const Image& im) {
    std::vector<TypeDesc> out;
    std::unordered_set<uint32_t> seen;
    const char prefix[] = ".?AVffxProvider_FSR4";
    const size_t prefix_len = sizeof prefix - 1;
    for (const Section& s : im.sections) {
        if (s.code) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(s.begin);
        size_t i = 0;
        while (i + prefix_len <= s.size) {
            // (jump to the next '.', not every byte: the scan is over every non-code section of every loaded module)
            const void* dot = std::memchr(p + i, '.', s.size - i - prefix_len + 1);
            if (!dot) break;
            i = (size_t)(static_cast<const uint8_t*>(dot) - p);
            if (p[i + 1] != '?' || std::memcmp(p + i, prefix, prefix_len) != 0) {
                i++;
                continue;
            }
            size_t n = 0;
            while (i + n < s.size && p[i + n] != 0 && n < kNameLimit) n++;
            if (i + n >= s.size || p[i + n] != 0) {
                i++;
                continue;
            }
            const char* name = reinterpret_cast<const char*>(p + i);
            if (is_common(name) || i < 16) {
                i += n;
                continue;
            }
            const uintptr_t td = s.begin + i - 16;
            if (!im.in_non_code(td) || !im.in_image(td, 16)) {
                i += n;
                continue;
            }
            const uint64_t p_vftable = read_u64(td);
            const uint64_t spare = read_u64(td + 8);
            if (spare != 0 || p_vftable == 0 || !im.in_image(p_vftable)) {
                i += n;
                continue;
            }
            const uint64_t rva = td - im.base;
            if (rva > 0xFFFFFFFFull) {
                i += n;
                continue;
            }
            if (!seen.insert((uint32_t)rva).second) {
                i += n;
                continue;
            }
            out.push_back({(uint32_t)rva, model_of(name), name});
            i += n;   // (past this string; the next search starts at its end)
        }
    }
    return out;
}

/// The Complete Object Locators that point at those type descriptors.
std::vector<Col> find_cols(const Image& im, const std::vector<TypeDesc>& tds) {
    std::unordered_map<uint32_t, size_t> by_rva;
    for (size_t i = 0; i < tds.size(); i++) by_rva.emplace(tds[i].rva, i);
    std::vector<Col> out;
    std::unordered_set<uint32_t> seen;
    for (const Section& s : im.sections) {
        if (s.code) continue;
        for (size_t i = 0; i + kColSize <= s.size; i += 4) {
            const uintptr_t at = s.begin + i;
            if (read_u32(at) != 1) continue;                 // signature
            if (read_u32(at + 4) != 0) continue;             // offset
            const uint32_t ptd = read_u32(at + 12);          // pTypeDescriptor (RVA)
            const uint32_t pself = read_u32(at + 20);        // pSelf (RVA)
            const uint64_t rva64 = at - im.base;
            if (rva64 > 0xFFFFFFFFull) continue;
            const uint32_t rva = (uint32_t)rva64;
            if (pself != rva) continue;
            const auto it = by_rva.find(ptd);
            if (it == by_rva.end()) continue;
            if (!seen.insert(rva).second) continue;
            out.push_back({rva, it->second});
        }
    }
    return out;
}

/// The vtables whose [-1] slot points at each locator, checked to be 8 code entries.
std::vector<Provider> find_providers(const Image& im, const std::vector<TypeDesc>& tds, const std::vector<Col>& cols) {
    std::vector<Provider> out;
    std::unordered_set<uintptr_t> seen;
    for (const Col& c : cols) {
        const uint64_t col_va = im.base + c.rva;
        for (const Section& s : im.sections) {
            if (s.code) continue;
            for (size_t i = 0; i + 8 <= s.size; i += 8) {
                if (read_u64(s.begin + i) != col_va) continue;
                const uintptr_t vtable = s.begin + i + 8;
                const Section* vt_section = im.section_at(vtable);
                if (!vt_section || vt_section->code || vtable - vt_section->begin > vt_section->size ||
                    kVtableEntries * 8 > vt_section->size - (vtable - vt_section->begin)) continue;
                uint64_t entries[kVtableEntries] = {};
                bool ok = true;
                for (size_t e = 0; e < kVtableEntries; e++) {
                    entries[e] = read_u64(vtable + e * 8);
                    if (!entries[e] || !im.in_code((uintptr_t)entries[e])) ok = false;
                }
                if (!ok || !seen.insert(vtable).second) continue;
                Provider p;
                p.model = tds[c.type].model;
                p.vtable = reinterpret_cast<void**>(vtable);
                p.is_supported = reinterpret_cast<bool (*)(void*, void*)>(entries[2]);
                p.class_name = tds[c.type].name;
                p.type_descriptor = im.base + tds[c.type].rva;
                p.col = im.base + c.rva;
                out.push_back(p);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Provider& a, const Provider& b) {
        return a.model != b.model ? a.model < b.model : (uintptr_t)a.vtable < (uintptr_t)b.vtable;
    });
    return out;
}

/// The 4.1.2 shape: entries 0, 1 and 3..7 are the same across every provider, entry 2 is distinct.
bool check_shape(const std::vector<Provider>& providers, const char** reason) {
    if (providers.size() < 2) {
        *reason = "fewer than two FSR 4 providers";
        return false;
    }
    uint64_t ref[kVtableEntries] = {};
    for (size_t e = 0; e < kVtableEntries; e++) ref[e] = read_u64((uintptr_t)providers[0].vtable + e * 8);
    std::unordered_set<uint64_t> distinct;
    for (const Provider& p : providers) {
        for (size_t e = 0; e < kVtableEntries; e++) {
            const uint64_t v = read_u64((uintptr_t)p.vtable + e * 8);
            if (e == 2) {
                if (!distinct.insert(v).second) {
                    *reason = "provider vtable shape does not match";
                    return false;
                }
            } else if (v != ref[e]) {
                *reason = "provider vtable shape does not match";
                return false;
            }
        }
    }
    if (distinct.size() != providers.size()) {
        *reason = "provider vtable shape does not match";
        return false;
    }
    *reason = "";
    return true;
}

/// The static registry is an array of pointers to the provider objects, in model order. Finding it lets the probe ask
/// the providers' own IsSupported without a hook.
bool find_registry(const Image& im, std::vector<Provider>& providers) {
    std::unordered_map<uintptr_t, size_t> candidates;
    for (size_t pi = 0; pi < providers.size(); pi++) {
        const uintptr_t vtable = (uintptr_t)providers[pi].vtable;
        for (const Section& s : im.sections) {
            if (s.code || !s.writable) continue;
            for (size_t i = 0; i + 8 <= s.size; i += 8)
                if (read_u64(s.begin + i) == vtable) candidates.emplace(s.begin + i, pi);
        }
    }
    if (candidates.empty()) return false;

    std::vector<uintptr_t> best;
    for (const Section& s : im.sections) {
        if (s.code) continue;   // (the registry may be read-only; the provider objects are the writable candidates)
        for (size_t i = 0; i + 8 <= s.size; i += 8) {
            const auto first = candidates.find(read_u64(s.begin + i));
            if (first == candidates.end()) continue;
            std::vector<uintptr_t> run;
            int last_model = -1;
            size_t j = i;
            for (; j + 8 <= s.size; j += 8) {
                const uintptr_t at = s.begin + j;
                const auto it = candidates.find(read_u64(at));
                if (it == candidates.end()) break;
                const int model = (int)providers[it->second].model;
                if (model <= last_model) break;
                run.push_back(at);
                last_model = model;
            }
            if (run.size() > best.size()) best = std::move(run);
            if (run.size() >= 2) i += (run.size() - 1) * 8;
        }
    }
    if (best.size() < 2) return false;
    for (uintptr_t at : best) {
        const uintptr_t object = read_u64(at);
        const auto it = candidates.find(object);
        if (it != candidates.end()) providers[it->second].instance = reinterpret_cast<void*>(object);
    }
    return true;
}

constexpr size_t kMaxHooked = 8;

struct HookedProvider {
    void** vtable = nullptr;
    bool (*original)(void*, void*) = nullptr;
    Model model = MODEL_UNKNOWN;
};

HookedProvider g_hooked[kMaxHooked];
std::atomic<int> g_hooked_count{0};
std::atomic<bool> g_released{false};   // (release: AMD's own answers whatever is selected)
std::atomic<int> g_model{SELECT_AUTO};
std::atomic<int> g_native_model{MODEL_UNKNOWN};

/// The libraries whose layout was read and run. FSR SDK v2.4.0's signed Vulkan upscaler (FSR 4.1.2, file version
/// 4.1.2.4555): the SHA-256 scripts/fetch.py pins for Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll.
const KnownLibrary kKnown[] = {
    {"4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7", "FSR SDK 2.4 (FSR 4.1.2)"},
};

/// What every hooked provider's IsSupported is: AMD's own answer with Auto (and after release), yes for the Lite provider
/// alone with Lite. AMD's loader walks the providers in model order and takes the first yes, so the first yes of an Auto
/// walk is the model AMD picks for this GPU.
bool delegating_is_supported(void* self, void* device) {
    void** vtable = self ? *static_cast<void***>(self) : nullptr;
    const int n = g_hooked_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) {
        const HookedProvider& h = g_hooked[i];
        if (h.vtable != vtable) continue;
        if (g_model.load(std::memory_order_relaxed) == SELECT_LITE && !g_released.load(std::memory_order_relaxed))
            return h.model == MODEL_LITE;
        const bool yes = h.original(self, device);
        if (yes) record_native_model(h.model);
        return yes;
    }
    return false;
}

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64] = {};
    size_t fill = 0;
    uint64_t bytes = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        std::memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; i++) {
            const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
            const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6], v[6] = v[5], v[5] = v[4], v[4] = v[3] + t1, v[3] = v[2], v[2] = v[1], v[1] = v[0], v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += v[i];
    }

    void add(const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bytes += size;
        while (size) {
            const size_t take = std::min(size, sizeof block - fill);
            std::memcpy(block + fill, p, take);
            fill += take, p += take, size -= take;
            if (fill == sizeof block) compress(block), fill = 0;
        }
    }

    std::string hex() {
        const uint64_t bits = bytes * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (fill != 56) add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
        add(len, 8);
        static const char* const digits = "0123456789abcdef";
        std::string out;
        for (uint32_t word : h)
            for (int shift = 28; shift >= 0; shift -= 4) out += digits[(word >> shift) & 15];
        return out;
    }
};

}  // namespace

const char* model_name(Model model) {
    switch (model) {
        case MODEL_FP8: return "FP8";
        case MODEL_INT8: return "INT8";
        case MODEL_INT8_NAVI2: return "INT8 Navi 2";
        case MODEL_LITE: return "INT8 Lite";
        default: return "unknown";
    }
}

const char* model_class_name(Model model) {
    switch (model) {
        case MODEL_FP8: return ".?AVffxProvider_FSR4@@";
        case MODEL_INT8: return ".?AVffxProvider_FSR4_Int8@@";
        case MODEL_INT8_NAVI2: return ".?AVffxProvider_FSR4_Int8_Navi2@@";
        case MODEL_LITE: return ".?AVffxProvider_FSR4Lite_Int8@@";
        default: return "unknown";
    }
}

ScanResult scan_image(const void* image_base, size_t image_size) {
    ScanResult result;
    if (!image_base || image_size < 0x40) {
        result.reason = "not a PE image";
        return result;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(image_base);
    if (read_u16(base) != kDosMagic) {
        result.reason = "not a PE image";
        return result;
    }
    const uint32_t pe = read_u32(base + 0x3C);
    if ((uint64_t)pe + 26 > image_size || read_u32(base + pe) != kPeMagic) {
        result.reason = "not a PE image";
        return result;
    }
    if (read_u16(base + pe + 4) != kMachineAmd64 || read_u16(base + pe + 24) != kPe32Plus) {
        result.reason = "not an x64 PE image";
        return result;
    }
    const uint16_t count = read_u16(base + pe + 6);
    const uint16_t optional = read_u16(base + pe + 20);
    const uint64_t table = (uint64_t)pe + 24 + optional;
    if (table + (uint64_t)count * 40 > image_size) {
        result.reason = "PE section table outside the image";
        return result;
    }

    Image im;
    im.base = base;
    im.size = image_size;
    for (uint16_t i = 0; i < count; i++) {
        const uintptr_t sh = base + (uintptr_t)table + i * 40;
        const uint32_t vsize = read_u32(sh + 8);
        const uint32_t va = read_u32(sh + 12);
        const uint32_t raw = read_u32(sh + 16);
        const uint32_t flags = read_u32(sh + 36);
        const size_t size = vsize ? vsize : raw;
        if (va >= image_size) continue;
        Section s;
        s.begin = base + va;
        s.size = std::min<size_t>(size, image_size - va);
        s.code = (flags & (kScnMemExecute | kScnCode)) != 0;
        s.writable = (flags & kScnMemWrite) != 0;
        im.sections.push_back(s);
    }
    if (im.sections.empty()) {
        result.reason = "PE image has no sections";
        return result;
    }

    const std::vector<TypeDesc> tds = find_type_descriptors(im);
    if (tds.empty()) {
        result.reason = "no FSR 4 provider classes";
        return result;
    }
    const std::vector<Col> cols = find_cols(im, tds);
    result.providers = find_providers(im, tds, cols);
    if (result.providers.empty()) {
        result.reason = "no FSR 4 provider vtables";
        return result;
    }
    result.shape_ok = check_shape(result.providers, &result.reason);
    for (const Provider& p : result.providers)
        if (p.model == MODEL_LITE) result.lite_present = true;
    result.registry_found = find_registry(im, result.providers);
    return result;
}

std::vector<ModuleScan> scan_loaded_modules() {
    std::vector<ModuleScan> out;
    // A module snapshot neither pins its images nor identifies the loader's provider. Until that
    // relationship and the private ABI are verified, do not inspect live modules.
    return out;
}

Model native_model(const ScanResult& result, void* device, bool* supported) {
    (void)result;
    (void)device;
    if (supported)
        for (int i = 0; i < MODEL_COUNT; i++) supported[i] = false;
    return MODEL_UNKNOWN;
}

int parse_model_env(const char* value) {
    if (!value || !*value) return -1;
    if (std::strcmp(value, "auto") == 0 || std::strcmp(value, "0") == 0) return SELECT_AUTO;
    if (std::strcmp(value, "lite") == 0 || std::strcmp(value, "1") == 0) return SELECT_LITE;
    return -1;
}

const KnownLibrary* known_library(const char* sha256_hex) {
    if (!sha256_hex || std::strlen(sha256_hex) != 64) return nullptr;
    for (const KnownLibrary& k : kKnown) {
        bool same = true;
        for (int i = 0; i < 64 && same; i++) {
            const char c = sha256_hex[i];
            same = (c >= 'A' && c <= 'F' ? (char)(c - 'A' + 'a') : c) == k.sha256[i];
        }
        if (same) return &k;
    }
    return nullptr;
}

std::string sha256_hex(const void* data, size_t size) {
    Sha256 s;
    s.add(data, size);
    return s.hex();
}

std::string sha256_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    Sha256 s;
    std::vector<uint8_t> buf(1 << 16);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) s.add(buf.data(), n);
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    return failed ? "" : s.hex();
}

KnownModule load_known(const std::string& dir) {
    KnownModule out;
#ifdef _WIN32
    const char* const file = "amd_fidelityfx_upscaler_vk.dll";
    const std::string path = dir + "\\" + file;
    out.scan.name = file;
    const std::string sha = sha256_file(path);
    if (sha.empty()) {
        out.note = std::string("no ") + file + " beside the loader: AMD's own choice of model";
        return out;
    }
    const KnownLibrary* library = known_library(sha.c_str());
    if (!library) {
        out.note = std::string(file) + " is not a library whose layout is known (SHA-256 " + sha.substr(0, 16) +
                   "...): not scanned, AMD's own choice of model";
        return out;
    }
    // (a copy already loaded under this name is whatever the process loaded first, not necessarily this file: left alone)
    if (GetModuleHandleA(file)) {
        out.note = std::string(file) + " was loaded before its file was checked: AMD's own choice of model";
        return out;
    }
    HMODULE m = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        out.note = std::string(file) + " did not load (Windows error " + std::to_string((unsigned long)GetLastError()) +
                   "): AMD's own choice of model";
        return out;
    }
    // (the module loaded is the file hashed: its own path, and that file's SHA-256 again now that it is mapped)
    char loaded[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(m, loaded, sizeof loaded);
    HMODULE pinned = nullptr;
    if (!len || len >= sizeof loaded || sha256_file(loaded) != sha ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, loaded, &pinned) || pinned != m) {
        FreeLibrary(m);
        out.note = std::string(file) + " could not be tied to the file that was checked: AMD's own choice of model";
        return out;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(m);
    const uint32_t pe = read_u32(base + 0x3C);
    out.scan.base = base;
    out.scan.size = read_u32(base + pe + 24 + 56);   // (IMAGE_OPTIONAL_HEADER64::SizeOfImage)
    out.scan.result = scan_image(m, out.scan.size);
    out.library = library;
    out.note = std::string(file) + " is " + library->what + "'s (SHA-256 checked), loaded from the loader's folder and pinned";
#else
    (void)dir;
#endif
    return out;
}

bool known_module_is_loaders(const KnownModule& known) {
#ifdef _WIN32
    if (known.library && known.scan.base &&
        reinterpret_cast<uintptr_t>(GetModuleHandleA(known.scan.name.c_str())) == known.scan.base)
        return true;
#else
    (void)known;
#endif
    release();
    return false;
}

bool install(uintptr_t module_base, const ScanResult& result, const KnownLibrary* library) {
    // (the shared vtable shape is not a fingerprint of the private ABI: the file's SHA-256 is, and only the caller that
    // hashed the file it loaded can name the library)
    bool known = false;
    for (const KnownLibrary& k : kKnown) known = known || library == &k;
    if (!known || !module_base || !result.shape_ok || result.providers.empty()) return false;
    if (result.providers.size() > kMaxHooked || g_hooked_count.load(std::memory_order_acquire) > 0) return false;
    for (const Provider& p : result.providers)
        if (!p.vtable || !p.is_supported || p.model == MODEL_UNKNOWN ||
            reinterpret_cast<bool (*)(void*, void*)>(p.vtable[2]) != p.is_supported)
            return false;   // (a vtable that changed under the scan: nothing written)
    // Slots may share a page: restore in reverse order to recover its original protection.
    uintptr_t before[kMaxHooked] = {};
    auto protect = [](void* slot, bool writable, uintptr_t* saved) {
#ifdef FSR4_MODELS_TEST
        return fsr4_test_protect(slot, writable, saved);
#elif defined(_WIN32)
        DWORD old = 0;
        const bool ok = VirtualProtect(slot, sizeof(void*), writable ? PAGE_READWRITE : (DWORD)*saved, &old) != 0;
        if (writable && ok) *saved = old;
        return ok;
#else
        (void)slot; (void)writable; (void)saved;
        return true;
#endif
    };
    size_t acquired = 0;
    for (; acquired < result.providers.size(); acquired++)
        if (!protect(&result.providers[acquired].vtable[2], true, &before[acquired])) break;
    if (acquired != result.providers.size()) {
        while (acquired > 0) {
            --acquired;
            protect(&result.providers[acquired].vtable[2], false, &before[acquired]);
        }
        return false;
    }
    for (size_t i = 0; i < result.providers.size(); i++) {
        const Provider& p = result.providers[i];
        g_hooked[i].vtable = p.vtable, g_hooked[i].original = p.is_supported, g_hooked[i].model = p.model;
    }
    g_hooked_count.store((int)result.providers.size(), std::memory_order_release);
    for (const Provider& p : result.providers)
        p.vtable[2] = reinterpret_cast<void*>(&delegating_is_supported);
    bool restored = true;
    while (acquired > 0) {
        --acquired;
        if (!protect(&result.providers[acquired].vtable[2], false, &before[acquired])) restored = false;
    }
    if (!restored) release();   // (the hooks remain valid, but only delegate to AMD)
    return restored;
}

void release() {
    g_released.store(true, std::memory_order_relaxed);
}

bool hook_installed() {
    return !g_released.load(std::memory_order_relaxed) && g_hooked_count.load(std::memory_order_acquire) > 0;
}

bool has_model(Model model) {
    if (g_released.load(std::memory_order_relaxed)) return false;
    const int n = g_hooked_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_hooked[i].model == model) return true;
    return false;
}

void set_model(Selection model) {
    g_model.store((int)model, std::memory_order_relaxed);
}

Selection selected_model() {
    return (Selection)g_model.load(std::memory_order_relaxed);
}

Model native_model() {
    return (Model)g_native_model.load(std::memory_order_relaxed);
}

void record_native_model(Model model) {
    int expected = MODEL_UNKNOWN;
    g_native_model.compare_exchange_strong(expected, (int)model, std::memory_order_relaxed);
}

}  // namespace fsr4_models
}  // namespace mkwvk
