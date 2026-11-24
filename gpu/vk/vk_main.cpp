// The Vulkan back end's start: vulkan-1.dll, the instance, the device (Vulkan 1.3 and the extensions the NVN replay
// wants where the GPU has them: mesh shaders, host memory import, graphics pipeline libraries), the window's
// swapchain, and the rx_* table (vk_api.h). The NVN replay itself is vk_replay.cpp.
#include "vk_paths.h"
#include "vk_loader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>   // (sysconf: the RAM, auto_profile)
#endif

#include "vk_api.h"
#include "vk_draw.h"
#include "vk_framegen.h"
#include "vk_internal.h"
#include "vk_new.h"
#include "cxx_escape.h"
#include "renderer_guard.h"
#include "gpu_choice.h"
#include "vk_menu.h"
#include "vk_bug_report.h"
#include "auto_settings.h"
#include "vk_dlss.h"
#include "vk_fsr4.h"
#include "vk_xess.h"
#include "vk_reflex.h"
#include "vk_antilag.h"
#include "vk_gpu_clock.h"
#include "vk_progress.h"
#include "vk_programs.h"
#include "vk_shader.h"
#include "mesh_mode.h"
#include "crash_markers.h"   // (mkwvk_close_asked: a close is not a crash)
#include "prepare_journal.h"   // (kExitEntryLimit: the journalled preparation's exit status for one entry past its limit)
#include "vk_fps.h"
#include "memory_priority.h"
#ifndef LONGSHOT_ENV_H   // (env_on: the switches' one rule)
#include "longshot_env.h"
#endif
#include "longshot_no_gpu.h"   // (why Vulkan listed no GPU, where the PC says: pick_device)
#include "switches.h"   // (the switches' table: what the run has set, said at the start)
#ifndef _WIN32
extern char** environ;
#endif

namespace vkx {
#define VKX_DEFINE(f) PFN_##f f;
VKX_GLOBAL(VKX_DEFINE)
VKX_INSTANCE(VKX_DEFINE)
VKX_DEVICE(VKX_DEFINE)
VKX_DEVICE2(VKX_DEFINE)
VKX_DEVICE3(VKX_DEFINE)
#undef VKX_DEFINE

static PFN_vkGetInstanceProcAddr get_instance_proc;

bool load_global() {
#ifdef _WIN32
    // Windows Vulkan shims can intercept the executable's imports but miss calls from this DLL.
    if (mkwvk::host && mkwvk::host->vulkan_loader_proc) {
        get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(mkwvk::host->vulkan_loader_proc());
    } else {
        // Older hosts leave System32 loader resolution to the renderer. The loader chooses the drivers in use.
        HMODULE m = LoadLibraryExA("vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return false;
        get_instance_proc = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(m, "vkGetInstanceProcAddr");
    }
#else
    HMODULE m = LoadLibraryA("vulkan-1.dll");   // (linux: libvulkan.so.1, host/posix/win32_posix.c)
    if (!m) return false;
    get_instance_proc = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(m, "vkGetInstanceProcAddr");
#endif
    if (!get_instance_proc) return false;
#define VKX_LOAD(f) f = (PFN_##f)get_instance_proc(nullptr, #f);
    VKX_GLOBAL(VKX_LOAD)
#undef VKX_LOAD
    return vkCreateInstance != nullptr;
}
PFN_vkGetInstanceProcAddr instance_proc_addr() { return get_instance_proc; }
void load_instance(VkInstance instance) {
#define VKX_LOAD(f) f = (PFN_##f)get_instance_proc(instance, #f);
    VKX_INSTANCE(VKX_LOAD)
#undef VKX_LOAD
}
void load_device(VkDevice device) {
#define VKX_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(device, #f);
    VKX_DEVICE(VKX_LOAD)
    VKX_DEVICE2(VKX_LOAD)
    VKX_DEVICE3(VKX_LOAD)
#undef VKX_LOAD
}
} // namespace vkx

namespace mkwvk {

bool predicted_complete();   // vk_draw.cpp: completion of explicit All preparation, including zero keys

int display_refresh_mhz();   // (below, outside the unnamed namespace)

std::vector<std::string> gpu_device_names;
std::vector<gpu_choice::Card> gpu_device_cards;
const MkwVkHost* host;
std::string options_path() {
    if (host && host->options_path) {
        const char* p = host->options_path();
        return p ? p : "";
    }
    return game_folder_path() + "longshot_options.ini";
}
int64_t prof_ns[PROF_COUNT];
Device dev;
PFN_vkGetInstanceProcAddr instance_proc() { return vkx::instance_proc_addr(); }
/// (longshot) The driver as it names itself (VkPhysicalDeviceDriverProperties: "NVIDIA 580.95.05", "radv Mesa 25.2.4"), else its
/// version number: the log's "vk: driver:" line and the menu's Save my pipeline list (vk_menu.cpp)
std::string driver_text() {
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    if (dev.physical) vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    std::string name(drv.driverName, strnlen(drv.driverName, sizeof drv.driverName)),
        info(drv.driverInfo, strnlen(drv.driverInfo, sizeof drv.driverInfo));
    if (name.empty() && info.empty()) {
        char b[32];
        snprintf(b, sizeof b, "%#x", dev.props.driverVersion);
        return b;
    }
    return info.empty() ? name : name.empty() ? info : name + " " + info;
}

std::string device_name_text() { return std::string(dev.props.deviceName, strnlen(dev.props.deviceName, sizeof dev.props.deviceName)); }

/// (longshot) Milliseconds on the steady clock: the log's last line and the preparation's watchdog
int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<int> native_zstd_default{0};
std::atomic<int64_t> last_log_ms{0};   // (when logf last wrote: the preparation's status line comes after a silence)
static void vlogf(LogLevel level, const char* fmt, std::va_list a) {
    static const char* const tags[] = {"[error] ", "[warn] ", "", "[debug] ", "[trace] "};
    char b[1024];
    const int n = std::snprintf(b, sizeof b, "%s", tags[level >= log_error && level <= log_trace ? level : log_info]);
    std::vsnprintf(b + n, sizeof b - n, fmt, a);
    last_log_ms.store(steady_ms(), std::memory_order_relaxed);
    if (host) host->log(b);
}
void logf(const char* fmt, ...) {
    std::va_list a;
    va_start(a, fmt);
    vlogf(log_info, fmt, a);
    va_end(a);
}
void logf_at(LogLevel level, const char* fmt, ...) {
    std::va_list a;
    va_start(a, fmt);
    vlogf(level, fmt, a);
    va_end(a);
}

// (longshot, issue 1280) The renderer's C++ allocations at Windows' commit limit (RAM plus the page file full). operator new gets
// nothing from malloc and throws std::bad_alloc; nothing catches it (often nothing can: the unwind itself fails on its way up,
// 0xc00000ff), and the game ends with exception 0x20474343, "An unexpected error", with no wait and no out-of-memory notice.
// The host's own allocations wait there (host/mkw_glue.c hos_alloc_failed); this is the same for the renderer's.
// new_refused is the renderer's new_handler (mkwvk_init): operator new calls it each time malloc refuses and tries again when it
// returns. It returns after 50 ms, for up to 5 s, while the textures give commit back; then the host's out-of-memory line and
// notice, and the game stops (MkwVkHost out_of_memory, version 28; an older host: thrown). A refusal more than
// kNewApartMs after this thread's last try is a new one, with a whole wait of its own. In a NewNothrow scope (vk_new.h) it
// throws at once: that nothrow new's caller has a fallback for its null.
// HOS_TEST_ALLOC_REFUSE=<text> (host/mkw_glue.c's test) whose text is in kNewWhat: at the 600th present one allocation too large
// for any PC is asked for, to see the wait, the line, the notice and the stop in a run (new_refuse_test)
static constexpr int kNewTries = 100;            // (100 x 50 ms: 5 s, as the host's hos_alloc_failed and commit_wait)
static constexpr int kNewTryMs = 50;
static constexpr int64_t kNewApartMs = 250;
static constexpr char kNewWhat[] = "the renderer's C++ allocations";
constinit thread_local int t_new_tries = 0;
constinit thread_local int64_t t_new_last_ms = 0;
static void new_sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void new_refused() {
    if (t_new_nothrow) throw std::bad_alloc();   // (libc++'s and libstdc++'s nothrow new: the caller's null)
    if (t_new_tries && steady_ms() - t_new_last_ms > kNewApartMs) t_new_tries = 0;
    if (!t_new_tries) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 8)
            logf("vk: a C++ allocation refused (at the commit limit?): tried again for up to %d s", kNewTries * kNewTryMs / 1000);
    }
    if (t_new_tries < kNewTries) {
        t_new_tries++;
        new_sleep_ms(kNewTryMs);
        t_new_last_ms = steady_ms();
        return;   // (operator new asks malloc again)
    }
    t_new_tries = 0;
    if (host && host->version >= 28 && host->out_of_memory) host->out_of_memory(kNewWhat);   // (does not return)
    throw std::bad_alloc();
}
void new_refuse_test(int64_t presents) {   // (the replay's present, its count)
    static const char* const e = getenv("HOS_TEST_ALLOC_REFUSE");
    if (!e || !*e || presents != 600 || !std::strstr(kNewWhat, e)) return;
    logf("vk: HOS_TEST_ALLOC_REFUSE: one C++ allocation of 1 PB, refused on any PC, to see what a player sees at the commit limit");
    static void* volatile kept;   // (kept: a new and delete of nothing else may be left out by the compiler)
    kept = ::operator new(size_t(1) << 50);
    ::operator delete(kept);
}

namespace {

const char* mesh_note = "not offered by the driver";   // (the device line's: mesh_rule::device_note, set by create_device)

bool has_extension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

bool create_instance() {
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        logf("vk: the Vulkan loader is %u.%u: 1.3 is needed", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version));
        return false;
    }
#ifdef _WIN32
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const uint32_t n_exts = 2;
#else
    // (linux: every window system's surface the loader has, for whichever one SDL's window is on: X11 or Wayland)
    const char* exts[4] = {VK_KHR_SURFACE_EXTENSION_NAME};
    uint32_t n_exts = 1;
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
        for (const char* want : {"VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface"})
            if (has_extension(all, want)) exts[n_exts++] = want;
    }
#endif
#ifndef _WIN32
    // (longshot) Mesa's Intel compiler (driconf shader_spilling_rate, read when the driver loads, ignored by the others): when a
    // program does not fit its registers it spills some and allocates the whole program again, rounds of one at first and then
    // of (spilled so far / rate). Held to 32 lanes an Iris Xe has 32 registers a lane, and MKW's large compute programs spill
    // thousands: at Mesa's default one is not built after 20 minutes and 13 GB, at 2 in 32 s and 1.7 GB (3 and 4 are much
    // slower). The programs that do not spill are built the same; the driver's estimate of those that do goes from 12% better
    // to 53% worse. A value the player set is kept
    setenv("shader_spilling_rate", "2", 0);
#endif
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mkw-recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    std::vector<const char*> ext_list(exts, exts + n_exts);
    {   // (longshot) what NVIDIA DLSS needs (vk_dlss.cpp; nothing in a build without it)
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
        dlss_instance_extensions(ext_list, all);
        xess_instance_extensions(ext_list, all, app.apiVersion);   // (longshot: Intel XeSS's, when its library is there: vk_xess.cpp)
        // (longshot) the HDR10 colour space for the Insert menu's HDR (the menu's settings are read after the instance is made, so
        // whenever the driver has it; the SDR choice in create_swapchain keeps to the sRGB surface)
        if (has_extension(all, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
            ext_list.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME), dev.swapchain_colorspace = true;
    }
    ci.enabledExtensionCount = (uint32_t)ext_list.size();
    ci.ppEnabledExtensionNames = ext_list.data();
    // RX_VK_VALIDATION=1: the Khronos validation layer (when installed)
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (env_on("RX_VK_VALIDATION")) ci.enabledLayerCount = 1, ci.ppEnabledLayerNames = &layer;
    if (vkCreateInstance(&ci, nullptr, &dev.instance) != VK_SUCCESS) {
        logf("vk: vkCreateInstance failed");
        return false;
    }
    load_instance(dev.instance);
    return true;
}

bool pick_device() {
    menu_init();
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(dev.instance, &n, all.data());
    if (!n) {   // (longshot) no device at all: no Vulkan driver lists a GPU. Why, where the PC says (NVIDIA's driver not loaded, a
                // real-time kernel): the window after the start reads this line (host/hos_crash_dialog.h)
        LongshotNoGpu why;
#ifdef _WIN32
        longshot_no_gpu_probe(nullptr, &why);
#else
        longshot_no_gpu_probe("", &why);
#endif
        char line[400];
        longshot_no_gpu_line(line, sizeof line, &why);
        logf("vk: %s", line);
    }

    std::vector<uint32_t> valid_gpus;
    gpu_device_names.clear();
    gpu_device_cards.clear();
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        logf("vk: device %u: %s, Vulkan %u.%u, driver %#x, type %d, id %04x:%04x", i, p.deviceName,
             VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion, (int)p.deviceType,
             p.vendorID, p.deviceID);
        if (p.apiVersion >= VK_API_VERSION_1_3) {
            valid_gpus.push_back(i);
            gpu_device_names.push_back(p.deviceName);
            gpu_device_cards.push_back({p.vendorID, p.deviceID});   // (the row's saved card: gpu_choice.h card_id)
        }
    }
    menu_set_gpu_count((int)valid_gpus.size());

    // (AMD laptops) With AMD graphics in the processor and an AMD card, Windows has two AMD drivers installed, one an
    // adapter, and each one's Vulkan driver lists every AMD GPU: the card shows up twice. It came first through the
    // processor graphics' driver, which the card's own update leaves as it was: on an RX 7700S that older driver
    // (amdvlk64.dll 32.0.11036.4002) crashed in its shader compiler (0xc0000409) on the game's compute pipelines while
    // the card's 32.0.32015.7029 built them. The same kind of GPU twice: the one with the newer driver wins.
    // RX_VK_DEVICE=<n> or opts.gpu_device: that device, as listed in the log
    int best = -1, best_score = -1;
    uint32_t best_driver = 0;
    int target_raw_idx = -1;
    if (const char* want = getenv("RX_VK_DEVICE")) {
        if (*want) target_raw_idx = atoi(want);
    } else if (const int k = gpu_choice::menu_device(opts.gpu_device, (int)valid_gpus.size()); k >= 0) {
        target_raw_idx = (int)valid_gpus[k];
    }

    for (uint32_t i : valid_gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        if (target_raw_idx >= 0) {
            if ((int)i == target_raw_idx) best = (int)i, best_score = 3;
            continue;
        }
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 :
                          p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > best_score || (score == best_score && p.driverVersion > best_driver))
            best = (int)i, best_score = score, best_driver = p.driverVersion;
    }
    if (best < 0 && target_raw_idx >= 0) {
        logf("vk: preferred device %d not available or lacks Vulkan 1.3, falling back to auto selection", target_raw_idx);
        for (uint32_t i : valid_gpus) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(all[i], &p);
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 :
                              p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score || (score == best_score && p.driverVersion > best_driver))
                best = (int)i, best_score = score, best_driver = p.driverVersion;
        }
    }
    if (best < 0) {
        logf("vk: no Vulkan 1.3 GPU");
        return false;
    }
    dev.physical = all[best];
    vkGetPhysicalDeviceProperties(dev.physical, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &dev.memory);
    return true;
}

