// Synthetic PE images for gpu/vk/fsr4_models.h's scanner: the four FSR 4 provider classes, their Complete Object
// Locators, vtables and static registry, laid out as AMD's 4.1.2 Vulkan DLL has them. No AMD DLL, no GPU, no game.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "fsr4_models.h"

using namespace mkwvk::fsr4_models;

#ifdef FSR4_MODELS_TEST
static int fail_protect = -1, protect_calls = 0;
static std::vector<void*> protected_slots;
bool fsr4_test_protect(void* slot, bool writable, uintptr_t* protection) {
    if (writable) {
        if (protect_calls++ == fail_protect) return false;
        protected_slots.push_back(slot);
        *protection = 1;
    } else {
        if (protected_slots.empty() || protected_slots.back() != slot || *protection != 1) std::abort();
        protected_slots.pop_back();
    }
    return true;
}
#endif

namespace {

constexpr uint32_t kPe = 0x80;
constexpr uint32_t kOpt = kPe + 24;
constexpr uint32_t kSections = kOpt + 0xF0;
constexpr uint32_t kText = 0x1000;
constexpr uint32_t kRdata = 0x2000;
constexpr uint32_t kData = 0x3000;
constexpr uint32_t kSize = 0x4000;
constexpr uint32_t kCodeFlags = 0x60000020;    // CODE | EXECUTE | READ
constexpr uint32_t kReadFlags = 0x40000040;    // INITIALIZED_DATA | READ
constexpr uint32_t kWriteFlags = 0xC0000040;   // INITIALIZED_DATA | READ | WRITE

struct Image {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kSize);
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(bytes.data()); }
    uint64_t va(uint32_t rva) const { return base() + rva; }
    void u16(uint32_t at, uint16_t v) { std::memcpy(bytes.data() + at, &v, 2); }
    void u32(uint32_t at, uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); }
    void u64(uint32_t at, uint64_t v) { std::memcpy(bytes.data() + at, &v, 8); }
    void str(uint32_t at, const char* s) { std::memcpy(bytes.data() + at, s, std::strlen(s) + 1); }
    static uint32_t align(uint32_t at, uint32_t n) { return (at + n - 1) & ~(n - 1); }
};

struct ModelInfo {
    Model model;
    const char* name;
    const char* class_name;
};

const ModelInfo kModels[] = {
    {MODEL_FP8, "FP8", ".?AVffxProvider_FSR4@@"},
    {MODEL_INT8, "INT8", ".?AVffxProvider_FSR4_Int8@@"},
    {MODEL_INT8_NAVI2, "INT8 Navi 2", ".?AVffxProvider_FSR4_Int8_Navi2@@"},
    {MODEL_LITE, "INT8 Lite", ".?AVffxProvider_FSR4Lite_Int8@@"},
};

struct Fixture {
    bool models[4] = {true, true, true, true};
    bool unknown = false;
    bool bad_shared = false;
    bool is_supported_yes[4] = {false, false, true, false};   // (the RX 6600 case: only Navi 2 says yes)
};

void headers(Image& im) {
    im.u16(0, 0x5A4D);
    im.u32(0x3C, kPe);
    im.u32(kPe, 0x00004550);
    im.u16(kPe + 4, 0x8664);   // AMD64
    im.u16(kPe + 6, 3);
    im.u16(kPe + 20, 0xF0);    // SizeOfOptionalHeader (PE32+)
    im.u16(kOpt, 0x20B);
    im.u32(kOpt + 0x38, kSize);

    const uint32_t sec = kSections;
    im.str(sec, ".text");
    im.u32(sec + 8, 0x1000);
    im.u32(sec + 12, kText);
    im.u32(sec + 16, 0x1000);
    im.u32(sec + 36, kCodeFlags);

    im.str(sec + 40, ".rdata");
    im.u32(sec + 48, 0x1000);
    im.u32(sec + 52, kRdata);
    im.u32(sec + 56, 0x1000);
    im.u32(sec + 76, kReadFlags);

    im.str(sec + 80, ".data");
    im.u32(sec + 88, 0x1000);
    im.u32(sec + 92, kData);
    im.u32(sec + 96, 0x1000);
    im.u32(sec + 116, kWriteFlags);

    // A ret at every address a vtable names; the scanner only checks that they are in .text.
    for (uint32_t rva = kText + 0x100; rva < kText + 0x300; rva += 0x10) im.bytes[rva] = 0xC3;
}

