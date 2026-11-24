// The FSR 4 model classes inside AMD's Vulkan upscaler: what a scan finds (its providers, their models and the model AMD
// picks for this GPU) and the selection the hook answers with. The scan and the hook are fsr4_models.cpp's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mkwvk {
namespace fsr4_models {

/// Dispose of a handle returned by an unsuccessful creation before another attempt.
template <typename Handle, typename Result, typename Create, typename Destroy>
Result create_attempt(Handle& handle, Result ok, Create create, Destroy destroy) {
    handle = nullptr;
    const Result result = create(&handle);
    if (result != ok && handle) {
        destroy(&handle);
        handle = nullptr;
    }
    return result;
}

enum Model : int {
    MODEL_FP8 = 0,
    MODEL_INT8,
    MODEL_INT8_NAVI2,
    MODEL_LITE,
    MODEL_UNKNOWN,
    MODEL_COUNT
};

/// What the hook answers: Auto delegates to AMD, Lite answers true only for the Lite provider.
enum Selection : int { SELECT_AUTO = 0, SELECT_LITE = 1 };

/// "FP8", "INT8", "INT8 Navi 2", "INT8 Lite", "unknown"
const char* model_name(Model model);
/// The MSVC type descriptor's name for a known model; "unknown" otherwise
const char* model_class_name(Model model);

struct Provider {
    Model model = MODEL_UNKNOWN;
    void** vtable = nullptr;
    bool (*is_supported)(void*, void*) = nullptr;
    void* instance = nullptr;   // (the provider object the registry points at, when found)
    const char* class_name = nullptr;
    uintptr_t type_descriptor = 0;
    uintptr_t col = 0;
};

struct ScanResult {
    std::vector<Provider> providers;
    bool shape_ok = false;        // (the 4.1.2 shape: entries 0, 1, 3..7 shared, entry 2 distinct)
    bool lite_present = false;
    bool registry_found = false;  // (the provider objects, so their own IsSupported can be asked)
    const char* reason = "";
};

/// Scans a PE image already mapped at image_base, image_size bytes (Windows: a loaded module; tests: a synthetic image).
ScanResult scan_image(const void* image_base, size_t image_size);

struct ModuleScan {
    std::string name;
    uintptr_t base = 0;
    size_t size = 0;
    ScanResult result;
};

/// Live module scanning stays disabled: a module snapshot neither pins its images nor says which one the loader uses.
/// The hook goes through load_known instead.
std::vector<ModuleScan> scan_loaded_modules();

/// An upscaler library whose private layout (the provider classes, their vtables, IsSupported at entry 2) was read and
/// run: named by its file's SHA-256, the same one scripts/fetch.py pins for the file a build fetches.
struct KnownLibrary {
    const char* sha256;   // (lower-case hex)
    const char* what;
};
/// The known library with this SHA-256 (64 hex digits, either case), or nullptr: any other file is left alone.
const KnownLibrary* known_library(const char* sha256_hex);

/// SHA-256 as 64 lower-case hex digits: of bytes, and of a file ("" when it cannot be read).
std::string sha256_hex(const void* data, size_t size);
std::string sha256_file(const std::string& path);

struct KnownModule {
    ModuleScan scan;                       // (base 0 when nothing was loaded)
    const KnownLibrary* library = nullptr; // (set only when the file on disk is a known library and it is what was loaded)
    std::string note;                      // (what the log says of it)
};
/// (Windows) dir's amd_fidelityfx_upscaler_vk.dll, when its SHA-256 is a known library's: loaded from that path before
/// AMD's loader asks for it by name, pinned for the process's life, and scanned. A file with another SHA-256 is not
/// loaded here and not scanned. Elsewhere: nothing (no Lite provider exists there).
KnownModule load_known(const std::string& dir);
/// (Windows) Whether the module AMD's loader uses under that name is the one load_known loaded; false turns the hook's
/// answers back into AMD's own (release) so a second copy of the library is never answered for.
bool known_module_is_loaders(const KnownModule& known);

/// -1: unset or not one of auto|lite/0|1; SELECT_AUTO/SELECT_LITE otherwise
int parse_model_env(const char* value);

/// Points each provider's IsSupported (vtable entry 2) at the delegating answer, in this process's copy only. Refused, with
/// no slot written, unless `library` is one of the known libraries (known_library: the caller hashed the file it loaded),
/// the scan has the 4.1.2 shape and every slot still holds what the scan read.
bool install(uintptr_t module_base, const ScanResult& result, const KnownLibrary* library);

/// The hooked answers are AMD's own again whatever is selected, and no model counts as hooked (the slots keep the
/// delegating function: nothing is written twice).
void release();

/// Whether at least one provider vtable has the hook.
bool hook_installed();

/// Whether the hooked providers include `model` (Lite in practice).
bool has_model(Model model);

/// The selection the hook answers with.
void set_model(Selection model);
Selection selected_model();

/// AMD's own pick, recorded by the probe or the first Auto answer; MODEL_UNKNOWN until known.
Model native_model();
void record_native_model(Model model);

/// Returns MODEL_UNKNOWN without calling inferred provider instances. Clears supported when given.
Model native_model(const ScanResult& result, void* device, bool* supported = nullptr);

}  // namespace fsr4_models
}  // namespace mkwvk