/// (longshot) Whether the shaders' float adds to storage buffers are asked to be the GPU's own atomic (create_device):
/// RX_VK_NATIVE_FADD by the switches' rule (longshot_env.h env_value_off: 0, false, off, no or empty is off, any other
/// value on); unset, with NVIDIA's driver, with AMD's Windows driver on Radeon 860M/840M (0x1114) from 0x8001a0
/// (Adrenalin 26.10.1), and with Intel's Windows driver on the GPU families it was run on: the Xe-LPG
/// iGPUs of Meteor Lake and Arrow Lake (PCI device 0x7Dxx) from driver 101.9175 on (Intel's Windows driverVersion is
/// 101 << 14 | 9175); the Arc B-series cards, Battlemage (PCI device 0xE2xx), from 101.9154 on; the Xe3 iGPUs of
/// Panther Lake (PCI device 0xB0xx) from 101.9150 on; and the Xe2 iGPUs of Lunar Lake (PCI device 0x64xx) from 101.8991
/// on (an Arc 140V, 0x64A0). There the loop loses the device, or loses adds, in the sky's light
/// sums (499dbfd0): exposure far too high, a milky white haze. Intel's other GPUs on Windows (Iris Xe and UHD, Arc A-series), older drivers, AMD's other GPUs
/// and Mesa's Intel driver report the feature too but were not tried: they keep the loop unless the switch asks,
/// until runs on them say otherwise
static bool native_fadd_wanted(const char* env, VkDriverId driver, uint32_t device, uint32_t version, bool windows) {
    if (env) return !env_value_off(env);
    return policy_rule::native_fadd_listed(driver, device, version, windows);   // (device_policy.h: the one list of drivers and families)
}

bool create_device() {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, exts.data());
    std::vector<const char*> want = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    auto optional = [&](const char* name, bool& flag) {
        flag = has_extension(exts, name);
        if (flag) want.push_back(name);
    };
    // (the driver, asked once before anything is decided by it: the mesh mode and the float add below, and the facts)
    VkPhysicalDeviceDriverProperties fadd_driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    {
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &fadd_driver;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    }
    // (how task and mesh programs run, mesh_mode.h: RX_VK_MESH_SHADERS=native or compute, a launch switch; unset,
    // Automatic. RX_VK_FORCE_MESH_FALLBACK=1, a test, is Compute over it: mesh shaders not used where they are, so the no-mesh
    // fallback the GTX 10xx and RX 5000 cards run can be tried on any GPU. A mesh_shaders= line in longshot_options.ini
    // does not choose: it is only told of in the log)
    static const mesh_rule::Choice mesh_choice = [] {
        if (FILE* fl = fopen(options_path().c_str(), "r")) {   // (vk_paths.h)
            char line[80];
            int old = 0;
            while (fgets(line, sizeof line, fl))
                if (mesh_rule::old_ini_choice(line, &old)) {
                    logf("vk: mesh_shaders=%d in longshot_options.ini is no longer read (the Mesh shaders row left the menu): "
                         "set RX_VK_MESH_SHADERS=native or compute in longshot_env.txt to choose", old);
                    break;
                }
            fclose(fl);
        }
        return mesh_rule::choose(getenv("RX_VK_MESH_SHADERS"), getenv("RX_VK_FORCE_MESH_FALLBACK"));
    }();
    const int mesh_mode = mesh_choice.mode;
    // (longshot) RADV's native task/mesh path deadlocks the GPU: the gfx ring and RADV's gang compute ring
    // end up waiting on each other, and a mesh-only draw hangs with the compute queue empty (devcoredumps on an
    // RX 6800: no VM fault; waves stuck on gfx in the second). Task + mesh draws alone on the fallback still crash, so
    // only full Compute is stable there, and the reports span Navi 2x (RX 6800, RX 6600 XT) and the APUs (the Steam Deck
    // OLED's Van Gogh, RDNA2; the ROG Xbox Ally X, RDNA3.5). With RADV, Automatic picks Compute; Native stays a choice
    // (RX_VK_MESH_SHADERS=native). RDNA1 and older have no mesh shaders on RADV: for them a no-op. The log says the driver
    // offers them all the same (mesh_rule::device_note). (By the driver, device_policy.h mesh_mode: AMDVLK, which does
    // not show the deadlock, keeps native as its Automatic)
    // (the card's own, before the mode below: whether it has task and mesh shaders, for the Quality preset Auto (this PC), which
    // must not put a card that has them on Minimum because this run uses Compute)
    if (has_extension(exts, VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
        VkPhysicalDeviceMeshShaderFeaturesEXT m{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &m};
        vkGetPhysicalDeviceFeatures2(dev.physical, &f2);
        dev.mesh_capable = m.meshShader && m.taskShader;
    }
    int mesh_mode_used = mesh_mode;
    if (mesh_mode_used == 0 && fadd_driver.driverID == VK_DRIVER_ID_MESA_RADV) mesh_mode_used = 2;
    const bool radv_compute = mesh_mode == 0 && mesh_mode_used == 2;
    if (mesh_mode_used == 2) dev.mesh_shader = false;
    else optional(VK_EXT_MESH_SHADER_EXTENSION_NAME, dev.mesh_shader);
    optional(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, dev.external_memory_host);
    optional(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, dev.external_memory_fd);   // (guest memory as udmabufs: vk_memory.cpp import_dma_buf)
    if (dev.external_memory_fd) optional(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, dev.dma_buf_import);
    optional(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, dev.pipeline_library);
    bool module_id = false;   // (not a field of dev: vk_state.h is part of the translator's stamp)
    optional(VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME, module_id);
    if (dev.pipeline_library) optional(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, dev.graphics_pipeline_library);
    optional(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, dev.custom_border_color);
    optional(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, dev.dynamic_state3);
    optional(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, dev.depth_clip_control);
    optional(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, dev.null_descriptor);
    optional(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, dev.index_uint8);
    optional(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, dev.push_descriptor);
    optional(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, dev.conditional_rendering);
    optional(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, dev.provoking_vertex);
    optional(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, dev.vertex_divisor);
    optional(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, dev.stencil_export);
    optional(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, dev.memory_budget);
    // (longshot, RX_VK_MEMORY_PRIORITY, off by default: memory_priority.h. The extension is asked for only with the switch on)
    if (memory_priority::parse(getenv("RX_VK_MEMORY_PRIORITY")).on) optional(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, dev.memory_priority);
    optional(VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME, dev.view_2d_of_3d);
    // (longshot) on by default: with a marker before each draw and dispatch the device has not been lost (five
    // runs); without them it was, every run, shortly after OoT's first gameplay frames (nvlddmkm event 153; the
    // extension alone, RX_VK_CHECKPOINTS=ext, did not keep it). The race they hide is not found yet.
    // RX_VK_CHECKPOINTS=0: off
    if (!getenv("RX_VK_CHECKPOINTS") || strcmp(getenv("RX_VK_CHECKPOINTS"), "0") != 0)
        optional(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, dev.checkpoints);
    // (asked for only: a marker written twice a draw, where the NV ones are not there to hide a race)
    if (const char* c = getenv("RX_VK_CHECKPOINTS"); !dev.checkpoints && c && strcmp(c, "0") != 0 && strcmp(c, "ext") != 0)
        optional(VK_AMD_BUFFER_MARKER_EXTENSION_NAME, dev.buffer_marker);
    optional(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, dev.device_fault);
    optional(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, dev.coop_matrix);
    // (longshot) the GPU's own float add on storage buffers: the game's RED/ATOM.ADD.F32 become one OpAtomicFAddEXT instead
    // of a compare-exchange loop, which gives up after CAS_LOOP_MAX_TRIES attempts and loses that add (eden
    // spirv_emit_context.cpp DefineStorageBuffers). By default with NVIDIA's driver, AMD's Windows driver on 0x1114 from 0x8001a0, and Intel's Windows driver on the
    // Meteor Lake, Arrow Lake, Panther Lake and Lunar Lake iGPUs and the Arc B-series (native_fadd_wanted); RX_VK_NATIVE_FADD=1 asks for it on any GPU, =0 keeps the loop
    // on every one, the extension not even enabled
    const char* const fadd_env = getenv("RX_VK_NATIVE_FADD");
#ifdef _WIN32
    constexpr bool fadd_windows = true;
#else
    constexpr bool fadd_windows = false;
#endif
    const bool fadd_wanted = native_fadd_wanted(fadd_env, fadd_driver.driverID, dev.props.deviceID, dev.props.driverVersion, fadd_windows);
    bool atomic_float = false;
    if (fadd_wanted) optional(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, atomic_float);
    optional(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, dev.swap_mutable);   // (longshot: the overlays, vk_imgui.cpp)
    dlss_device_extensions(want, exts);   // (longshot: NVIDIA DLSS's, vk_dlss.cpp)
    dlss_fg_device_extensions(want, exts);   // (longshot: NVIDIA DLSS Frame Generation's through NGX, asked of NGX)
    bool fsr_descriptor_buffer = false;   // (longshot, linux: only with a Vulkan FSR 4 library beside the game, vk_fsr4.cpp)
    if (fsr4_wants_descriptor_buffer()) optional(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, fsr_descriptor_buffer);
    xess_device_extensions(want, exts);   // (longshot: Intel XeSS's, when its library is there and the instance took its own)
    reflex_device_extensions(want, exts);   // (longshot: NVIDIA Reflex's, vk_reflex.cpp)
    antilag_device_extensions(want, exts);   // (longshot: AMD Anti-Lag 2's, vk_antilag.cpp)

    // the queue: one family for graphics, compute and transfer
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, families.data());
    dev.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < n && dev.queue_family == UINT32_MAX; i++)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            dev.queue_family = i;
    if (dev.queue_family == UINT32_MAX) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = dev.queue_family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    // (longshot) The presents on a queue of a compute family of their own, beside the replay's: RX_VK_PRESENT_QUEUE=1 (the
    // switches' yes/no rule; off by default on every PC). On the replay's queue vkQueuePresentKHR blocks with dev.present_mu
    // held, ~0.8 ms a present with frame interpolation at 4x and up to 8, and the game's main and presentation threads wait
    // there to submit. A second queue of the graphics family takes that lock off the presents, but its presents block longer
    // (~2-4 ms: the graphics work of the two queues takes turns on the GPU) and the made frames are shown late more often.
    // A compute family's queue runs beside the graphics work: ~10% more fps in Hyrule Field at 4x, far fewer long frames, and
    // evenly shown made frames. The swapchain's images are shared by both families (vk_main.cpp create_swapchain), and a
    // family that cannot present to the window leaves it off.
    // Off by default: it loses the GPU on some NVIDIA cards (RTX 4090, RTX 5080) seconds after frame generation starts, a
    // fault never placed; RX_VK_PRESENT_QUEUE=0 stops it.
    // The Debug tab's Present queue row chooses (present_queue= in longshot_options.ini, read here at the start as
    // robust_buffers= below: the menu's options are not loaded yet); RX_VK_PRESENT_QUEUE over it, by the switches' yes/no rule.
    uint32_t compute_family = UINT32_MAX;
    {
        bool want = false;
        if (const char* e = getenv("RX_VK_PRESENT_QUEUE"); e && *e) {
            want = !env_value_off(e);
        } else if (FILE* fl = fopen(options_path().c_str(), "r")) {   // (vk_paths.h)
            char line[80];
            while (fgets(line, sizeof line, fl))
                if (!strncmp(line, "present_queue=", 14)) {
                    want = atoi(line + 14) != 0;
                    break;
                }
            fclose(fl);
        }
        for (uint32_t i = 0; want && i < n && compute_family == UINT32_MAX; i++)
            if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                compute_family = i;
    }
    static const float priorities[2] = {1.0f, 1.0f};
    std::vector<VkDeviceQueueCreateInfo> qs;
    auto queues = [&](uint32_t family, uint32_t count) {
        for (auto& x : qs)
            if (x.queueFamilyIndex == family) { x.queueCount = std::max(x.queueCount, count); return; }
        VkDeviceQueueCreateInfo x{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        x.queueFamilyIndex = family, x.queueCount = count, x.pQueuePriorities = priorities;
        qs.push_back(x);
    };
    queues(dev.queue_family, 1);
    if (compute_family != UINT32_MAX) queues(compute_family, 1);
    (void)q;
    // (the presenter thread's waits for its presents to be shown: VK_KHR_present_id + VK_KHR_present_wait, where the driver has them)
    const bool want_present_wait = has_extension(exts, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                                   has_extension(exts, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    if (want_present_wait)
        for (const char* name : { VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_EXTENSION_NAME }) {
            bool listed = false;
            for (const char* w : want) {
                if (!strcmp(w, name)) { listed = true; break; }
            }
            if (!listed) want.push_back(name);
        }

    // features: what the hardware has of what the replay uses (all of them on the GPUs MKW-class hardware maps to)
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    void** tail = &f13.pNext;
    // (longshot: the load at a program's create call makes a pipeline from the caches by its module's identifier, without the
    // program's code: vk_programs.h. Nothing else uses it)
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT module_idf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT};
    if (module_id) *tail = &module_idf, tail = &module_idf.pNext;
    if (dev.mesh_shader) *tail = &mesh, tail = &mesh.pNext;
    if (dev.null_descriptor) *tail = &robust2, tail = &robust2.pNext;
    if (dev.index_uint8) *tail = &u8, tail = &u8.pNext;
    if (dev.dynamic_state3) *tail = &eds3, tail = &eds3.pNext;
    if (dev.custom_border_color) *tail = &border, tail = &border.pNext;
    if (dev.depth_clip_control) *tail = &clip, tail = &clip.pNext;
    if (dev.conditional_rendering) *tail = &cond, tail = &cond.pNext;
    if (dev.provoking_vertex) *tail = &pv, tail = &pv.pNext;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vdiv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    if (dev.vertex_divisor) *tail = &vdiv, tail = &vdiv.pNext;
    if (dev.graphics_pipeline_library) *tail = &gpl, tail = &gpl.pNext;
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (dev.device_fault) *tail = &fault, tail = &fault.pNext;
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT v2d3d{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT};
    if (dev.view_2d_of_3d) *tail = &v2d3d, tail = &v2d3d.pNext;
    VkPhysicalDeviceMemoryPriorityFeaturesEXT mem_priority{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    if (dev.memory_priority) *tail = &mem_priority, tail = &mem_priority.pNext;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    if (dev.coop_matrix) *tail = &coop, tail = &coop.pNext;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT afloat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    if (atomic_float) *tail = &afloat, tail = &afloat.pNext;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    if (fsr_descriptor_buffer) *tail = &descriptor_buffer, tail = &descriptor_buffer.pNext;
    reflex_device_features(tail);   // (present ids, when NVIDIA Reflex's extensions are there)
    VkPhysicalDevicePresentIdFeaturesKHR present_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    if (want_present_wait) {
        if (!reflex_present_id_features()) *tail = &present_id, tail = &present_id.pNext;   // (one of each struct in the chain)
        *tail = &present_wait, tail = &present_wait.pNext;
    }
    antilag_device_features(tail);   // (AMD Anti-Lag 2's, when its extension is there)
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    if (module_id && module_idf.shaderModuleIdentifier) module_ids_on();
    descriptor_buffer.descriptorBufferCaptureReplay = VK_FALSE;
    dev.view_2d_of_3d = dev.view_2d_of_3d && v2d3d.sampler2DViewOf3D;
    dev.memory_priority = dev.memory_priority && mem_priority.memoryPriority;
    coop.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    dev.coop_matrix = dev.coop_matrix && coop.cooperativeMatrix &&
                      f12.vulkanMemoryModel && f12.vulkanMemoryModelDeviceScope;
    {   // (only the float add on buffers on: the one the translator emits)
        dev.float_atomic_add = atomic_float && afloat.shaderBufferFloat32AtomicAdd;
        void* const next = afloat.pNext;
        afloat = VkPhysicalDeviceShaderAtomicFloatFeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
        afloat.pNext = next;
        afloat.shaderBufferFloat32AtomicAdd = dev.float_atomic_add ? VK_TRUE : VK_FALSE;
        const char* const how = dev.float_atomic_add ? "native (VK_EXT_shader_atomic_float)"
                                : !fadd_wanted       ? "compare-exchange loop"
                                : atomic_float       ? "compare-exchange loop (no shaderBufferFloat32AtomicAdd)"
                                                     : "compare-exchange loop (no VK_EXT_shader_atomic_float)";
        if (fadd_env) logf("vk: float atomic add on storage buffers: %s, RX_VK_NATIVE_FADD=%s", how, fadd_env);
        else logf("vk: float atomic add on storage buffers: %s, %s", how,
                  !fadd_wanted ? "by default only with NVIDIA's driver, Intel's Windows driver on Meteor Lake, Arrow Lake, Panther Lake, Lunar Lake and Battlemage, "
                                 "and AMD's Windows driver on Radeon 860M/840M (RX_VK_NATIVE_FADD=1: native where the GPU has it)"
                  : fadd_driver.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY ? "NVIDIA's driver"
                  : fadd_driver.driverID == VK_DRIVER_ID_AMD_PROPRIETARY    ? "AMD's Windows driver, Radeon 860M/840M (0x1114) from 0x8001a0"
                                                                            : "Intel's Windows driver, Meteor Lake, Arrow Lake, Panther Lake, Lunar Lake or Battlemage");
    }
    // (bounds-checked buffer access: an out-of-range access dropped instead of faulting the device. On by default: the GPU
    // culling pass 53caac5f can fault the device while a save loads, and the scene's lighting reads garbage for single frames
    // (black flashes); with it, neither, at the same GPU time.
    // The 'Robust buffers' row of longshot_options.ini chooses (0 Auto: on, on every vendor; 1 Off; 2 On); RX_VK_ROBUST=0 or 1
    // over it. Off is null descriptors only: some NVIDIA cards run slower with it on)
    static const bool robust = []() -> bool {
        if (const char* e = getenv("RX_VK_ROBUST")) return atoi(e) != 0;
#ifndef _WIN32
        if (const auto& dump = restored_shader_dump(); dump.dir[0] && dump.robust_buffers >= 0) {
            logf("vk: Robust buffers from the restored shader dump: %s", dump.robust_buffers ? "on" : "off");
            return dump.robust_buffers != 0;
        }
#endif
        int v = 0;   // (Auto)
        if (FILE* f = fopen(options_path().c_str(), "r")) {   // (vk_paths.h)
            char line[80];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "robust_buffers=", 15)) {
                    v = atoi(line + 15);
                    break;
                }
            fclose(f);   // (once: the PR closed it twice when the row said Auto)
        }
        return v != 1;   // (Auto and On: on)
    }();
    const bool robust_ok = robust && robust2.robustBufferAccess2 && f.features.robustBufferAccess;
    robust2.robustBufferAccess2 = robust_ok ? VK_TRUE : VK_FALSE;
    robust2.robustImageAccess2 = VK_FALSE;
    dev.null_descriptor = dev.null_descriptor && robust2.nullDescriptor;
    pv.transformFeedbackPreservesProvokingVertex = VK_FALSE;
    dev.provoking_vertex = dev.provoking_vertex && pv.provokingVertexLast;
    dev.vertex_divisor = dev.vertex_divisor && vdiv.vertexAttributeInstanceRateDivisor;   // (a divisor over 1 needs the feature, not only the extension)
    dev.conditional_rendering = dev.conditional_rendering && cond.conditionalRendering;
    // (longshot) the occlusion queries' real counts (vk_replay.cpp, RX_VK_OCC_QUERIES): a sample count, not only "any", and
    // their reset from the CPU once read (both on when the device has them: the queried chain is what it is created with)
    dev.occlusion_precise = f.features.occlusionQueryPrecise == VK_TRUE;
    dev.host_query_reset = f12.hostQueryReset == VK_TRUE;
    dev.custom_border_color = dev.custom_border_color && border.customBorderColors && border.customBorderColorWithoutFormat;
    dev.depth_clip_control = dev.depth_clip_control && clip.depthClipControl;
    dev.graphics_pipeline_library = dev.graphics_pipeline_library && gpl.graphicsPipelineLibrary;
    if (dev.graphics_pipeline_library) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT lp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &lp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.graphics_pipeline_library = lp.graphicsPipelineLibraryFastLinking != VK_FALSE;
    }
    dev.eds3_polygon = dev.dynamic_state3 && eds3.extendedDynamicState3PolygonMode;
    dev.eds3_clamp = dev.dynamic_state3 && eds3.extendedDynamicState3DepthClampEnable;
    dev.eds3_blend = dev.dynamic_state3 && eds3.extendedDynamicState3ColorBlendEnable &&
                     eds3.extendedDynamicState3ColorBlendEquation && eds3.extendedDynamicState3ColorWriteMask;
    dev.eds3_a2c = dev.dynamic_state3 && eds3.extendedDynamicState3AlphaToCoverageEnable;
    dev.vk12 = f12;
    dev.vk13 = f13;
    {   // (longshot, after TotK Recomp's vk_caps.c) NVN's programs are written for 32 lanes: the translator is told 32, and the lane
        // shuffles of OoT's networks and the lane masks of its mesh programs depend on it. NVIDIA's GPUs have no other size; AMD's
        // default to 64 where they like, and the networks' output came out as coloured noise (Longshot issues 10 and 14)
        VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        drv.pNext = &fc;
        VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        sc.pNext = &drv;
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        sg.pNext = &sc;
        VkPhysicalDeviceMeshShaderPropertiesEXT mesh_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sg;
        if (dev.mesh_shader) fc.pNext = &mesh_props;   // (with the extension on only)
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        if (dev.mesh_shader) dev.max_mesh_groups = std::min(mesh_props.maxMeshWorkGroupCount[0], mesh_props.maxMeshWorkGroupTotalCount);
        const bool pin = f13.subgroupSizeControl && sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32 && !env_on("RX_VK_NO_SUBGROUP_PIN");
        dev.sg32_stages = pin ? sc.requiredSubgroupSizeStages : 0;
        dev.max_workgroup_subgroups = sc.maxComputeWorkgroupSubgroups;
        dev.subgroup_size = sg.subgroupSize;
        dev.subgroup_min = sc.minSubgroupSize;
        dev.subgroup_stages = sg.supportedStages;
        fc.pNext = nullptr;
        dev.float_controls = fc;
        dev.subgroup_max = sc.maxSubgroupSize > sg.subgroupSize ? sc.maxSubgroupSize : sg.subgroupSize;
        dev.driver_id = drv.driverID;
        // (RADV on GCN 1-4, GFX6-8: no 16-bit shader inputs and outputs there, which RADV has from GFX9 (packed 16-bit math) on.
        // Its compiler cannot build the biggest mesh fallback programs: fallback_size_policy.h)
        dev.radv_gcn = drv.driverID == VK_DRIVER_ID_MESA_RADV && !f11.storageInputOutput16;
        logf("vk: subgroups: default %u lanes, %u-%u, pinned to 32 on stages %#x; RADV on GCN 1-4: %s", sg.subgroupSize, sc.minSubgroupSize,
             sc.maxSubgroupSize, (unsigned)dev.sg32_stages, dev.radv_gcn ? "yes" : "no");
        logf("vk: shader caps: subgroup stages %#x; denorm independence %d, fp32 denorm preserve %u flush %u, signed zero fp16 %u fp32 %u; "
             "int64 buffer atomics %u", (unsigned)sg.supportedStages, (int)fc.denormBehaviorIndependence, fc.shaderDenormPreserveFloat32,
             fc.shaderDenormFlushToZeroFloat32, fc.shaderSignedZeroInfNanPreserveFloat16, fc.shaderSignedZeroInfNanPreserveFloat32,
             dev.vk12.shaderBufferInt64Atomics);
        if (drv.driverID == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA && getenv("shader_spilling_rate"))
            logf("vk: Mesa's Intel compiler spills at rate %s (driconf shader_spilling_rate: 2 unless set)", getenv("shader_spilling_rate"));
    }
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) {
        logf("vk: the GPU lacks dynamic rendering, synchronization2 or timeline semaphores");
        return false;
    }
    mesh.multiviewMeshShader = mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;   // (not used)
    dev.mesh_shader = dev.mesh_shader && mesh.meshShader && mesh.taskShader;
    // (task and mesh shader counts in pipeline statistics, a feature of their own: RADV has it off on RDNA2, and a query
    // pool made with those counts all the same can hang the GPU)
    dev.mesh_queries = dev.mesh_shader && mesh.meshShaderQueries;
    // (longshot) RDNA1 and older GPUs: no mesh shaders, but task and mesh programs run as compute shaders
    // that write their vertices to buffers, drawn through an ordinary graphics pipeline. It needs buffer device addresses.
    dev.mesh_fallback = !dev.mesh_shader && f12.bufferDeviceAddress && !env_on("RX_VK_NO_MESH_FALLBACK");
    if (dev.mesh_fallback)
        logf("vk: mesh shaders %s: task and mesh programs run as compute shaders (issue 640 fallback)",
             dev.mesh_capable ? "offered by the driver but not used" : "not offered by the driver");
    bool task_mesh_fallback = false;