struct Built {
    Image image;
    uint32_t vtable_rva[5] = {};
    uint32_t instance_rva[5] = {};
    int count = 0;
};

Built build(const Fixture& f) {
    Built b;
    Image& im = b.image;
    headers(im);

    // (the Windows patch test calls the providers' IsSupported: x64 code returning true/false)
    for (int i = 0; i < 4; i++) {
        const uint32_t at = kText + 0x200 + (uint32_t)i * 0x10;
        if (f.is_supported_yes[i]) {
            const uint8_t yes[] = {0xB8, 1, 0, 0, 0, 0xC3};   // mov eax, 1; ret
            std::memcpy(im.bytes.data() + at, yes, sizeof yes);
        } else {
            const uint8_t no[] = {0x31, 0xC0, 0xC3};          // xor eax, eax; ret
            std::memcpy(im.bytes.data() + at, no, sizeof no);
        }
    }

    uint32_t td_cursor = Image::align(kData + 0x100, 8);
    uint32_t col_cursor = Image::align(kRdata + 0x100, 8);
    uint32_t vt_cursor = Image::align(col_cursor + 5 * 24, 8);
    uint32_t obj_cursor = Image::align(td_cursor + 5 * 128, 8);
    uint32_t reg_cursor = Image::align(obj_cursor + 5 * 0x20, 8);

    const uint32_t shared[8] = {kText + 0x100, kText + 0x110, 0, kText + 0x130,
                                kText + 0x140, kText + 0x150, kText + 0x160, kText + 0x170};

    for (int i = 0; i < 4; i++) {
        if (!f.models[i]) continue;
        const int n = b.count;

        const uint32_t td_rva = td_cursor;
        im.u64(td_rva, im.va(kRdata));
        im.u64(td_rva + 8, 0);
        im.str(td_rva + 16, kModels[i].class_name);
        td_cursor = Image::align(td_rva + 16 + (uint32_t)std::strlen(kModels[i].class_name) + 1, 8);

        const uint32_t col_rva = col_cursor;
        im.u32(col_rva, 1);
        im.u32(col_rva + 4, 0);
        im.u32(col_rva + 8, 0);
        im.u32(col_rva + 12, td_rva);
        im.u32(col_rva + 16, 0);
        im.u32(col_rva + 20, col_rva);
        col_cursor += 24;

        const uint32_t slot_rva = vt_cursor;
        const uint32_t vtable_rva = slot_rva + 8;
        im.u64(slot_rva, im.va(col_rva));
        for (int e = 0; e < 8; e++) {
            uint64_t addr = 0;
            if (e == 2) addr = im.va(kText + 0x200 + (uint32_t)n * 0x10);
            else if (e == 1 && f.bad_shared && n == 1) addr = im.va(kText + 0x120);
            else addr = im.va(shared[e]);
            im.u64(vtable_rva + e * 8, addr);
        }
        vt_cursor = vtable_rva + 8 * 8;
        b.vtable_rva[n] = vtable_rva;

        const uint32_t obj_rva = obj_cursor;
        im.u64(obj_rva, im.va(vtable_rva));
        b.instance_rva[n] = obj_rva;
        obj_cursor += 0x20;
        b.count++;
    }

    if (f.unknown) {
        const int n = b.count;
        const char* name = ".?AVffxProvider_FSR4_Future@@";

        const uint32_t td_rva = td_cursor;
        im.u64(td_rva, im.va(kRdata));
        im.u64(td_rva + 8, 0);
        im.str(td_rva + 16, name);
        td_cursor = Image::align(td_rva + 16 + (uint32_t)std::strlen(name) + 1, 8);

        const uint32_t col_rva = col_cursor;
        im.u32(col_rva, 1);
        im.u32(col_rva + 4, 0);
        im.u32(col_rva + 8, 0);
        im.u32(col_rva + 12, td_rva);
        im.u32(col_rva + 16, 0);
        im.u32(col_rva + 20, col_rva);
        col_cursor += 24;

        const uint32_t slot_rva = vt_cursor;
        const uint32_t vtable_rva = slot_rva + 8;
        im.u64(slot_rva, im.va(col_rva));
        for (int e = 0; e < 8; e++) {
            uint64_t addr = 0;
            if (e == 2) addr = im.va(kText + 0x200 + (uint32_t)n * 0x10);
            else addr = im.va(shared[e]);
            im.u64(vtable_rva + e * 8, addr);
        }
        vt_cursor = vtable_rva + 8 * 8;
        b.vtable_rva[n] = vtable_rva;

        const uint32_t obj_rva = obj_cursor;
        im.u64(obj_rva, im.va(vtable_rva));
        b.instance_rva[n] = obj_rva;
        obj_cursor += 0x20;
        b.count++;
    }

    for (int i = 0; i < b.count; i++) im.u64(reg_cursor + i * 8, im.va(b.instance_rva[i]));
    return b;
}

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