#ifdef _WIN32
    // AMD's Windows driver can time out on Navi 21 to 23 on task + mesh draws and on mesh-only draws: mesh_mode.h
    task_mesh_fallback = mesh_rule::task_mesh_fallback_default(dev.props.vendorID, dev.props.deviceID);
#endif
    const bool rdna2_windows = task_mesh_fallback;
    if (mesh_mode == 1) task_mesh_fallback = false;   // (Native: the card's own mesh shaders for every draw)
    if (const char* e = getenv("RX_VK_TASK_MESH_FALLBACK")) task_mesh_fallback = atoi(e) != 0;
    dev.task_mesh_fallback = task_mesh_fallback && dev.mesh_shader && f12.bufferDeviceAddress && !env_on("RX_VK_NO_MESH_FALLBACK");
    dev.mesh_only_fallback = dev.task_mesh_fallback && rdna2_windows;
    if (dev.task_mesh_fallback)
        logf("vk: %s programs use the compute fallback (RX_VK_TASK_MESH_FALLBACK=0: native)",
             dev.mesh_only_fallback ? "mesh-only and task + mesh" : "task + mesh");
    logf("vk: mesh shaders: %s, from %s: %s", mesh_mode == 2 ? "Compute" : mesh_mode == 1 ? "Native" : "Automatic",
         mesh_choice.why, dev.mesh_shader ? (dev.mesh_only_fallback ? "mesh-only and task + mesh draws on the compute path"
                            : dev.task_mesh_fallback ? "native, task + mesh draws on the compute path" : "native")
                         : dev.mesh_fallback ? (radv_compute ? "the compute path (RADV's own can hang the GPU, issue 2245)"
                                                            : "the compute path")
                                             : "none");
    mesh_note = mesh_rule::device_note(dev.mesh_capable, dev.mesh_shader, mesh_mode, radv_compute);
    if (robust) logf("vk: robust buffer access %s", robust_ok ? "on" : "not supported");
    f.features.robustBufferAccess = robust_ok ? VK_TRUE : VK_FALSE;   // (off by default: the game's accesses stay in their buffers)
    dev.features = f.features;
    dev.sparse_images = (families[dev.queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && f.features.sparseBinding &&
                        f.features.sparseResidencyImage2D && !(getenv("RX_VK_SPARSE") && !atoi(getenv("RX_VK_SPARSE")));
    dev.sparse_buffers = (families[dev.queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && f.features.sparseBinding;
    {   // (longshot) what this PC is, learnt once, and the defaults it gets by it: device_policy.h. Most sites still ask the
        // device their own way and decide for themselves; the log's "vk: policy:" lines show what the rules give this PC
        dev.driver = fadd_driver;   // (asked above; kept, as the facts point at its driverInfo)
        dev.driver.pNext = nullptr;
        DeviceFacts& pf = dev.facts;
        pf.vendor_id = dev.props.vendorID, pf.device_id = dev.props.deviceID, pf.driver_version = dev.props.driverVersion;
        pf.driver_id = dev.driver.driverID;
        pf.device_type = dev.props.deviceType;
        pf.driver_info = dev.driver.driverInfo;
        pf.device_name = dev.props.deviceName;
        const char* const deck = getenv("SteamDeck");
        pf.steam_deck_env = deck && atoi(deck) == 1;
        pf.vram_heap = device_vram_bytes();
        uint32_t vram_heap_index = UINT32_MAX;
        for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
            if ((dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                (vram_heap_index == UINT32_MAX || dev.memory.memoryHeaps[h].size > dev.memory.memoryHeaps[vram_heap_index].size))
                vram_heap_index = h;
        for (uint32_t t = 0; t < dev.memory.memoryTypeCount; t++) {
            const VkMemoryPropertyFlags both = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            if (dev.memory.memoryTypes[t].heapIndex == vram_heap_index && (dev.memory.memoryTypes[t].propertyFlags & both) == both)
                pf.rebar = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        }
        {   // (the allowance as vram_usage will answer once VMA is there: the driver's budgets, or VMA's 80% of each heap)
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 m2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
            m2.pNext = &budget;
            const auto ask = !dev.memory_budget ? nullptr
                             : (PFN_vkGetPhysicalDeviceMemoryProperties2)instance_proc()(dev.instance, "vkGetPhysicalDeviceMemoryProperties2");
            if (ask) ask(dev.physical, &m2);
            uint64_t allowed = 0;
            for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
                if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    allowed += ask ? std::min<uint64_t>(budget.heapBudget[h], dev.memory.memoryHeaps[h].size)
                                   : dev.memory.memoryHeaps[h].size * 8 / 10;
            // (RX_VK_TEST_ALLOWED_MB, as vram_usage has it)
            const uint64_t test_allowed = getenv("RX_VK_TEST_ALLOWED_MB") ? (uint64_t)atoi(getenv("RX_VK_TEST_ALLOWED_MB")) << 20 : 0;
            pf.allowed_at_start = test_allowed && allowed > test_allowed ? test_allowed : allowed;
        }
#ifdef _WIN32
        pf.windows = true;
        MEMORYSTATUSEX ram{};
        ram.dwLength = sizeof ram;
        if (GlobalMemoryStatusEx(&ram)) pf.ram = ram.ullTotalPhys;
#else
        const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page > 0) pf.ram = (uint64_t)pages * (uint64_t)page;
#endif
        pf.cpus_all = std::thread::hardware_concurrency();
        pf.cpus_usable = usable_cpus();
        pf.cores_usable = usable_cores();   // (longshot: Threaded rendering's Auto counts by it)
        pf.mesh_shader = dev.mesh_capable;
        pf.mesh_queries = dev.mesh_queries;
        pf.buffer_device_address = f12.bufferDeviceAddress;
        if (has_extension(exts, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME)) {   // (the card's, whether this run asks for it or not)
            VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 card{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &af};
            vkGetPhysicalDeviceFeatures2(dev.physical, &card);
            pf.float32_atomic_add = af.shaderBufferFloat32AtomicAdd;
        }
        pf.coop_matrix_ext = has_extension(exts, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        pf.nv_checkpoints_ext = has_extension(exts, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        pf.storage_io16 = f11.storageInputOutput16;
        pf.storage_buffer_array_dynamic_indexing = f.features.shaderStorageBufferArrayDynamicIndexing;
        pf.robust_image_access = f13.robustImageAccess;
        pf.subgroup_size = dev.subgroup_size, pf.subgroup_max = dev.subgroup_max;
        pf.sg32_stages = dev.sg32_stages;
        dev.policy = device_policy(pf);
        // (and the switches over them, read once, by one rule: device_policy_env)
        device_policy_env(dev.policy, pf, [](const char* name) -> const char* { return getenv(name); });
        // (longshot) the translation's vendor and driver workarounds (device_policy.h fetch_bounds, nan_bits, gather_offset),
        // each forced by its RX_VK_ switch
        device_policy_fixes_env(dev.policy, [](const char* name) -> const char* { return getenv(name); });
        native_zstd_default.store(dev.policy.native_zstd_keep_core ? 2 : 1, std::memory_order_release);
        logf("vk: vendor fixes for %s (%s): out-of-bounds texel fetches 0 %d, NaN tests by bits %d, gather offset %d",
             dev.driver.driverName, dev.driver.driverInfo, (int)dev.policy.fetch_bounds, (int)dev.policy.nan_bits,
             (int)dev.policy.gather_offset);
    }
    // (longshot) Intel XeSS's features (vk_xess.cpp), last, into the chain as it will be made: the library sets what it needs in
    // the renderer's structures and adds what it lacks (VK_EXT_mutable_descriptor_type's); nothing without its library, and a
    // refusal leaves the chain as it was and XeSS out for the run
    void* chain = &f;
    xess_device_features(&chain, tail);
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = chain;
    ci.queueCreateInfoCount = (uint32_t)qs.size();
    ci.pQueueCreateInfos = qs.data();
    ci.enabledExtensionCount = (uint32_t)want.size();
    ci.ppEnabledExtensionNames = want.data();
    if (vkCreateDevice(dev.physical, &ci, nullptr, &dev.device) != VK_SUCCESS) {
        logf("vk: vkCreateDevice failed");
        return false;
    }


    load_device(dev.device);
    // (longshot: when programs are loaded, vk_programs.h. Without mesh shaders, or with the task + mesh draws through the
    // fallback, a start keeps the fallback's programs ready, and loads at create have not been shown to be as good there)
    programs_load_decide(dev.mesh_shader && !dev.mesh_fallback && !dev.task_mesh_fallback);
    vkGetDeviceQueue(dev.device, dev.queue_family, 0, &dev.queue);
    dev.present_queue = VK_NULL_HANDLE;
    dev.present_family = dev.queue_family;
    if (compute_family != UINT32_MAX) vkGetDeviceQueue(dev.device, compute_family, 0, &dev.present_queue), dev.present_family = compute_family;
    {
        const VkPhysicalDevicePresentIdFeaturesKHR* pid = reflex_present_id_features();
        dev.present_wait = want_present_wait && (pid ? pid->presentId : present_id.presentId) && present_wait.presentWait;
        logf("vk: the presenter thread waits for its presents to be shown: %s", dev.present_wait ? "yes (VK_KHR_present_wait)" : "no");
    }
    if (dev.buffer_marker) buffer_markers_init();
    coopmat_init();
    reflex_device_created();
    antilag_device_created();
    // (longshot: on the Steam Deck the GPU held at amdgpu's peak power state while the game runs (RX_VK_GPU_CLOCK=0: not), gpu_clock.h;
    // the device's render node from VK_EXT_physical_device_drm, which needs no enabling to be asked)
    gpu_clock_device_created(has_extension(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME));
    if (dev.push_descriptor) {
        VkPhysicalDevicePushDescriptorPropertiesKHR pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &pp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.max_push_descriptors = pp.maxPushDescriptors;
    }
    if (dev.vertex_divisor) {
        VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT vp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &vp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.max_vertex_divisor = vp.maxVertexAttribDivisor;
    }
    return true;
}

} // namespace

std::string device_report_text() {
    if (!dev.physical) return "";
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    // (driverName and driverInfo: "NVIDIA" "580.95.05", "radv" "Mesa 25.2.4", "Intel Corporation" "101.6130")
    std::string text = std::string(p2.properties.deviceName) + ", " + drv.driverName;
    if (drv.driverInfo[0]) text += std::string(" ") + drv.driverInfo;
    return text;
}
/// (longshot) The UNORM format of an sRGB one (the same bytes, read and written as they are), else the format itself
VkFormat unorm_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
/// (longshot) The views of the swapchain's images let go (before the swapchain they view: the device idle already)
void swap_views_drop() {
    for (auto view : dev.swap_views)
        if (view) vkDestroyImageView(dev.device, view, nullptr);
    dev.swap_views.clear();
}
/// (longshot) A view of each of the swapchain's images, for Dear ImGui's overlays (vk_imgui.cpp), when they can be drawn into,
/// in the format given (the swapchain's, or its UNORM one when the swapchain was made mutable)
void swap_views_make(VkImageUsageFlags usage, VkFormat format) {
    swap_views_drop();
    dev.swap_views.assign(dev.swap_images.size(), VK_NULL_HANDLE);
    dev.swap_view_format = format;
    if (!(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return;
    for (size_t i = 0; i < dev.swap_images.size(); i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = dev.swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev.device, &vi, nullptr, &dev.swap_views[i]) != VK_SUCCESS) dev.swap_views[i] = VK_NULL_HANDLE;
    }
}

/// (longshot) Frame generation as the swapchain is made for it: the fixed multiplier, 2 to 4 (more takes no more images), or 0
/// for off and Auto, as it runs (vk_framegen.h framegen_now: at most DLSS FG's top while it makes the frames). Its present
/// mode under V-Sync Auto and its spare images follow it; present_image makes the swapchain again when it changes
int swapchain_framegen() { const int framegen = framegen_now(); return framegen >= 2 ? std::min(framegen, 4) : 0; }

/// (longshot) Whether the swapchain is made for NVIDIA DLSS Frame Generation: a fixed multiplier, and DLSS-G making the sets
/// (vk_dlss.cpp dlss_fg_runs: asked for, on a device it runs on, its runtime making as many frames as the multiplier asks).
/// V-Sync Auto is FIFO for it, where it was measured; the renderer's own methods and other GPUs keep Auto's mailbox (or
/// immediate). present_image makes the swapchain again when it changes: NGX's answer comes at the start-up screen or the
/// first present with a multiplier, so a swapchain made for DLSS-G before a no is made once more without. (The set's made
/// frames are the multiplier as it runs less 1, framegen_now: never more than its runtime makes, since the multiplier stops
/// at that + 1 while DLSS-G makes the frames. swapchain_framegen stops at 4, for the images)
bool swapchain_dlssg() { return swapchain_framegen() >= 2 && dlss_fg_runs(framegen_now() - 1); }

void retire_swapchain() {
    if (!dev.swapchain) return;
    device_wait_idle_locked();
    reflex_swapchain_destroyed();
    fg_latency_swapchain_gone();   // (RX_VK_FG_LATENCY_LOG's waits for its presents)
    swap_views_drop();
    for (auto semaphore : dev.swap_rendered) vkDestroySemaphore(dev.device, semaphore, nullptr);
    vkDestroySwapchainKHR(dev.device, dev.swapchain, nullptr);
    dev.swapchain = VK_NULL_HANDLE;
    ++dev.swapchain_generation;
    dev.swap_rendered.clear();
    dev.swap_images.clear();
    dev.swap_spare = 0;
    dev.swap_slots = 3;
}

bool invalidate_lost_surface(VkResult result) {
    if (result != VK_ERROR_SURFACE_LOST_KHR) return false;
    // (longshot, review) the same rule as the close and create_swapchain, at this third choke:
    // retire clears the vector the presenter thread reads after its waits. A present in the making
    // keeps surface and chain: handled anyway — the next frame asks again (acquire answers lost
    // again, present_out reports again), so a wedge costs frames, never a destroyed chain
    if (!swapchain_made_ready()) {
        static unsigned told = 0;
        if (told++ < 8) logf("vk: the surface is lost but a present is in the making: surface and chain wait for it");
        return true;
    }
    retire_swapchain();
    if (dev.surface) vkDestroySurfaceKHR(dev.instance, dev.surface, nullptr);
    dev.surface = VK_NULL_HANDLE;
    dev.present_queue_supported = false;
    static unsigned logged = 0;
    if (logged++ < 8) logf("vk: presentation surface lost; will retry with a replacement");
    return true;
}

namespace {
bool open_window_surface(bool log_failure);
void window_size(uint32_t* width, uint32_t* height);
int system_hdr_on(std::string* display);
}

bool create_swapchain(uint32_t width, uint32_t height) {
    // (longshot, review) the drain's answer is not decoration: the presenter thread holds this
    // chain's image index across its fence and shown-waits and reads dev.swap_rendered[index]
    // after, and this function's retire (oldSwapchain passes even on a failed create) destroys the
    // chain and clears that vector. A present still in the making keeps its chain: answer not-made,
    // and the resize path re-publishes its request for the next frame (vk_replay.cpp)
    if (!swapchain_made_ready()) return false;
    if (!dev.surface) {
        window_size(&width, &height);
        if (!width || !height) return false;
        using Clock = std::chrono::steady_clock;
        static Clock::time_point next_attempt;
        const auto now = Clock::now();
        if (now < next_attempt) return false;
        next_attempt = now + std::chrono::milliseconds(250);
        if (!open_window_surface(false)) return false;
    }
    VkSurfaceCapabilitiesKHR caps;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical, dev.surface, &caps);
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS) {
        logf("vk: querying surface capabilities failed (VkResult %d)", (int)result);
        return false;
    }
    if (caps.currentExtent.width != UINT32_MAX) width = caps.currentExtent.width, height = caps.currentExtent.height;
    logf("vk: surface caps images %u..%u extent %ux%u..%ux%u usage %#x alpha %#x transform %#x",
         caps.minImageCount, caps.maxImageCount, caps.minImageExtent.width, caps.minImageExtent.height,
         caps.maxImageExtent.width, caps.maxImageExtent.height, caps.supportedUsageFlags,
         caps.supportedCompositeAlpha, caps.supportedTransforms);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        logf("vk: surface does not support transfer-destination swapchain images");
        return false;
    }
    if (!width || !height) return false;
    uint32_t n = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, nullptr);
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS || !n) {
        logf("vk: querying surface formats failed (VkResult %d, count %u)", (int)result, n);
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(n);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, formats.data());
    if (invalidate_lost_surface(result) || result != VK_SUCCESS || !n) return false;
    formats.resize(n);
    VkSurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats)   // (an UNORM target: the game's window textures are presented as they are)
        // (in sRGB: RADV on Wayland lists every format again with PASS_THROUGH_EXT and the compositor's colour spaces,
        // and the last UNORM entry left the compositor an untagged surface, a grey picture. Still the last sRGB one, so
        // a driver that lists only sRGB (NVIDIA, X11, Windows) keeps the format it had.)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            fmt = f;
    // (longshot) the Insert menu's HDR: a 10-bit HDR10 surface when the display lists one (Windows: "Use HDR" on in its display
    // settings), the picture then written by framegen_hdr. Otherwise the SDR one above, and why (dev.hdr_sdr_why: Windows'
    // own setting tells "HDR off" from "the driver lists none with HDR on", as NVIDIA's can), said
    // again when the reason or the display asked about changes, and when the menu's HDR was turned off in between
    dev.hdr = false;
    dev.hdr_sdr_why = hdr_sdr_none;
    static int told = -1;   // (the HdrSdrWhy last said, hdr_sdr_none an HDR10 surface; -1 nothing yet, or HDR off since)
    static std::string told_display;
    if (!opts.hdr) told = -1;
    if (opts.hdr) {
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
                f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
                fmt = f, dev.hdr = true;
        std::string display;   // (the display Windows was asked about, named in the line: a PC can have several)
        if (!dev.hdr) {
            const int system_hdr = dev.swapchain_colorspace ? system_hdr_on(&display) : -1;
            dev.hdr_sdr_why = !dev.swapchain_colorspace ? hdr_sdr_no_extension
                              : system_hdr == 0     ? hdr_sdr_os_off
                              : system_hdr == 1     ? hdr_sdr_driver
                                                    : hdr_sdr_unknown;
            if (!display.empty()) display = " (" + display + ")";
        }
        if ((int)dev.hdr_sdr_why != told || display != told_display) {
            told = dev.hdr_sdr_why, told_display = display;
            static const char* const why[] = {
                "", " (no VK_EXT_swapchain_colorspace)", " (HDR is off for this display in Windows' display settings)",
                (" though Windows has HDR on for this display: the graphics driver offers the game none (a clean reinstall of "
                 "the driver, then a restart: longshot.magicbean.dev/help)"),
                " (is HDR on in the display settings?)"};
            static_assert(sizeof why / sizeof *why == hdr_sdr_count, "create_swapchain: a line for each HdrSdrWhy");
            if (dev.hdr)
                logf("vk: HDR: an HDR10 surface (format %d), paper white %d nits, peak %d nits", (int)fmt.format, opts.hdr_white,
                     opts.hdr_peak);
            else
                logf("vk: HDR: the display%s lists no HDR10 surface%s: SDR", display.c_str(), why[dev.hdr_sdr_why]);
        }
    }
    // mailbox when the driver has it (latest frame, no tear). The game paces itself (nn_vi.c).
    n = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes;
    if (result == VK_SUCCESS && n) {
        modes.resize(n);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, modes.data());
    }
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS || modes.empty())
        logf("vk: querying present modes failed (VkResult %d, count %u); trying mandatory FIFO mode", (int)result, n);
    // (longshot) the Insert menu's V-Sync (menu_vsync: RX_VK_FIFO=1 On, RX_VK_IMMEDIATE=1 Off for a run): On is FIFO, as on a
    // card or driver with vsync forced; Auto and Off as below. FIFO is the one mode every driver has
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    const int vsync = menu_vsync();
    // (longshot) V-Sync Auto with DLSS Frame Generation at 2x and up: FIFO. Mailbox, NVIDIA's Fast Sync, puts the made frames on
    // the refresh grid whenever they come slower than the display (DLSS Frame Gen 3x: ~135 shown on a 165 Hz screen), so evenly
    // timed presents still reached the screen unevenly; FIFO on a VRR display shows each one when it is presented. Only where
    // that was measured (swapchain_dlssg): the renderer's own frame generation and other GPUs keep Auto as it was, also when
    // DLSS-G is asked for but the renderer's own makes the frames (NGX's no, or given up; a multiplier over what its runtime
    // makes runs at its most + 1 once NGX has answered, framegen_now)
    const bool fg_fifo = vsync == 0 && swapchain_dlssg();
    if (fg_fifo) logf("vk: V-Sync Auto with DLSS Frame Generation at %dx: FIFO", framegen_now());   // (not the images' 4 at most)
    else if (vsync == 0 && swapchain_framegen() >= 2 && dlss_fg_wanted() && dlss_why() == DLSS_OK)
        logf("vk: V-Sync Auto with frame generation at %dx, its frames the renderer's own (DLSS-G does not make them): Auto's "
             "mode, as without DLSS-G", framegen_now());
    if (vsync != 1) {
        if (!fg_fifo)
            for (auto m : modes)
                if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
        // (Windows, no mailbox: AMD's driver has none, and FIFO held anything under 60 fps at 30.
        //  Linux: immediate on NVIDIA's X11 path turns vblank off for every output, so the other
        //  monitor and other programs tear: Auto leaves it to V-Sync Off, which asks for it.)
        const bool want_immediate = vsync == 2;
#ifdef _WIN32
        if (want_immediate || (mode == VK_PRESENT_MODE_FIFO_KHR && !fg_fifo))
#else
        if (want_immediate)
#endif
            for (auto m : modes)
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = dev.surface;
    ci.minImageCount = std::max(caps.minImageCount, 4u);   // (longshot: mailbox with frame interpolation: an image always free)
    // doc: docs/dev/reference/architecture.md#frame-generations-presents (rule 4: two sets' spare images on the timeline)
    // (longshot) A fixed frame generation multiplier's set is presented ahead (vk_framegen.cpp present_ahead_wanted): each
    // of its frames holds an image from the copy to its present, so the set needs as many spare images as it has frames. 3x
    // and 4x ask for that many over the surface's least (an NVIDIA surface: 5 and 6 images); a set the chain cannot hold is
    // shown one at a time as before. Two sets' worth while the sets are on the game's timeline, whatever the present mode:
    // the next set's copies go to the GPU as soon as it comes, while this one is still shown (display_main's take_early; an
    // NVIDIA surface: 6 images at 2x, its most, 8, from 3x up; at 4x, under two sets, the next set's later copies wait for
    // images). Under FIFO with one set's, the copies go to the GPU once the set before is shown, behind the game's next
    // frame, and the GPU's delay (vk_framegen.cpp pace_delay) grows with its own lateness. RX_VK_FG_TIMELINE=0 (the sets
    // one at a time, display_main): one set's. Under FIFO a set with more frames than the chain can ever hold spare (an NVIDIA
    // surface, 8 images: 7x and 8x) is not presented ahead and goes out as it comes: two sets' images there only deepen
    // FIFO's queue, two refreshes more delay: one set's. (present_image makes the chain
    // again when the multiplier changes past 4x too. The multiplier as it runs, framegen_now: a saved 7x or 8x that DLSS FG
    // makes at 6x is a set of 6)
    static const char* timeline_env = getenv("RX_VK_FG_TIMELINE");
    const bool timeline = !(timeline_env && env_value_off(timeline_env));
    const bool set_too_big = mode == VK_PRESENT_MODE_FIFO_KHR && caps.maxImageCount &&
                             (uint32_t)std::max(framegen_now(), 0) > caps.maxImageCount - std::min(caps.maxImageCount, caps.minImageCount);
    // (under FIFO, display_main holds the frames a refresh apart (vk_framegen.cpp fifo_slot), which needs the display's refresh.
    // With none known (display_refresh_mhz 0) the frames go as they come, and two sets' images would only let FIFO queue more
    // of them than one set does: more delay, nothing gained. One set's there)
    const bool fifo_unpaced = mode == VK_PRESENT_MODE_FIFO_KHR && display_refresh_mhz() <= 0;
    const uint32_t fg_sets = timeline && !set_too_big && !fifo_unpaced ? 2 : 1;
    // (at most kPresentSlotsMax over the least, present_image's frame slots: an image past them would share a slot with
    // the set still shown (docs/dev/reference/architecture.md, frame generation's presents, rule 4) and cost video memory for nothing. swapchain_framegen stops at
    // 4, so two sets ask for 8 today; the cap keeps that so if it changes)
    if (swapchain_framegen() >= 3 || (swapchain_framegen() >= 2 && fg_sets == 2))
        ci.minImageCount = std::max(ci.minImageCount, caps.minImageCount + std::min<uint32_t>(fg_sets * (uint32_t)swapchain_framegen(),
                                                                                           (uint32_t)kPresentSlotsMax));
    // (present_image's frame slots for the same sets: a slot per frame of two where the next set is taken early, else 3 as
    // before. The chain is made again when the multiplier, V-Sync or RX_VK_FG_TIMELINE change, so the count changes only
    // with it. Stored with the chain below: retire_swapchain sets 3 for the old one)
    const uint32_t slots_made = (uint32_t)present_slots_for(swapchain_framegen(), fg_sets == 2);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = {width, height};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // The renderer copies into swapchain images. Readback is optional and only available on surfaces that allow it.
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // (longshot) the overlays Dear ImGui draws (vk_imgui.cpp) render into it. Every surface allows it (the Vulkan spec)
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) {
        for (VkCompositeAlphaFlagBitsKHR alpha : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                                  VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & alpha) { ci.compositeAlpha = alpha; break; }
    }
    ci.presentMode = mode;
    dev.swap_mode = mode;
    uint32_t sharing[3] = {dev.queue_family};
    uint32_t sharing_n = 1;
    auto share = [&](uint32_t family) {
        for (uint32_t i = 0; i < sharing_n; i++)
            if (sharing[i] == family) return;
        sharing[sharing_n++] = family;
    };
    if (dev.present_queue_supported) share(dev.present_family);
    if (sharing_n > 1) {   // (presented from another family: no ownership moves)
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = sharing_n;
        ci.pQueueFamilyIndices = sharing;
    }
    ci.clipped = VK_TRUE;
#ifdef _WIN32
    // Streamline can release the old swapchain's images inside its create hook.
    // Retire explicitly on NVIDIA so the game and overlay release their views first.
    if (dev.props.vendorID == 0x10de) retire_swapchain();
#endif
    ci.oldSwapchain = dev.swapchain;
    // (longshot) an sRGB swapchain (some drivers list no UNORM format) made mutable when the driver can: the overlays draw
    // through UNORM views, their colours as they are, as on a UNORM swapchain. Without it, vk_imgui.cpp makes them linear
    const VkFormat view_formats[2] = {fmt.format, unorm_of(fmt.format)};
    VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    const bool mutable_view = dev.swap_mutable && view_formats[1] != fmt.format && (ci.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (mutable_view) {
        ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        format_list.viewFormatCount = 2;
        format_list.pViewFormats = view_formats;
        format_list.pNext = ci.pNext;
        ci.pNext = &format_list;
    }
    reflex_swapchain_info(ci);   // (NVIDIA Reflex: latency mode on)
    VkSwapchainKHR sc;
    if (dev.swapchain) fg_latency_swapchain_gone();   // (oldSwapchain: RX_VK_FG_LATENCY_LOG's waits on it end first)
    result = vkCreateSwapchainKHR(dev.device, &ci, nullptr, &sc);
    // Passing oldSwapchain retires it even when creation fails. Nothing may acquire from it again.
    retire_swapchain();
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS) {
        logf("vk: vkCreateSwapchainKHR failed (VkResult %d, extent %ux%u, format %d, usage %#x, mode %d, images %u)",
             (int)result, width, height, (int)fmt.format, ci.imageUsage, (int)mode, ci.minImageCount);
        return false;
    }
    if (vkGetSwapchainImagesKHR(dev.device, sc, &n, nullptr) != VK_SUCCESS || !n) {
        vkDestroySwapchainKHR(dev.device, sc, nullptr);
        return false;
    }
    std::vector<VkImage> images(n);
    if (vkGetSwapchainImagesKHR(dev.device, sc, &n, images.data()) != VK_SUCCESS || !n) {
        vkDestroySwapchainKHR(dev.device, sc, nullptr);
        return false;
    }
    images.resize(n);
    std::vector<VkSemaphore> rendered(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& semaphore : rendered) {
        if (vkCreateSemaphore(dev.device, &si, nullptr, &semaphore) != VK_SUCCESS) {
            for (auto made : rendered)
                if (made) vkDestroySemaphore(dev.device, made, nullptr);
            vkDestroySwapchainKHR(dev.device, sc, nullptr);
            return false;
        }
    }
    dev.swapchain = sc;
    dev.swap_rendered = std::move(rendered);
    dev.swap_format = fmt.format;
    dev.swap_extent = {width, height};
    dev.swap_images = std::move(images);
    dev.swap_spare = n > caps.minImageCount ? n - caps.minImageCount : 0;
    dev.swap_slots = slots_made;
    logf("vk: swapchain %ux%u, %u images, %s (V-Sync %s)", width, height, n,
         mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo",
         vsync == 1 ? "On" : vsync == 2 ? "Off" : "Auto");
    swap_views_make(ci.imageUsage, mutable_view ? view_formats[1] : fmt.format);
    reflex_swapchain_created(sc);   // (NVIDIA Reflex: its sleep mode on the new swapchain)
    return true;
}