void test_four() {
    std::printf("case: four providers\n");
    const Built b = build(Fixture{});
    const ScanResult r = scan_image(b.image.bytes.data(), b.image.bytes.size());
    check(r.providers.size() == 4, "four providers found");
    check(r.shape_ok, "four providers have the 4.1.2 shape");
    check(r.lite_present, "Lite is present");
    check(r.registry_found, "provider registry found");
    if (r.providers.size() != 4) return;
    for (int i = 0; i < 4; i++) {
        check(r.providers[i].model == kModels[i].model, "provider order is FP8, INT8, Navi 2, Lite");
        check(std::strcmp(r.providers[i].class_name, kModels[i].class_name) == 0, "provider class name");
        check((uintptr_t)r.providers[i].vtable == b.image.va(b.vtable_rva[i]), "provider vtable address");
        check((uintptr_t)r.providers[i].is_supported == b.image.va(kText + 0x200 + (uint32_t)i * 0x10),
              "provider IsSupported address");
        check(r.providers[i].instance == (void*)b.image.va(b.instance_rva[i]), "provider instance from the registry");
    }
}

void test_no_lite() {
    std::printf("case: no Lite\n");
    Fixture f;
    f.models[3] = false;
    const Built b = build(f);
    const ScanResult r = scan_image(b.image.bytes.data(), b.image.bytes.size());
    check(r.providers.size() == 3, "three providers found");
    check(r.shape_ok, "three providers have the 4.1.2 shape");
    check(!r.lite_present, "Lite is not present");
    check(r.registry_found, "three-provider registry found");
}

void test_one_provider() {
    std::printf("case: one provider\n");
    Fixture f;
    f.models[0] = f.models[1] = f.models[2] = false;   // Lite only
    const Built b = build(f);
    const ScanResult r = scan_image(b.image.bytes.data(), b.image.bytes.size());
    check(r.providers.size() == 1, "one provider found");
    check(!r.shape_ok, "one provider is not a patchable shape");
    check(r.lite_present, "the one provider is Lite");
    check(!r.registry_found, "one provider is not a registry");
}

void test_bad_shape() {
    std::printf("case: shifted shared entry\n");
    Fixture f;
    f.bad_shared = true;
    const Built b = build(f);
    const ScanResult r = scan_image(b.image.bytes.data(), b.image.bytes.size());
    check(r.providers.size() == 4, "the providers are still listed");
    check(!r.shape_ok, "a shifted shared vtable entry is rejected");
}

void test_unknown() {
    std::printf("case: unknown provider\n");
    Fixture f;
    f.unknown = true;
    const Built b = build(f);
    const ScanResult r = scan_image(b.image.bytes.data(), b.image.bytes.size());
    check(r.providers.size() == 5, "the unknown provider is listed");
    check(r.shape_ok, "the unknown provider shares the 4.1.2 shape");
    check(r.providers.size() == 5 && r.providers[4].model == MODEL_UNKNOWN, "the unknown provider is last");
    check(r.providers.size() == 5 && std::strcmp(r.providers[4].class_name, ".?AVffxProvider_FSR4_Future@@") == 0,
          "the unknown class name is kept");
    check(r.registry_found, "the five-provider registry is found");
}

void test_not_pe() {
    std::printf("case: not a PE image\n");
    uint8_t junk[64] = {};
    const ScanResult r = scan_image(junk, sizeof junk);
    check(r.providers.empty(), "no providers in junk");
    check(r.reason && *r.reason, "the rejection says why");
}

bool yes(void*, void*) { return true; }
bool no(void*, void*) { return false; }

const char* const kSdk24 = "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7";