namespace {

HWND main_window = nullptr;
#ifdef _WIN32
// (longshot) Windows 11 24H2's advanced colour info (DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 and the names of its numbers in
// the Windows SDK), written out for headers that lack it or keep it behind NTDDI_VERSION. tests/test_hdr_sdr_why.py holds
// the layout and the numbers against the SDK's own
struct AdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 flags, color_encoding, bits_per_color_channel, active_color_mode;
};
enum : UINT32 {
    kGetAdvancedColorInfo2 = 15,   // (DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2)
    kAdvancedColorModeHdr = 2      // (DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR; 0 SDR, 1 wide colour)
};
#endif
// (longshot) whether the system has HDR on for the window's display: 1 on, 0 off, -1 not known. Windows' own setting ("Use
// HDR"), as its display settings show it: create_swapchain's word on why the swapchain is SDR. *display is given the
// display's name (the monitor's, else Windows' "\.\DISPLAYn") once it is found, for the log. Windows 11 24H2 tells the mode
// (activeColorMode). An older one only tells "advanced colour", which is HDR or the wide colour an SDR display is given:
// off is off, on without "wide colour enforced" is HDR, and on with it is not told apart here (not known, so the old
// question and no wrong word). Elsewhere not known (a Linux compositor lists HDR10 when it has HDR on).
// Asked at each swapchain while HDR is on and the surface SDR: a few hundredths of a millisecond (tests/test_hdr_sdr_why.py
// prints it), beside a swapchain's tens, and an answer kept could outlive Windows' setting
int system_hdr_on(std::string* display) {
#ifdef _WIN32
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!main_window || !GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &mi)) return -1;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG r = ERROR_INSUFFICIENT_BUFFER;
    for (int tries = 0; r == ERROR_INSUFFICIENT_BUFFER && tries < 4; tries++) {   // (the displays can change in between)
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return -1;
        paths.resize(np), modes.resize(nm);
        r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        paths.resize(np), modes.resize(nm);
    }
    if (r != ERROR_SUCCESS) return -1;
    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof source, p.sourceInfo.adapterId, p.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, mi.szDevice)) continue;
        if (display) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof target, p.targetInfo.adapterId, p.targetInfo.id};
            const bool named = DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0];
            char utf8[256] = "";
            WideCharToMultiByte(CP_UTF8, 0, named ? target.monitorFriendlyDeviceName : mi.szDevice, -1, utf8, sizeof utf8 - 1,
                                nullptr, nullptr);
            *display = utf8;
        }
        AdvancedColorInfo2 info2{};
        info2.header = {(DISPLAYCONFIG_DEVICE_INFO_TYPE)kGetAdvancedColorInfo2, sizeof info2, p.targetInfo.adapterId,
                        p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) return info2.active_color_mode == kAdvancedColorModeHdr;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
        info.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof info, p.targetInfo.adapterId, p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS) return -1;
        return !info.advancedColorEnabled ? 0 : info.wideColorEnforced ? -1 : 1;
    }
#else
    (void)display;
#endif
    return -1;
}
// (longshot) longshot --prepare-shaders (a builder, after a build): the same work with no game, its progress as
// "@@progress" lines on stdout. A skip there leaves longshot_prepare_skipped beside the game: its own starts then
// leave the work out too (what was not prepared is built when first drawn), until a --prepare-shaders run ends it
bool prepare_only() { return env_on("LONGSHOT_PREPARE_ONLY"); }
std::string skipped_path() { return cache_path("longshot_prepare_skipped"); }
// (longshot) The --prepare-shaders run's watchdog, for any GPU and any part of it.
// - What it waits on, in the log the builder shows: every 30 s that nothing has moved, the part, its count, and whether the
//   GPU driver faulted; a driver fault as soon as it is caught. (The pipeline steps log their own progress every 10 s,
//   vk_draw.cpp prepare_progress_log; this adds the shader programs' count when the log has been quiet for 30 s), so the
//   builder's details tell a slow driver from a stuck one.
// - When it stops for good: a driver that faults inside a pipeline compile (caught by guarded_graphics: "the driver faulted
//   compiling", above) can leave that compile unfinished inside it, and builds behind it then wait for ever (on AMD, any build of
//   the same program), seen on AMD and NVIDIA. Whatever
//   the cause, a part that needs the stuck driver cannot go on. So when nothing has moved for RX_VK_PREPARE_STALL_S seconds
//   (300; 0: never stopped. Long: some steps read the caches or decide the predictions for a while with no count, on a slow
//   disk or CPU too. While a driver compile is under way and the process keeps half a CPU or more busy, the compiler at work,
//   three times that, and twelve times on Mesa's Intel driver (prepare_long_compiles): on an Iris Xe one variant of
//   abccf81b45c5eb50, a program MKW dispatches in play, was not done after 15 minutes held to 32 lanes. A compile that uses no
//   CPU is stuck on a lock, and stops at the ordinary limit on every driver),
//   the run says where it stopped and why, leaves
//   longshot_prepare_skipped so the game's own starts do not stop in the same place (what was not prepared is built when first
//   drawn), and ends at once with exit status 4 (make_game.py: "the GPU driver stopped responding"). Not through exit(): that
//   unloads the driver, which can wait on the same lock. Without this make_game.py would stop it after 15 minutes with no
//   line, and the game's first start would stop in the same place
// - The pipeline cache's save, the last part, has no count and can take minutes on a large cache. When the run went on from a
//   part and left workers inside the driver (prepare_left_workers), or the driver faulted, the save can wait on them for ever
//   (vkGetPipelineCacheData), and so can exit() unloading the driver: then the save is stopped too, after twice the parts' limit
//   (600 s), and a run that gets through it ends at once (prepare_end). Twice: a fault AMD's retry recovered from ("recovered
//   with optimization disabled") counts as one, so on such a card every run is risky, and a save that is only slow should not be
//   stopped at the parts' limit. Otherwise the save is only reported, and its lines stop after the parts' 300 s so that
//   make_game.py's 15 minutes with no line (PREPARE_SILENCE) still stop a save that never ends
/// (where the run is, for the watchdog: under one lock, so it reads a part with its own count)
struct PrepareSeen {
    int part = -1;
    size_t done = 0, total = 0;
    int64_t moved_ms = 0;
};
std::mutex prepare_seen_mu;
PrepareSeen prepare_seen;
/// A part's count, from its progress: the watchdog's clock starts again when the part or its count changes. Part 3: the save
void prepare_moved(int part, size_t done, size_t total) {
    std::lock_guard lk(prepare_seen_mu);
    if (part != prepare_seen.part || done != prepare_seen.done || !prepare_seen.moved_ms)
        prepare_seen = {part, done, total, steady_ms()};
}
/// (longshot) A driver compile that ended (vk_draw.cpp ComputeTurn): the driver answers, so the watchdog's clock starts again though
/// the part's count has not moved. The compute warm's count moves when a program's variants are all done, up to 64 of them,
/// not as each one ends: on an Iris Xe one held to 32 lanes can take minutes, and the watchdog stopped a preparation whose
/// driver was compiling all along
void prepare_compiled_seen() {
    std::lock_guard lk(prepare_seen_mu);
    if (prepare_seen.moved_ms) prepare_seen.moved_ms = steady_ms();
}
/// The run ends now: the log written out, then TerminateProcess, not exit(), which unloads a driver that can still hold workers
/// (@@done first for a run that finished: the line the game prints after the preparation, host/hos_main.c)
[[noreturn]] void prepare_end(int code) {
    if (host && host->log_flush) host->log_flush();
    if (!code) std::printf("@@done shaders\n");
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    std::_Exit(code);
}
std::atomic<int> driver_faults_seen{0};
/// (longshot) The CPU time this process has used, in ms (-1: the system does not say): a driver compile that is working keeps a CPU
/// busy, one that is stuck on a lock uses none
int64_t process_cpu_ms() {
#ifdef _WIN32
    FILETIME created, ended, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) return -1;
    const auto ms = [](const FILETIME& f) { return (int64_t)((((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10000); };
    return ms(kernel) + ms(user);
#else
    timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
#endif
}
/// (longshot) The watchdog's last look at the CPU: half a CPU or more busy over its window (prepare_compiler_busy, for
/// precompile_run's own limit)
std::atomic<bool> prepare_cpu_busy{false};
void prepare_watchdog() {
    static const int stall_s = getenv("RX_VK_PREPARE_STALL_S") ? atoi(getenv("RX_VK_PREPARE_STALL_S")) : 300;
    // (a status line after this long with no other line; RX_VK_PREPARE_STATUS_S=<s>, for the tests)
    static const int64_t quiet_s = getenv("RX_VK_PREPARE_STATUS_S") ? std::max(1, atoi(getenv("RX_VK_PREPARE_STATUS_S"))) : 30;
    prepare_moved(0, 0, 0);
    std::thread([] {
        static const char* const parts[] = {"the shader programs", "the predicted pipelines", "the noted pipelines",
                                            "the pipeline cache's save", "the FSR 4 pipelines"};
        const int64_t limit_s = stall_s > 0 ? stall_s : 300;   // (the save's lines stop after this, whatever RX_VK_PREPARE_STALL_S)
        const int save_stall_s = stall_s * 2;   // (a risky save's stop: twice the parts', above)
        const int64_t compile_stall_s = stall_s * 3;   // (a compile with a CPU at work: three times, 900 s by default)
        const int64_t long_compile_stall_s = stall_s * 12;   // (the same on Mesa's Intel driver: an hour by default)
        // (the CPU the process used over the last window, 30 s by default: half a CPU or more is a compiler at work)
        const int64_t busy_window_ms = std::clamp<int64_t>((int64_t)stall_s * 100, 1000, 30000);
        int64_t cpu_then = process_cpu_ms(), cpu_then_ms = steady_ms();
        bool cpu_busy = false;
        for (;;) {
            Sleep(1000);
            const int64_t now = steady_ms();
            if (now - cpu_then_ms >= busy_window_ms) {
                const int64_t cpu = process_cpu_ms();
                cpu_busy = cpu >= 0 && cpu_then >= 0 && (cpu - cpu_then) * 2 >= now - cpu_then_ms;
                cpu_then = cpu, cpu_then_ms = now;
                prepare_cpu_busy.store(cpu_busy, std::memory_order_relaxed);
            }
            // (whether a compile is under way, read before the progress below: a compile that ends moves the progress's time and
            // then leaves the list (vk_draw.cpp ComputeTurn), so one that ended between the two reads is in the time read here.
            // Read the other way round, a compile that ended between them left an old time and no compile under way, and a
            // healthy run was stopped at the short limit)
            const bool under_way = prepare_compiling();
            PrepareSeen at;
            {
                std::lock_guard lk(prepare_seen_mu);
                at = prepare_seen;
            }
            const char* what = at.part >= 0 && at.part < (int)std::size(parts) ? parts[at.part] : "the preparation";
            const int64_t still = (now - at.moved_ms) / 1000;
            const int faults = driver_faults();
            const bool left = prepare_left_workers();
            const bool risky = faults > 0 || left;   // (the driver may be stuck: the save too can wait for ever)
            const bool saving = at.part == 3;
            const bool compiling = !saving && cpu_busy && under_way;   // (a compile at work; one with no CPU is stuck)
            const int64_t part_stall_s = !compiling ? stall_s : prepare_long_compiles() ? long_compile_stall_s : compile_stall_s;
            // (what it waits on: after a fault in the driver, most likely the driver; else the driver or the disk, a slow one
            // reading the caches)
            const char* waiting_on = faults ? "waiting on the GPU driver, which faulted (above)"
                                     : left ? "waiting on the GPU driver, which still has pipelines it did not finish"
                                     : compiling ? "waiting on the GPU driver, which is compiling (a CPU busy)"
                                                             : "waiting on the GPU driver or the disk";
            if (faults != driver_faults_seen.exchange(faults))
                logf("vk: preparation: the GPU driver faulted %d time%s so far (above): a driver can stop responding after one, so a "
                     "stop here would be why", faults, faults == 1 ? "" : "s");
            if (stall_s > 0 && (!saving ? still >= part_stall_s : risky && still >= save_stall_s)) {
                if (!saving)
                    logf("vk: preparation: STOPPED: nothing moved for %lld s in %s (%zu of %zu): the GPU driver stopped responding%s. "
                         "The rest is built in play, when first drawn; updating the GPU driver may help", (long long)part_stall_s, what,
                         at.done, at.total, faults ? " after it faulted (above)" : "");
                else
                    logf("vk: preparation: STOPPED: %s waited %d s on the GPU driver, %s. The pipelines are made; only this "
                         "file is left out. Updating the GPU driver may help", what, save_stall_s,
                         faults ? "which faulted (above)" : "which still has pipelines it did not finish");
                if (FILE* f = fopen(skipped_path().c_str(), "wb")) fclose(f);
                prepare_end(4);
            }
            // (its own lines, at most one every quiet_s: while nothing has moved, what it waits on, whatever else the log says (the
            // pipeline steps' own lines, vk_draw.cpp prepare_progress_log, come every 10 s and repeat their count while it is
            // stuck); while the shader programs move, their count when the log has been quiet (the pipeline steps log theirs);
            // the save, below)
            static int64_t said_ms = 0;
            if (now - said_ms < quiet_s * 1000) continue;
            const bool quiet = now - last_log_ms.load(std::memory_order_relaxed) >= quiet_s * 1000;
            if (saving) {
                if (!quiet) continue;
                if (risky)
                    logf("vk: preparation: still writing %s (%lld s), %s. Stopped at %d s if it does not end", what,
                         (long long)still, waiting_on, save_stall_s);
                else if (still < limit_s)
                    logf("vk: preparation: still writing %s (%lld s): every pipeline is made, this is the disk", what,
                         (long long)still);
                else
                    continue;   // (silent from here: make_game.py's 15 minutes with no line stop a save that never ends)
            } else if (still >= quiet_s) {
                if (stall_s > 0)
                    logf("vk: preparation: waiting: nothing in %s has finished for %lld s (%zu of %zu), %s. Stopped at %lld s if "
                         "nothing moves", what, (long long)still, at.done, at.total, waiting_on, (long long)part_stall_s);
                else
                    logf("vk: preparation: waiting: nothing in %s has finished for %lld s (%zu of %zu), %s", what,
                         (long long)still, at.done, at.total, waiting_on);
            } else if (at.part == 0 && quiet) {
                logf("vk: preparation: still working on %s: %zu of %zu%s", what, at.done, at.total, start_memory_words().c_str());
            } else {
                continue;
            }
            said_ms = now;
            if (host && host->log_flush) host->log_flush();
        }
    }).detach();
}
// (longshot) The journalled run's own limit, per entry (prepare_journal.h, --prepare-entry-limit <s>): the watchdog above
// stops a run in which nothing moves and cannot say which shader the driver is stuck in, so the whole preparation was given
// up for one shader. With a journal, an entry that has been inside the driver for longer than one gets is named in the
// journal and the run ends at once with exit status 6 (prepare_journal::kExitEntryLimit), through prepare_end as the
// watchdog's stop: the supervisor (nx2/prepare_supervisor.py) starts it again without that program. Looked at once a
// second, on a thread of its own: the watchdog's is compiled alone by its check. No journal, or no limit: nothing to do
void prepare_entry_watch() {
    const char* limit = getenv("LONGSHOT_PREPARE_ENTRY_S");
    const char* journal = getenv("LONGSHOT_PREPARE_JOURNAL");
    if (!limit || atoi(limit) <= 0 || !journal || !*journal) return;
    std::thread([] {
        for (;;) {
            Sleep(1000);
            if (prepare_journal_overdue()) prepare_end(prepare_journal::kExitEntryLimit);
        }
    }).detach();
}
/// Before the parts of a --prepare-shaders run: what one the supervisor started needs. The entry watch; and, started again
/// past a shader the run before died or hung in (--prepare-from), that run taken off the steps' crash markers before the
/// steps read them (vk_draw.cpp prepare_restart_forgive). Nothing in a game run
void prepare_supervised() {
    if (!prepare_only()) return;
    prepare_entry_watch();
    prepare_restart_forgive();
}
// (longshot) The first start after an update prepares every shader (prepare_run) and the pipelines the last sessions noted
// (precompile_run): a minute and more, before the game's first frame. They run on a thread of their own while this one, the
// renderer's start, shows the window at once with what they are doing on it, and keeps it shown and sized; on Windows it is also
// the window's thread, and handles its messages meanwhile (it would not answer otherwise). Closing the window stops them
// (mkwvk_close_asked): what was made is kept, the next start goes on from there. A start without a window
// (offscreen) prepares all of it before anything else (and a console with the count on Windows)
std::atomic<int> prep_stage{0};   // 0 the programs, 1 the pipelines the notes predict (a first start), 2 the noted pipelines
std::atomic<size_t> prep_done{0}, prep_total{0};
// (the parts' guesses before they run, and the parts that do not run: the one bar's total, PrepareBar)
std::atomic<size_t> prep_guess[3]{};
std::atomic<bool> prep_skip[3]{};
std::atomic<bool> prep_running{false}, prep_finished{false};
HANDLE prep_ended = nullptr;
#ifndef _WIN32
const MkwVkWindow* prep_window = nullptr;   // (its size)
#endif
void prep_progress(size_t done, size_t total) {
    prep_done.store(done, std::memory_order_relaxed);
    prep_total.store(total, std::memory_order_relaxed);
}
/// The window's size now (0x0: minimized, or unknown)
void window_size(uint32_t* w, uint32_t* h) {
    *w = *h = 0;
#ifdef _WIN32
    RECT r{};
    if (main_window && GetClientRect(main_window, &r)) *w = (uint32_t)(r.right - r.left), *h = (uint32_t)(r.bottom - r.top);
#else
    int width = 0, height = 0;
    if (prep_window) prep_window->drawable_size(prep_window->window, &width, &height);
    if (width > 0 && height > 0) *w = (uint32_t)width, *h = (uint32_t)height;
#endif
}
void prepare_with_screen() {
    prep_ended = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    prep_running.store(true);
    std::thread worker([] {
        const bool update = prepare_is_update();   // (an update, RX_VK_PREPARE_BACKGROUND=1: what the notes name now, the rest in play)
        {
            size_t guess[3];
            prepare_guess(guess);
            for (int k = 0; k < 3; k++) prep_guess[k].store(guess[k]);
        }
        if (update) prep_skip[0].store(true), prep_skip[1].store(true);
        // (Compile shaders on Most, the default: no prediction runs, and its guess, half again the programs, is not in the total
        // while the programs are prepared. Left in, it shrank their share of the bar, and the bar jumped once they were done)
        if (!compile_all()) prep_skip[1].store(true);
        const bool prepared = !update && prepare_run(prep_progress, false);
        if (!prepared) prep_skip[0].store(true);
        // (a first start, or one after a start that did not finish them: the pipelines the notes predict, into the driver's cache)
        if ((prepared || (!update && predict_pending())) && compile_all() && !prepare_stopped()) {
            prep_done.store(0), prep_total.store(0);
            prep_stage.store(1);
            predict_run(prep_progress);
        } else {
            prep_skip[1].store(true);
        }
        if (!prepare_stopped()) {
            if (update) state_list_run();   // (the shipped list's pipelines, as a start with no screen makes them)
            prep_done.store(0), prep_total.store(0);
            prep_stage.store(2);
            precompile_run(prep_progress);
        }
        if (prepare_stopped()) pipeline_cache_save(true);   // (the driver's part built so far, for the next start)
        else if (update) prepare_background();
        prep_finished.store(true, std::memory_order_release);
        SetEvent(prep_ended);
    });
    // (the screen is drawn and presented on a thread of its own: a present can wait for as long as the window cannot be seen (FIFO
    // on Wayland, a locked screen), and that must hold neither the window's messages nor the game's start)
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    static std::atomic<bool> paint_stop{false};
    HANDLE painted = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread painter([t0, painted] {
        ProgressClock eta;   // (over the whole bar: its units, PrepareBar)
        PrepareBar bar;
        double next_log = 10;
        bool prepared = false;   // (this start prepares the programs: a first start after an update, two steps)
        uint32_t last_w = dev.swap_extent.width, last_h = dev.swap_extent.height;
        for (;;) {
            const bool finished = prep_finished.load(std::memory_order_acquire);
            const double secs = std::chrono::duration<double>(clock::now() - t0).count();
            const int stage = prep_stage.load();
            const size_t done = prep_done.load(std::memory_order_relaxed), total = prep_total.load(std::memory_order_relaxed);
            for (int k = 0; k < 3; k++) {
                bar.guess(k, prep_guess[k].load(std::memory_order_relaxed));
                if (prep_skip[k].load(std::memory_order_relaxed)) bar.skip(k);
            }
            if (total) bar.at(stage, done, total);
            else bar.reach(stage);
            if (finished) bar.finish();
            const double whole = bar.fraction();
            eta.note(secs, (size_t)(bar.units_done() * 100), (size_t)(bar.units_total() * 100));
            const double left = eta.left();
            if (secs >= next_log && total) {
                logf("vk: start-up: %s %zu of %zu, %.0f%% of all%s%s", stage == 2 ? "pipelines" : stage ? "predicted pipelines" : "shaders", done,
                     total, whole * 100, left >= 0 ? ", " : "", left_text(left).c_str());
                next_log = secs + 10;
            }
            if (stage == 0 && total) prepared = true;
            StartupState st;
            st.stage = stage, st.done = done, st.total = total, st.left = left, st.secs = secs;
            st.prepared = prepared, st.finished = finished, st.stopping = prepare_stopped();
            st.saving = prepare_saving();
            st.whole = whole;
            const StartupView view = startup_view(st);
            startup_screen(&view);
            uint32_t w, h;
            window_size(&w, &h);
            if (w && h && (w != last_w || h != last_h)) {   // (resized: the window's thread leaves it to the renderer, not started yet)
                dev.swap_resize.store((uint64_t)w << 32 | h, std::memory_order_release);
                last_w = w, last_h = h;
            }
            if (w && h) present_image(nullptr, dev.swap_extent.width, dev.swap_extent.height, nullptr);
            if (finished || paint_stop.load()) break;
            Sleep(33);
        }
        // (the end: a fade to black over 0.4 s, so the game's first frame comes out of black, not cut in over the screen; none
        // after a screen of under 1.5 s, a blink, or when the window is not shown (paint_stop: its presents wait))
        const double shown = std::chrono::duration<double>(clock::now() - t0).count();
        if (shown >= 1.5 && !paint_stop.load()) {
            const auto f0 = clock::now();
            for (;;) {
                const double f = std::chrono::duration<double>(clock::now() - f0).count() / 0.4;
                startup_fade_to((float)std::min(f, 1.0));
                uint32_t w, h;
                window_size(&w, &h);
                if (w && h) present_image(nullptr, dev.swap_extent.width, dev.swap_extent.height, nullptr);
                if (f >= 1 || paint_stop.load()) break;
                Sleep(8);
            }
        }
        SetEvent(painted);
    });
    // (this thread: the window's messages on Windows, until the work is done)
    for (;;) {
#ifdef _WIN32
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
#endif
        if (WaitForSingleObject(prep_ended, 33) == WAIT_OBJECT_0) break;
    }
    worker.join();
    // (the painter ends by itself once the work is finished, after its fade; it is told to stop only when it has not by then)
    if (WaitForSingleObject(painted, 1500) == WAIT_OBJECT_0) painter.join();
    else {
        paint_stop.store(true);   // (its present waits for the window to be seen: the game's own presents will, too)
        logf("vk: start-up: the window is not being shown; the game starts without waiting for it");
        painter.detach();
    }
    startup_screen(nullptr);
    prep_running.store(false);
    logf("vk: start-up: the shaders took %.1f s", std::chrono::duration<double>(clock::now() - t0).count());
}
bool started_up() {
    if (const char* e = getenv("RX_VK_START_DELAY")) Sleep((DWORD)atoi(e) * 1000);   // (diagnosis: a long start, as the first one is)
    if (!replay_start()) return false;
    driver_cache_log("at the start");
#ifndef _WIN32
    if (const auto& dump = restored_shader_dump(); dump.dir[0]) {
        const uint64_t stamp = program_cache_stamp();
        if (stamp != std::strtoull(dump.stamp, nullptr, 16)) {
            logf("vk: restored shader dump stamp %s differs from this run's %016llx (Robust buffers %s). "
                 "Match the original shader settings or choose a compatible dump in Setup.", dump.stamp,
                 (unsigned long long)stamp, dev.features.robustBufferAccess ? "on" : "off");
            return false;
        }
        logf("vk: using restored Mesa %s shader cache; skipping startup shader preparation", dump.mesa);
        return true;
    }
#endif
    prepare_supervised();   // (a --prepare-shaders run the Builder's supervisor started: its entry watch, the run before forgiven)
    if (prepare_only()) {
        // (three parts: the programs, the pipelines the notes predict, then those they name, on one bar whose total has all three
        // from the start: PrepareBar. The fixed shares it had (70%, 20%, 10%) kept the predicted pipelines' minutes in a fifth)
        static int part;
        static PrepareBar bar;
        prepare_watchdog();
        const auto progress = [](size_t done, size_t total) {
            static int last = -1;
            if (!total) return;
            prepare_moved(part, done, total);
            bar.at(part, done, total);
            const int at = (int)(bar.fraction() * 1000);
            if (at == last) return;
            last = at;
            printf("@@progress %.3f\n", at / 1000.0);
            fflush(stdout);
        };
        printf("@@stage shaders Preparing shaders\n");
        fflush(stdout);
        // The FSR driver may need several GB of commit before the game shader cache is loaded.
        prepare_moved(4, 0, 0);
        fsr4_prepare_shaders();
        prepare_moved(0, 0, 0);
        {
            size_t guess[3];
            prepare_guess(guess);
            for (int k = 0; k < 3; k++) bar.guess(k, guess[k]);
        }
        if (!compile_all()) bar.skip(1);   // (Most: no prediction, and no guess of it in the total while the programs run)
        part = 0;
        const bool prepared = prepare_run(progress, false, true);
        // (or the last run did not finish the prediction; Compile shaders on Most: never, compile_all)
        // An explicit All request also upgrades a folder prepared on Most. Completed All work stays skipped.
        const bool predict = compile_all() && (prepared || predict_pending() || !predicted_complete());
        if (!prepared && !bar.counted[0]) bar.skip(0);   // (an already prepared seed can still import pipeline states)
        // (no prediction: its part of the bar goes. On Most after a preparation too: left in, the step to the noted pipelines
        // counted it done at its guess, and the bar jumped by the prediction's share, about half of it)
        if (!predict) bar.skip(1);
        part = 1;
        prepare_moved(1, 0, 0);
        if (predict) predict_run(progress);
        part = 2;
        prepare_moved(2, 0, 0);
        bar.reach(2);
        precompile_run(progress);
        prepare_moved(3, 0, 0);   // (the pipeline cache written out: the watchdog reports it, and stops it only when risky)
        pipeline_cache_save(true);
        remove(skipped_path().c_str());
        if (prepare_left_workers() || driver_faults() > 0) {   // (exit() would unload a driver that may still hold workers)
            logf("vk: preparation: done; the run ends at once (%s)",
                 prepare_left_workers() ? "pipelines left in the driver" : "the driver faulted");
            prepare_end(0);
        }
        return true;
    }
    if (FILE* f = fopen(skipped_path().c_str(), "rb")) {
        fclose(f);
        logf("vk: the shaders' preparation was skipped at the build: left to when each is first drawn");
        return true;
    }
    if (precompile_wanted()) {   // (a release build, before the game: nothing to build during play)
        if (!dev.offscreen) {
            prepare_with_screen();
            return true;
        }
        // (a console while it lasts: there is no window yet, and a long wait with nothing on screen looks like a hang)
        static HANDLE console = nullptr;
        const auto progress = [](size_t done, size_t total) {
            static bool tried = false;
            if (!total) return;
            if (!tried) {
                tried = true;
                if (AllocConsole()) {
#ifdef _WIN32
                    // This console is ours. Selection must not block preparation's progress writes.
                    const HANDLE input = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                     nullptr, OPEN_EXISTING, 0, nullptr);
                    if (input != INVALID_HANDLE_VALUE) {
                        DWORD mode;
                        if (GetConsoleMode(input, &mode))
                            SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
                        CloseHandle(input);
                    }
#endif
                    SetConsoleTitleA("Longshot");
                    console = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (console == INVALID_HANDLE_VALUE) console = nullptr;
                }
            }
            {   // (in the log too, every 10 s: a run with no window has nothing else to show it)
                static DWORD next_log = 0;
                const DWORD now = GetTickCount();
                if (!next_log) next_log = now + 10000;
                if ((int32_t)(now - next_log) >= 0) {
                    logf("vk: start-up: %zu of %zu", done, total);
                    next_log = now + 10000;
                }
            }
            if (!console) return;
            char line[80];
            const int n = snprintf(line, sizeof line, "%cPreparing shaders: %zu of %zu (%zu%%)   ", 13, done, total, done * 100 / total);
            DWORD wrote = 0;
            WriteFile(console, line, (DWORD)n, &wrote, nullptr);
        };
        const bool update = prepare_is_update();   // (an update: what the notes name now, the rest in play)
        if (!update && (prepare_run(progress, false) || predict_pending()) && compile_all()) predict_run(progress);
        if (update) state_list_run();
        precompile_run(progress);
        if (update) prepare_background();
        if (console) CloseHandle(console), console = nullptr, FreeConsole();
    }
    return true;
}
// (the window's surface made: the presents' own queue kept where its family can present to it, create_device)
static void choose_present_queue() {
    dev.present_queue_supported = false;
    if (!dev.present_queue) return;
    VkBool32 ok = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.present_family, dev.surface, &ok) != VK_SUCCESS || !ok) {
        logf("vk: queue family %u cannot present to the window: presents on the replay's queue", dev.present_family);
        return;
    }
    dev.present_queue_supported = true;
    logf("vk: presents on a queue of their own, of compute family %u (the Debug tab's Present queue; RX_VK_PRESENT_QUEUE=0: off)",
         dev.present_family);
}