void test_known_library() {
    std::printf("case: the known library and SHA-256\n");
    check(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    check(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    const std::string two_blocks(1000, 'a');   // (crosses block boundaries and the padding's second block)
    check(sha256_hex(two_blocks.data(), two_blocks.size()) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "SHA-256 of 1000 a");
    check(sha256_file("no such file here").empty(), "a file that cannot be read has no SHA-256");
    check(known_library(kSdk24) != nullptr, "FSR SDK 2.4's upscaler library is known");
    std::string upper = kSdk24;
    for (char& c : upper) c = (char)std::toupper((unsigned char)c);
    check(known_library(upper.c_str()) == known_library(kSdk24), "the SHA-256 in upper case names the same library");
    std::string other = kSdk24;
    other[63] = other[63] == '0' ? '1' : '0';
    check(!known_library(other.c_str()), "another SHA-256 names no library");
    check(!known_library(nullptr) && !known_library("") && !known_library("4a7e895a"), "a short or missing SHA-256 names none");
    const KnownModule none = load_known("no such folder here");
    check(!none.library && !none.scan.base, "no library is loaded from a folder without the file");
}

/// The fixture's instance i, with its vtable's IsSupported a function of this program (the image's own code is not
/// executable memory here)
void set_answer(ScanResult& r, int i, bool (*answer)(void*, void*)) {
    r.providers[i].vtable[2] = reinterpret_cast<void*>(answer);
    r.providers[i].is_supported = answer;
}

bool ask(const Built& b, const ScanResult& r, int i) {
    void* instance = (void*)b.image.va(b.instance_rva[i]);
    return reinterpret_cast<bool (*)(void*, void*)>(r.providers[i].vtable[2])(instance, nullptr);
}

void test_patch() {
    std::printf("case: the hook, only for a known library\n");
    check(scan_loaded_modules().empty(), "live discovery stays disabled");
    const Built b = build(Fixture{});
    ScanResult r = scan_image(b.image.bytes.data(), b.image.bytes.size());
    check(r.providers.size() == 4 && r.shape_ok && r.lite_present, "patch fixture scans");
    for (int i = 0; i < 4; i++) set_answer(r, i, i == 2 ? yes : no);   // (the RX 6600 case: only Navi 2 says yes)
    const KnownLibrary* known = known_library(kSdk24);
    const KnownLibrary made_up{kSdk24, "a copy of the table's row"};

    check(!install(b.image.base(), r, nullptr), "the shared shape alone is not a known library");
    check(!install(b.image.base(), r, &made_up), "a library the caller made up is not a known one");
    check(!install(0, r, known), "no module, no hook");
    ScanResult bad = r;
    bad.shape_ok = false;
    check(!install(b.image.base(), bad, known), "a shape that is not understood is not hooked");
    ScanResult moved = r;
    moved.providers[1].is_supported = yes;   // (its slot holds `no`: the vtable changed under the scan)
    check(!install(b.image.base(), moved, known), "a slot that no longer holds what the scan read is not hooked");
    check(!hook_installed() && !has_model(MODEL_LITE), "every refusal leaves Lite unavailable");
    for (int i = 0; i < 4; i++)
        check(r.providers[i].vtable[2] == reinterpret_cast<void*>(i == 2 ? yes : no), "every refusal leaves every slot unchanged");

#ifdef FSR4_MODELS_TEST
    for (int failure = 0; failure < 4; failure++) {
        fail_protect = failure;
        protect_calls = 0;
        check(!install(b.image.base(), r, known), "a protection failure refuses the whole install");
        check(protected_slots.empty(), "all acquired protections restored in reverse order");
        check(!hook_installed() && !has_model(MODEL_LITE), "no model exposed after a protection failure");
        for (int i = 0; i < 4; i++)
            check(r.providers[i].vtable[2] == reinterpret_cast<void*>(i == 2 ? yes : no), "protection failure leaves every slot unchanged");
    }
    fail_protect = -1;
    protect_calls = 0;
#endif
    check(install(b.image.base(), r, known), "the known library's providers are hooked");
    check(hook_installed() && has_model(MODEL_LITE) && has_model(MODEL_FP8), "the hooked models are known");
    for (int i = 0; i < 4; i++) {
        check(r.providers[i].vtable[2] != reinterpret_cast<void*>(i == 2 ? yes : no), "IsSupported is the delegating answer");
        for (int e = 0; e < 8; e++)
            if (e != 2)
                check((uintptr_t)r.providers[i].vtable[e] != (uintptr_t)r.providers[i].vtable[2], "only entry 2 is written");
    }
    check(!install(b.image.base(), r, known), "a second install is refused");

    set_model(SELECT_AUTO);
    check(!ask(b, r, 0) && !ask(b, r, 1) && ask(b, r, 2) && !ask(b, r, 3), "Auto: each provider's own answer");
    check(native_model() == MODEL_INT8_NAVI2, "the first yes of an Auto walk is AMD's pick");
    set_model(SELECT_LITE);
    check(!ask(b, r, 0) && !ask(b, r, 1) && !ask(b, r, 2) && ask(b, r, 3), "Lite: yes for the Lite provider alone");
    release();
    check(!ask(b, r, 0) && !ask(b, r, 1) && ask(b, r, 2) && !ask(b, r, 3), "after release: AMD's own answers with Lite selected");
    check(!hook_installed() && !has_model(MODEL_LITE), "after release no model counts as hooked");
    check(!known_module_is_loaders(KnownModule{}), "a module that was never loaded is not the loader's");
    set_model(SELECT_AUTO);
}

void test_selection() {
    std::printf("case: selection and env\n");
    set_model(SELECT_LITE);
    check(selected_model() == SELECT_LITE, "set_model/selected_model Lite");
    set_model(SELECT_AUTO);
    check(selected_model() == SELECT_AUTO, "set_model/selected_model Auto");
    record_native_model(MODEL_LITE);
    check(native_model() == MODEL_INT8_NAVI2, "the first native model is kept (the hook's Auto walk recorded Navi 2)");
    check(parse_model_env(nullptr) == -1, "unset env");
    check(parse_model_env("") == -1, "empty env");
    check(parse_model_env("auto") == SELECT_AUTO, "auto");
    check(parse_model_env("lite") == SELECT_LITE, "lite");
    check(parse_model_env("0") == SELECT_AUTO, "0");
    check(parse_model_env("1") == SELECT_LITE, "1");
    check(parse_model_env("LITE") == -1, "invalid env");
}

void test_native_model() {
    std::printf("case: native model\n");
    ScanResult r;
    Provider p;
    p.model = MODEL_FP8;
    p.is_supported = no;
    p.instance = (void*)1;
    r.providers.push_back(p);
    p.model = MODEL_INT8;
    p.is_supported = yes;
    p.instance = (void*)1;
    r.providers.push_back(p);
    p.model = MODEL_INT8_NAVI2;
    p.is_supported = no;
    p.instance = (void*)1;
    r.providers.push_back(p);
    bool supported[MODEL_COUNT] = {};
    check(native_model(r, nullptr, supported) == MODEL_UNKNOWN, "unverified instances are not called");
    check(!supported[MODEL_INT8] && !supported[MODEL_FP8] && !supported[MODEL_INT8_NAVI2], "no private answers are inferred");
    p.model = MODEL_LITE;
    p.is_supported = yes;
    p.instance = nullptr;
    r.providers.push_back(p);
    check(native_model(r, nullptr) == MODEL_UNKNOWN, "unverified providers are not asked");
}

void test_failed_create() {
    void* handle = nullptr;
    int creates = 0, destroys = 0;
    const auto create = [&](void** out) {
        *out = (void*)1;
        return creates++ == 0 ? 1 : 0;
    };
    const auto destroy = [&](void** out) { destroys++; *out = nullptr; };
    check(create_attempt(handle, 0, create, destroy) == 1 && !handle && destroys == 1,
          "failed creation destroys its returned handle");
    check(create_attempt(handle, 0, create, destroy) == 0 && handle && destroys == 1,
          "retry retains the successful handle");
    destroy(&handle);
}

}  // namespace

int main() {
    test_four();
    test_no_lite();
    test_one_provider();
    test_bad_shape();
    test_unknown();
    test_not_pe();
    test_known_library();
    test_patch();
    test_selection();
    test_native_model();
    test_failed_create();
    check(std::strcmp(model_name(MODEL_LITE), "INT8 Lite") == 0, "model_name(Lite)");
    check(std::strcmp(model_class_name(MODEL_FP8), ".?AVffxProvider_FSR4@@") == 0, "model_class_name(FP8)");
    std::printf("fsr4_models_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