bool open_window_surface(bool log_failure) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleA(nullptr);
    si.hwnd = main_window;
    if (!IsWindow(main_window) || vkCreateWin32SurfaceKHR(dev.instance, &si, nullptr, &surface) != VK_SUCCESS) return false;
#else
    const MkwVkWindow* w = prep_window;
    if (!w || !w->create_surface(w->window, (void*)dev.instance, (void*)&surface)) {
        if (log_failure)
            logf("vk: SDL_Vulkan_CreateSurface failed; check SDL_VIDEODRIVER and the Vulkan window-system extensions");
        return false;
    }
#endif
    VkBool32 ok = VK_FALSE;
    const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.queue_family, surface, &ok);
    if (result != VK_SUCCESS || !ok) {
        if (log_failure) logf("vk: queue-family present support failed (VkResult %d, supported %u)", (int)result, ok);
        vkDestroySurfaceKHR(dev.instance, surface, nullptr);
        return false;
    }
    dev.surface = surface;
    choose_present_queue();
    return true;
}

/// (longshot) What this PC reports about itself, for the Quality preset Auto (this PC) (auto_settings.h, vk_menu.cpp
/// menu_auto_for_gpu): once, when the window is known (its screen's size and refresh), the card already chosen
void auto_profile() {
    static bool done;
    if (done) return;
    done = true;
    ::auto_settings::Hardware hw;
    const VkPhysicalDeviceProperties& p = dev.props;
    hw.vendor = p.vendorID == 0x10de ? ::auto_settings::NVIDIA : p.vendorID == 0x1002 ? ::auto_settings::AMD
              : p.vendorID == 0x8086 ? ::auto_settings::INTEL : ::auto_settings::OTHER;
    hw.integrated = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    for (uint32_t i = 0; i < dev.memory.memoryHeapCount; i++)
        if (dev.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            hw.vram_mb = std::max<uint32_t>(hw.vram_mb, (uint32_t)(test_vram_bytes(dev.memory.memoryHeaps[i].size) >> 20));
    hw.mesh_native = dev.mesh_capable;   // (the card's, not this run's mode: Linux AMD's Automatic is Compute)
    hw.mesh_as_compute = dev.mesh_capable && !dev.mesh_shader;
    hw.rtx = hw.vendor == ::auto_settings::NVIDIA && strstr(p.deviceName, "RTX");
    hw.rdna4 = is_rdna4(p.vendorID, p.deviceID, p.deviceName);
    // (the Steam Deck's APU, LCD and OLED, or Steam's flag on an integrated GPU: Steam sets SteamDeck=1 wherever it runs as on a
    // Deck, Gaming Mode sessions on desktops with a discrete card too, which are no handheld)
    const char* deck = getenv("SteamDeck");
    hw.handheld = (deck && atoi(deck) == 1 && hw.integrated) || strstr(p.deviceName, "Custom GPU 0405") ||
                  strstr(p.deviceName, "Custom GPU 0932");
    hw.dlss_built = dlss_built();
    hw.fsr4_built = fsr4_built();
    hw.xess_ready = xess_library_loaded();
    hw.threads = (int)std::thread::hardware_concurrency();
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) hw.ram_mb = (uint32_t)(m.ullTotalPhys >> 20);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    // (no window, the Builder's shader preparation and a headless start: the primary monitor, MonitorFromWindow(NULL, ...).
    // A 0x0 screen makes a fingerprint no options file has, so such a run would choose Auto again for no screen and write it
    // over the player's settings)
    if (GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTOPRIMARY), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        hw.screen_w = (int)dm.dmPelsWidth, hw.screen_h = (int)dm.dmPelsHeight;
        if (!main_window && dm.dmDisplayFrequency > 1) hw.refresh_hz = (int)dm.dmDisplayFrequency;   // (as display_refresh_mhz)
    }
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) hw.ram_mb = (uint32_t)(((uint64_t)pages * (uint64_t)page) >> 20);
    const MkwVkWindow* w = (const MkwVkWindow*)main_window;
    if (w && w->version >= 3 && w->desktop_size) w->desktop_size(w->window, &hw.screen_w, &hw.screen_h);
#endif
    if (main_window || !hw.refresh_hz) hw.refresh_hz = (display_refresh_mhz() + 500) / 1000;
    menu_auto_for_gpu(p, hw);
}

int start(void* hwnd) {
    main_window = (HWND)hwnd;
    {   // (which switches the run has set, and any RX_ name set that is none: switches.h)
#ifdef _WIN32
        const char* const* env = _environ;
#else
        const char* const* env = environ;
#endif
        switches::report(env, [](const char* line) { logf("%s", line); });
    }
    auto_profile();   // (the Quality preset Auto (this PC): before the game first asks its mode, which reads the options)
    if (!hwnd) {   // (HOS_HEADLESS: no window; the frames are rendered all the same, RX_VK_SHOTS writes them out)
        dev.offscreen = true;
        dev.swap_extent = {1920, 1080};
        dev.swap_format = VK_FORMAT_R8G8B8A8_UNORM;
        logf("vk: no window: presents render offscreen (%ux%u)", dev.swap_extent.width, dev.swap_extent.height);
        if (!started_up()) return 0;
        pipeline_cache_play();   // (play begins: what is made from now on goes into the play files)
        return 1;
    }
#ifdef _WIN32
    if (!open_window_surface(true)) return 0;
    RECT r{};
    GetClientRect((HWND)hwnd, &r);
    {
        // (longshot) the same guard as the SDL path below: hos_win runs the renderer on its own
        // thread, so the window's close (display.c, main thread) can be asked while the initial
        // chain is being made. The close takes present_image_mu through its retire; take it here
        // and check its flag — a window already closing gets no chain.
        std::lock_guard pi(present_image_mu);
        if (dev.closing.load(std::memory_order_acquire)) return 0;
        if (!create_swapchain((uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top))) return 0;
    }
#else
    const MkwVkWindow* w = (const MkwVkWindow*)hwnd;
    prep_window = w;
    if (w->version < 1 || w->version > MKWVK_WINDOW_VERSION) {   // (1: no refresh_mhz)
        logf("vk: unsupported SDL window bridge version %u (expected %u)", w->version, MKWVK_WINDOW_VERSION);
        return 0;
    }
    if (!open_window_surface(true)) return 0;
    int width = 0, height = 0;
    w->drawable_size(w->window, &width, &height);
    if (width <= 0 || height <= 0) {
        logf("vk: SDL returned zero drawable size %dx%d", width, height);
        return 0;
    }
    {
        // (longshot) the close (mkwvk_close_asked) can be asked while the renderer is still coming
        // up: SDL runs it on the window's thread. present_image's guard covers presents, but the
        // initial chain is made here, outside any present — so take the same lock the close takes
        // and check its flag, the same order present_image uses (present_image_mu, then present_mu
        // inside). A window already closing gets no chain: the host ends the process next either way.
        std::lock_guard pi(present_image_mu);
        if (dev.closing.load(std::memory_order_acquire)) return 0;
        if (!create_swapchain((uint32_t)width, (uint32_t)height)) {
            logf("vk: swapchain creation failed for drawable %dx%d", width, height);
            return 0;
        }
    }
#endif
    if (!started_up()) {
        logf("vk: renderer startup failed after window and swapchain initialization");
        return 0;
    }
    pipeline_cache_play();   // (play begins: what is made from now on goes into the play files)
    return 1;
}

// (the window's thread: only noted. The swapchain is remade by whoever presents next, in present_image: made here, it went from under a
// frame between its acquire and its submit, and the frame's commands named images of the swapchain just destroyed: a lost device)
void resize(int width, int height) {
    if (dev.offscreen || width <= 0 || height <= 0) return;
    dev.swap_resize.store((uint64_t)(uint32_t)width << 32 | (uint32_t)height, std::memory_order_release);
}

const char* describe() {
    static std::string d;
    d = std::string(dev.props.deviceName) + " (Vulkan, native)";
    return d.c_str();
}

MkwVkRenderer table;

} // namespace
/// (longshot) The shaders are being prepared (the start-up screen): the performance summary's "map" then (perf_report.cpp)
bool shaders_preparing() { return prep_running.load(std::memory_order_relaxed); }
/// (vk_internal.h: the driver compile that ended, for the watchdog above)
void prepare_compiled() { prepare_compiled_seen(); }
/// (vk_internal.h: the CPU at work, as the watchdog above last saw it; false where no watchdog runs, a game start)
bool prepare_compiler_busy() { return prepare_cpu_busy.load(std::memory_order_relaxed); }
} // namespace mkwvk

// (longshot) The window is being closed (host/display.c, host/posix/display_sdl.c, before they end the process), or the game quit
// (the Insert menu's Quit and Restart now, host/control.c's quit, which end it the same way). First the files that count the
// starts that crashed in a step are put back as this start found them (crash_markers.h): a quit is not a crash. Then, while the
// shaders are prepared, their workers are stopped and what they made is kept (10 s at most: a driver can take longer on one
// pipeline). Looked up by name, so an executable and a back end of different versions still close
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_close_asked(void) {
    using namespace mkwvk;
    // (before anything that takes time: the caches' saves below and the 10 s for the workers, with the window not answering. A
    // player who quits again then ends the process (Force Quit, End task), and so does the system at a log-out: that end comes
    // after a quit, and must not count as a crash either)
    crash_markers::close();
    programs_census_log("close");
    perf_report_final(-1);    // (the performance summary's last: ExitProcess is _exit on Linux, so no atexit runs there)
    descriptor_arrays_log("close");   // (the indexed descriptor arrays of the programs translated or read: vk_programs.cpp)
    module_ids_flush();
    page_table_census_log("close");   // (the guest pools the page table cannot reach: how many came, vk_memory.cpp)
    page_table_check_close();         // (RX_VK_GP_TABLE: its bindings covered, and with =2 the loads checked, vk_draw.cpp)
    import_stats_log("close");        // (guest-memory imports made, refused by the driver, held after a refusal: vk_memory.cpp)
    fsr4_close();             // (AMD FSR 4's tuned layers: their background builds stopped first, what they made then saved below)
    pipeline_cache_close();   // (in play: the pipelines made since the last save, kept for the next start)
    if (prep_running.load() && !prepare_stopped()) {
        logf("vk: the window was closed while the shaders were prepared: stopping, what is done is kept");
        prepare_stop();
        if (WaitForSingleObject(prep_ended, 10000) != WAIT_OBJECT_0) logf("vk: the preparation did not stop within 10 s: closing anyway");
    }
    dlss_fg_close();   // (NVIDIA DLSS Frame Generation through NGX: its feature let go before the process exits, vk_dlss.cpp)
    // (longshot) let go of the window's swapchain and everything it pins while the process is still here. The compositor
    // (gamescope on the Deck, any Wayland/X server) reprograms the display right as the window goes away, and on the Deck an
    // atomic commit there failed with ENOMEM after a long run had this process holding most of the system's memory: the
    // commit's own allocation failed, the modeset failed, and the player saw the emulator's last frames as garbage over the
    // Game Mode UI until the display was rebuilt by a mode switch. Freeing the swapchain's images (the VRAM they hold) and
    // idling the device before we exit is that headroom. Deliberate, not a crash path: dev.closing above tells every
    // presenter not to make a chain again; the lost-surface path stays for when the compositor itself dies. The presenter
    // thread is drained first and present_image_mu is held through the retire (the same lock order present_image itself
    // uses: present_image_mu, then present_mu), so no present is mid-acquire on an image being destroyed.
    dev.closing.store(true, std::memory_order_release);
    // (longshot) a present can hold this mutex for as long as its fences run (the lost-window
    // acquire waits UINT64_MAX under it): the close must not hang behind one and stop the window
    // from closing at all. Take the mutex with a few seconds of patience; no mutex, or a presenter
    // thread that does not land its hand-offed present, leaves the chain to the process exit —
    // which is exactly what happened before this block existed — rather than destroy under a
    // present that is still running (present_commit's swapchain check guards the stale one, not
    // one mid-flight).
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);   // (one patience budget for both locks)
    std::unique_lock pi(present_image_mu, std::try_to_lock);   // (vk_replay.cpp: every present, direct and frame generated, is under this)
    for (int waited = 0; !pi.owns_lock() && waited < 5000; waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::unique_lock attempt(present_image_mu, std::try_to_lock);
        if (attempt.owns_lock()) pi = std::move(attempt);
    }
    if (!pi.owns_lock() || !present_thread_drain()) {
        logf("vk: the window is closing with a present still running: the swapchain goes with the process, as it always did");
        return;
    }
    // (longshot) and the queue's lock on the same patience: queue_flush holds dev.present_mu across
    // vkQueueSubmit2 (vk_replay.cpp), so a submit the driver stalled leaves the image mutex free and the
    // presenter idle: both checks above pass and an unbounded wait here would hold the window thread
    // open instead of letting the window close. No lock: the chain goes with the process, as the two
    // checks above already do (the DLSS close waits for this same mutex the same way).
    std::unique_lock qk(dev.present_mu, std::defer_lock);
    while (!qk.try_lock() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!qk.owns_lock()) {
        logf("vk: the window is closing with the queue busy: the swapchain goes with the process, as it always did");
        return;
    }
    const bool had = dev.swapchain != VK_NULL_HANDLE;   // (a window that never presented: nothing was ever made)
    retire_swapchain();   // (device_wait_idle_locked, the rendered semaphores, the chain: vk_main.cpp above)
    if (had) logf("vk: the window is closing: the swapchain is let go with its images (what the compositor needs back for its final frames)");
}

static MkwVkHost host_storage;

static const MkwVkRenderer* init_body(const MkwVkHost* h);
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const MkwVkRenderer* mkwvk_init(const MkwVkHost* h) {
    try {
        return init_body(h);
    } catch (const std::exception& e) {
        mkwvk::cxx_escape::escaped("mkwvk_init", e);   // (said, then the game stops; before the hooks are set, on stderr)
    }
}
/// mkwvk_init's work, behind its catch
static const MkwVkRenderer* init_body(const MkwVkHost* h) {
    using namespace mkwvk;
    if (!h) return nullptr;
    if (h->version < 14 || h->version > MKWVK_API_VERSION) {
        std::fprintf(stderr, "vk: host version mismatch: host has %u, expected %u (or >= 14)\n", h->version, MKWVK_API_VERSION);
        return nullptr;
    }
    std::memset(&host_storage, 0, sizeof(host_storage));
    const size_t copy_size =
        h->version < 15 ? offsetof(MkwVkHost, slept_ns) :
        h->version < 17 ? offsetof(MkwVkHost, prompts_live) :
        h->version < 20 ? offsetof(MkwVkHost, screen_size) :
        h->version < 22 ? offsetof(MkwVkHost, take_text) :
        h->version < 23 ? offsetof(MkwVkHost, options_path) :
        h->version < 24 ? offsetof(MkwVkHost, audio_output_count) :
        h->version < 25 ? offsetof(MkwVkHost, mic_test) :
        h->version < 26 ? offsetof(MkwVkHost, perf_info) :
        h->version < 27 ? offsetof(MkwVkHost, main_thread_times) :
        h->version < 28 ? offsetof(MkwVkHost, out_of_memory) :
        h->version < 29 ? offsetof(MkwVkHost, monitor_count) :
        h->version < 30 ? offsetof(MkwVkHost, perf_tracking) :
        h->version < 32 ? offsetof(MkwVkHost, rumble_test) :
        h->version < 33 ? offsetof(MkwVkHost, nfc_count) :
        h->version < 34 ? offsetof(MkwVkHost, ab_frame) :
        h->version < 35 ? offsetof(MkwVkHost, mod_info) :
        h->version < 36 ? offsetof(MkwVkHost, ab_value) :
        h->version < 41 ? offsetof(MkwVkHost, output_preview_request) :
        h->version < 45 ? offsetof(MkwVkHost, vulkan_loader_proc) :
        sizeof(MkwVkHost);
    std::memcpy(&host_storage, h, std::min(sizeof(host_storage), copy_size));
    host = &host_storage;
    std::set_new_handler(new_refused);   // (a refused C++ allocation waits, then says out of memory)
    // (a C++ exception out of the renderer: said with its type and what(), then out of memory or the FATAL line: cxx_escape.h)
    cxx_escape::g_hooks.log = [](const char* line) { logf("%s", line); };
    cxx_escape::g_hooks.out_of_memory = [](const char* what) {
        if (host && host->version >= 28 && host->out_of_memory) host->out_of_memory(what);   // (does not return)
    };
    cxx_escape::g_hooks.fatal = [](const char* why) {   // (as vk_replay.cpp's replay_fatal: the crash window reads its "vk: FATAL:")
        logf("vk: FATAL: %s", why);
        if (host && host->log_flush) host->log_flush();
        std::fprintf(stderr, "vk: FATAL: %s\n", why);
        std::fflush(stderr);
        std::_Exit(3);
    };
    if (!load_global()) {
#ifdef _WIN32
        logf("vk: load_global failed (Vulkan loader library or vkCreateInstance missing)");
#else   // (linux: why libvulkan.so.1 did not load, kept by LoadLibraryA, which read the one message; "" if it loaded)
        const char* why = hos_load_error();
        logf("vk: load_global failed (Vulkan loader library or vkCreateInstance missing)%s%s", *why ? ": " : "", why);
#endif
        return nullptr;
    }
    if (!create_instance()) {
        logf("vk: create_instance failed");
        return nullptr;
    }
    if (!pick_device()) {
        logf("vk: pick_device failed");
        return nullptr;
    }
#ifndef _WIN32
    if (const auto& dump = restored_shader_dump(); dump.dir[0]) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &driver;
        vkGetPhysicalDeviceProperties2(dev.physical, &props);
        char uuid[2 * VK_UUID_SIZE + 1]{};
        for (size_t i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", (unsigned)dev.props.pipelineCacheUUID[i]);
        if (dump.vendor != dev.props.vendorID || dump.device != dev.props.deviceID || dump.driver != (unsigned)driver.driverID ||
            dump.version != dev.props.driverVersion || std::strcmp(dump.uuid, uuid)) {
            logf("vk: restored shader dump needs Turnip Mesa %s (%04x:%04x, cache %s). Detected %s (%04x:%04x, cache %s). "
                 "Select the matching driver and relaunch.", dump.mesa, dump.vendor, dump.device, dump.uuid, driver_text().c_str(),
                 dev.props.vendorID, dev.props.deviceID, uuid);
            return nullptr;
        }
    }
#endif
    if (!create_device()) {
        logf("vk: create_device failed");
        return nullptr;
    }
    logf("vk: %s, Vulkan %u.%u, driver %#x; mesh shaders %s, host memory import %s, pipeline libraries %s",
         dev.props.deviceName, VK_VERSION_MAJOR(dev.props.apiVersion), VK_VERSION_MINOR(dev.props.apiVersion),
         dev.props.driverVersion, mesh_note, dev.external_memory_host ? "yes" : "no",
         dev.graphics_pipeline_library ? "yes" : "no");
    logf("vk: driver: %s", driver_text().c_str());   // (the builders' Save my pipeline list reads it: nx2/pipeline_share.py)
    // (what this PC is, what it gets for it, and which of those a switch set: device_policy.h)
    device_policy_log(dev.policy, dev.facts, [](const char* line) { logf("%s", line); });
    // (the crash report's facts, as the bug report words them: host/hos_crash_dialog.h reads the last of each)
    logf("report: gpu %s", device_report_text().c_str());
    logf("report: cpu %s", mkwvk::bug_report::cpu_text().c_str());
    logf("report: os %s", mkwvk::bug_report::os_text().c_str());
    if (!dev.mesh_shader && !dev.mesh_fallback) {
        // (the game draws its foliage, grass and many effects with mesh shaders: without them those draws are left out and the
        // picture is broken ("static"-like scenes). Said once, on its own thread, and the game goes on.)
        // (a GPU with no buffer device addresses has no fallback to run either)
        logf("vk: this GPU has no mesh shaders: the game's foliage, grass and many effects cannot be drawn");
#ifdef _WIN32
        std::thread([name = std::string(dev.props.deviceName)] {
            const std::string text = "Your graphics card (" + name + ") or its driver has no mesh shader support, which Ocarina of Time "
                "uses for foliage, grass and many effects, so parts of the picture will be missing or broken.\n\nLongshot needs a GPU "
                "with mesh shaders: NVIDIA GeForce RTX 20 series or newer, AMD Radeon RX 6000 series or newer, or Intel Arc. If your "
                "card is one of those, update its driver.";
            MessageBoxA(nullptr, text.c_str(), "Longshot: graphics card", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }).detach();
#endif
    }
    table.version = h->version;
    table.start = start;
    table.resize = resize;
    table.describe = describe;
    fill_replay_table(table);
    guard_renderer(table);   // (each function's exceptions said and stopped on, not left to the host's C: renderer_guard.h)
    return &table;
}

namespace mkwvk {
HWND app_window() { return main_window; }   // (longshot: the Insert menu's Fullscreen row)

/// (longshot) The refresh rate of the display the window is on, in mHz; 0 when not known (frame generation's Auto). Asked
/// again once a second at most: the window may move to another display, and Windows' answer costs a few system calls
int display_refresh_mhz() {
    static std::atomic<int> cached{0};
    static std::atomic<int64_t> asked_at{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last = asked_at.load(std::memory_order_relaxed);
    if (last && now - last < 1000) return cached.load(std::memory_order_relaxed);
    asked_at.store(now, std::memory_order_relaxed);
    int mhz = 0;
#ifdef _WIN32
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (main_window && GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)   // (0 and 1: the default)
        // (whole Hz: a 59.94 Hz mode often says 59, and Auto would then make a frame too few a second. One under a
        // multiple of 6 (24, 30, 60, 120, 144, 240 Hz) is taken as that rate's /1.001)
        mhz = (dm.dmDisplayFrequency + 1) % 6 == 0 ? (int)((dm.dmDisplayFrequency + 1) * 1000000ull / 1001)
                                                    : (int)dm.dmDisplayFrequency * 1000;
#else
    const MkwVkWindow* w = (const MkwVkWindow*)main_window;
    if (w && w->version >= 2 && w->refresh_mhz) mhz = w->refresh_mhz(w->window);
#endif
    cached.store(mhz, std::memory_order_relaxed);
    return mhz;
}
} // namespace mkwvk
