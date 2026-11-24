"""Compile actual swapchain lifecycle/selection statements with CPU Vulkan stubs.
Usage: python -I -B tests/test_present_semaphores.py [--cxx clang++]
Checks per-image ownership and recovery from recreation failures. No GPU.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/longshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "longshot_env.h")
RUN_ALL_ARGS = ["--cxx", "{cxx}"]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
p.add_argument('--cxx', default='clang++')
a = p.parse_args()
source = a.source.resolve()
main = (source/'gpu/vk/vk_main.cpp').read_text()
replay = (source/'gpu/vk/vk_replay.cpp').read_text()
start = replay.index('void present_image_with_trial(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view, bool source_srgb, bool allow_scale, OutputTrialSource trial) {')
# Read the complete top-level function; capture dependencies can grow its text.
present = replay[start:replay.index('\n}', start) + 2]
# (present ahead, #1702: the present itself is present_image's end, present_out, which present_commit also runs; its wait
# is read from there, the complete function as above)
out_start = replay.index('static bool present_out(uint32_t index, std::unique_lock<decltype(dev.present_mu)>& qk, OutputTrialSource trial, bool preview_path) {')
present_out = replay[out_start:replay.index('\n}', out_start) + 2]
signal = re.search(r'    signal\.semaphore = [^;]+;', present)[0]
wait = re.search(r'    pi\.pWaitSemaphores = [^;]+;', present_out)[0]
# The acquire still supplies a CPU fence, including when the NVIDIA acquire also needs a semaphore.
assert 'acquire_timeout, acquire_semaphore, acquire_fence, &index)' in present
# Include the actual creation attempt: passing oldSwapchain retires it even on failure.
start = main.index('    ci.clipped = VK_TRUE;')
end = main.index('    logf("vk: swapchain', start)
tail = main[start:end]
retire_start = main.index('void retire_swapchain()')
retire = main[retire_start:main.index('\n}', retire_start) + 2]
loss_start = main.index('bool invalidate_lost_surface(')
loss = main[loss_start:main.index('\n}', loss_start) + 2]
acquire_loss_start = present.index('    if (res == VK_ERROR_SURFACE_LOST_KHR)')
acquire_loss = present[acquire_loss_start:present.index('\n#ifndef _WIN32', acquire_loss_start)]
present_loss_start = present_out.index('    if (res == VK_ERROR_SURFACE_LOST_KHR)')
present_loss = present_out[present_loss_start:present_out.index('\n    present_finish(res, false);', present_loss_start)]   # (the frame log and the failure line follow, in present_finish)
# (longshot) the close's own wiring, verbatim from mkwvk_close_asked: the flag, the presenter
# drain, the retire under the close's locks. Without this extraction the test checks the
# mechanics only: deleting the close's statements from the source would stay invisible.
close_start = main.index('    dev.closing.store(true, std::memory_order_release);')
# (up to the function's own closing brace at column zero, without taking it: the close body has
# 4-space closing braces of its own now — the patience loop — and the old 'newline + 4 spaces'
# anchor would have stopped in the middle of them)
close_end = main.index('\n}', close_start)
close_seq = main[close_start:close_end]
# (longshot) and the same for the initial chain: start() takes the close's lock and checks its
# flag before the first create_swapchain, so a window closing during startup gets no chain.
SDL_LOCKED = '        std::lock_guard pi(present_image_mu);\n        if (dev.closing.load(std::memory_order_acquire)) return 0;\n        if (!create_swapchain((uint32_t)width, (uint32_t)height)) {'
guard_start = main.index(SDL_LOCKED)
guard_end = main.index('\n        }', guard_start) + len('\n        }')
start_guard_seq = main[guard_start:guard_end]
# (longshot) the Windows path carries the same guard (hos_win runs the renderer on its own
# thread: the same close race exists there): pinned by its own anchor text.
WIN_LOCKED = '        std::lock_guard pi(present_image_mu);\n        if (dev.closing.load(std::memory_order_acquire)) return 0;\n        if (!create_swapchain((uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top))) return 0;'
assert WIN_LOCKED in main, "start(): the Windows initial chain is not under the close's lock and flag"
assert SDL_LOCKED in main, "start(): the SDL initial chain is not under the close's lock and flag"
# (longshot) and the create_swapchain guard: the real function answers not-made while a present is
# in the making, instead of retiring the chain under the presenter thread (which copies the image
# index at the hand-off and reads dev.swap_rendered[index] after its waits). The fixture's stub
# stands in for the rest of the function, so this line pins the guard itself.
assert 'bool create_swapchain(uint32_t width, uint32_t height) {\n    // (longshot, review)' in main and \
       '    if (!swapchain_made_ready()) return false;' in main.split('bool create_swapchain(uint32_t width, uint32_t height) {')[1][:2000], \
       "create_swapchain must answer not-made while a present is in the making"
assert 'if (!swapchain_made_ready()) {' in main.split('bool invalidate_lost_surface(')[1][:700], \
       "the lost-surface path must not let the chain go under a present in the making"
commit_start = replay.index('bool present_commit(const PendingPresent& p)')
commit = replay[commit_start:replay.index('\n}', commit_start) + 2]
begin = present[present.index('{') + 1:present.index('    if (src) {')]
reflex = (source/'gpu/vk/vk_reflex.cpp').read_text()
reset_start = reflex.index('void reflex_swapchain_destroyed(') if 'void reflex_swapchain_destroyed(' in reflex else -1
reset = reflex[reset_start:reflex.index('\n}', reset_start)+2] if reset_start >= 0 else 'void reflex_swapchain_destroyed() {}'
fixture = r'''
#include <vector>
#include <set>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
using VkSemaphore=uint64_t; using VkImage=uint64_t; using VkSwapchainKHR=uint64_t; using VkResult=int;
constexpr uint64_t VK_NULL_HANDLE=0;
constexpr int VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO=1, VK_SUCCESS=0, VK_TRUE=1;
constexpr VkResult VK_ERROR_SURFACE_LOST_KHR = -1000000000;
constexpr int VK_PRESENT_MODE_MAILBOX_KHR=1, VK_PRESENT_MODE_IMMEDIATE_KHR=2;
struct VkSemaphoreCreateInfo { int type; };
struct Extent { uint32_t width,height; };
struct Device { int device=1;uint64_t swapchain=0;int swap_format=0;Extent swap_extent{};bool swap_mutable=false;std::atomic<uint32_t> swap_spare{0};std::atomic<uint32_t> swap_slots{3};
    std::vector<VkImage> swap_images;std::vector<VkSemaphore> swap_rendered;
    struct { uint32_t vendorID=0; } props;
    std::atomic<uint64_t> swap_resize{0};std::mutex present_mu;uint64_t swapchain_generation=0;
    std::atomic<bool> closing{false};   // (vk_internal.h: the close retired the chain; present_image must not make another)
    int instance=1;uint64_t surface=1;bool present_queue_supported=true; } dev;
struct { int hdr=0, framegen=0; } opts;   // (the Insert menu's HDR and Frame generation rows: present_image remakes the swapchain when they change)
unsigned image_count=4, made=0, fail_at=0;uint64_t serial=1000;
int query_result=0,data_result=0;uint64_t destroyed_swapchain=0;
bool idle=false, bad_destroy=false; std::set<uint64_t> live,used;
std::set<uint64_t> chains, retired;
unsigned surfaces_destroyed=0;
uint64_t next_chain=0;int create_result=0;bool bad_chain=false;
struct CreateInfo {uint64_t oldSwapchain=0;unsigned imageUsage=1,minImageCount=4,flags=0,clipped=0;const void* pNext=nullptr;};
bool require_retired_before_create=false, created_too_early=false, views_live=false;
// (longshot: the overlays' mutable sRGB swapchain, vk_main.cpp: its format list, never used by these formats)
using VkFormat=int;
constexpr int VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO=2, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT=16,
    VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR=4;
struct VkImageFormatListCreateInfo {int sType;const void* pNext=nullptr;uint32_t viewFormatCount=0;const VkFormat* pViewFormats=nullptr;};
VkFormat unorm_of(VkFormat f) {return f;}
void reflex_swapchain_info(CreateInfo&) {}
struct Reflex {uint64_t swapchain=0,swapchain_generation=0;bool latency=false,frame_marked=false;int mode=-1;std::mutex mu;} cached;
Reflex& rx(){return cached;}
void reflex_swapchain_created(uint64_t sc) {cached.swapchain=sc;cached.latency=cached.frame_marked=true;cached.mode=1;}
''' + reset + r'''
template<class... T> void logf(const char*,T...) {}
namespace rec { struct Settled {}; }   // (vk_replay.cpp's present_commit names the recorder's type; the fixture stands in)
bool drain_says=true;   // (the source's present_thread_drain returns whether the presenter settled; the fixture answers)
bool present_thread_drain() { return drain_says; }   // (RX_VK_PRESENT_THREAD off: nothing handed over)
bool swapchain_made_ready() { return present_thread_drain(); }   // (vk_replay.cpp: the same rule, factored; the fixture answers through the drain)
void fg_latency_swapchain_gone() {}   // (RX_VK_FG_LATENCY_LOG unset: no waits for presents)
std::atomic<bool> presenter_surface_lost{false};   // (and nothing found lost by it)
std::atomic<bool> output_preview_path{false};
int vkCreateSwapchainKHR(int,const CreateInfo* ci,void*,uint64_t* out) {
    if (require_retired_before_create && (ci->oldSwapchain || !chains.empty() || !live.empty() || views_live || !idle))
        created_too_early=true;
    if(ci->oldSwapchain) {
        if(!chains.count(ci->oldSwapchain) || retired.count(ci->oldSwapchain))bad_chain=true;
        retired.insert(ci->oldSwapchain); // Vulkan retires oldSwapchain even on creation failure.
    }
    if(create_result)return create_result;
    *out=++next_chain;chains.insert(*out);return 0;
}
int vkGetSwapchainImagesKHR(int,uint64_t,uint32_t* n,uint64_t* out) {
    *n=image_count;if(out)for(unsigned i=0;i<*n;++i)out[i]=i;return out?data_result:query_result;
}
int vkCreateSemaphore(int,const VkSemaphoreCreateInfo*,void*,uint64_t* out) {
    if(++made==fail_at)return -1;*out=++serial;live.insert(*out);return 0;
}
void vkDestroySemaphore(int,uint64_t s,void*) {
    if((used.count(s) && !idle) || !live.erase(s))bad_destroy=true;
}
void vkDestroySwapchainKHR(int,uint64_t sc,void*) {
    if(cached.swapchain==sc || (sc==dev.swapchain && views_live))bad_chain=true;
    if(!chains.erase(sc))bad_chain=true;
    retired.erase(sc);destroyed_swapchain=sc;
}
void queue_flush_locked() {}
void vkDeviceWaitIdle(int) {idle=true;}
// (vk_replay.cpp's helper: held submits out, then the device idle; the real one also takes the present queue's own lock)
void device_wait_idle_locked() {queue_flush_locked();vkDeviceWaitIdle(dev.device);}
void swap_views_drop() {views_live=false;}
void vkDestroySurfaceKHR(int, uint64_t surface, void*) {
    if (!surface || !chains.empty() || !live.empty() || !idle) bad_destroy=true;
    ++surfaces_destroyed;
}
''' + retire + '\n' + loss + r'''
bool recreate() {
    uint32_t n=0,width=640,height=480; struct {int format=1;} fmt;
    struct {uint32_t minImageCount=2;} caps;   // (the surface's: present ahead holds images past it, vk_framegen.cpp)
    CreateInfo ci;int mode=0;VkResult result;
    const uint32_t slots_made=6;   // (vk_main.cpp: present_slots_for's count for the chain, computed before the cut)
''' + tail + r'''
    views_live=true;
    reflex_swapchain_created(sc);
    return true;
}
uint64_t pack(unsigned w,unsigned h) {return (uint64_t(w)<<32)|h;}
unsigned called_width=0,called_height=0,acquires=0;
bool preflight_fail=false,stale=false;uint64_t newer_resize=0;
bool create_swapchain(unsigned width,unsigned height) {
    called_width=width;called_height=height;
    if(!swapchain_made_ready())return false;   // (the real source's first statement, pinned by the assert above)
    if(newer_resize)dev.swap_resize.store(newer_resize);
    if(preflight_fail)return false;
    return recreate();
}
bool reflex_swapchain_stale() {return stale;}
int menu_vsync() {return 0;}   // (vk_menu.cpp: the V-Sync row, Auto)
int swapchain_framegen() {return 0;}   // (vk_main.cpp: frame generation's multiplier as the swapchain was made, none)
int framegen_now() {return 0;}   // (vk_framegen.cpp: frame generation's setting as it runs, Off)
bool swapchain_dlssg() {return false;}   // (vk_main.cpp: and DLSS-G makes its sets: no)
std::mutex present_image_mu;
// (longshot) the close's wiring and start's guard, verbatim from the sources: the fixture runs
// them as the sources write them. Delete either block in vk_main.cpp and the extraction above
// stops finding it; change its behaviour and these calls fail.
void close_now() {
''' + close_seq + r'''
}
bool startup_guard(unsigned width, unsigned height) {
''' + start_guard_seq + r'''
    return true;
}
struct PendingPresent {
    bool used;
    uint32_t index;
    uint64_t swapchain, swapchain_generation;
    uint64_t reflex_frame = ~0ull;   // (NVIDIA Reflex: the frame it shows; vk_reflex.h reflex_present_frame)
    uint64_t antilag_frame = ~0ull;  // (AMD Anti-Lag 2: the same; vk_antilag.h antilag_present_frame)
};
thread_local uint64_t reflex_present_frame = ~0ull;   // (vk_reflex.cpp: the frame the present shows, kept around present_out)
thread_local uint64_t antilag_present_frame = ~0ull;  // (vk_antilag.cpp: the same for AMD Anti-Lag 2)
PendingPresent* present_defer_into=nullptr;
unsigned committed=0, after_acquire=0;
bool present_out(uint32_t, std::unique_lock<std::mutex>&) {++committed; return true;}
''' + commit + r'''
void acquire_with_result(VkResult res) {
''' + acquire_loss + r'''
    ++after_acquire;
}
bool present_with_result(VkResult res, bool dedicated) {
    std::unique_lock qk(dev.present_mu);
    if (dedicated) qk.unlock();
''' + present_loss + r'''
    return true;
}
void present_begin() {
''' + begin + r'''
    if(!dev.swapchain || !chains.count(dev.swapchain) || retired.count(dev.swapchain))bad_chain=true;
    ++acquires; // The real prefix must return before the frame fence/acquire on failure.
}
uint64_t choose(unsigned index,unsigned frame_slot) {
    (void)frame_slot;struct{uint64_t semaphore;}signal{};
    struct{const uint64_t* pWaitSemaphores;}pi{};
''' + signal + '\n' + wait + r'''
    if(signal.semaphore!=*pi.pWaitSemaphores)return 0;
    used.insert(signal.semaphore);
    return signal.semaphore;
}
int main() {
    if(!recreate())return 1;
#ifdef _WIN32
    dev.props.vendorID=0x10de;
    require_retired_before_create=true;
    idle=false;
    if (!recreate() || created_too_early || bad_chain || bad_destroy) {
        std::puts("FAIL: NVIDIA replacement entered the create hook before retiring the old swapchain");
        return 52;
    }
    dev.props.vendorID=0;
    require_retired_before_create=false;
#endif
    uint64_t pending[16]{};unsigned owner[16]{},count=0;
    // Only reacquiring image i releases its previous presentation wait.
    unsigned images[]={0,1,2,3,0,1,2,3,3,1,0,2};
    for(unsigned step=0;step<12;++step) {
        unsigned image=images[step];
        for(unsigned j=0;j<count;++j)if(owner[j]==image)pending[j]=0;
        auto sem=choose(image,step%3);if(!sem)return 2;
        for(unsigned j=0;j<count;++j)if(pending[j]==sem) {
            std::printf("FAIL: slot %u reused semaphore for image %u while image %u still owns it\n",step%3,image,owner[j]);return 3;
        }
        pending[count]=sem;owner[count++]=image;
    }
    if(dev.swap_rendered.size()!=4 || live.size()!=4)return 4;
    if(dev.swap_spare!=2) {std::puts("FAIL: 4 images over a minimum of 2 should leave 2 to hold at once");return 29;}
    auto old=dev.swap_rendered;image_count=2;idle=false;
    if(!recreate() || bad_destroy || dev.swap_rendered.size()!=2 || live.size()!=2)return 5;
    if(dev.swap_spare!=0) {std::puts("FAIL: 2 images over a minimum of 2 leave none to hold at once");return 30;}
    for(auto s:old)if(live.count(s))return 6;
    for(unsigned slot=0;slot<3;++slot)for(unsigned image=0;image<2;++image)
        if(choose(image,slot)!=dev.swap_rendered[image])return 9;
    image_count=5;idle=false;
    if(!recreate() || bad_destroy || dev.swap_rendered.size()!=5 || live.size()!=5)return 10;
    if(dev.swap_spare!=3)return 31;
    if(dev.swap_slots!=6) {std::puts("FAIL: a chain made over an old one lost its frame slots (retire_swapchain's 3)");return 34;}
    for(unsigned image=0;image<5;++image)if(choose(image,image%3)!=dev.swap_rendered[image])return 11;
    // Once creation has been attempted, the old chain cannot be acquired or used as oldSwapchain again.
    image_count=4;made=0;fail_at=3;
    idle=false;
    if(recreate() || dev.swapchain || !dev.swap_rendered.empty() || !live.empty() || !chains.empty()) {
        std::puts("FAIL: retired swapchain remains published after semaphore allocation failure");return 7;
    }
    fail_at=0;made=0;query_result=-1;destroyed_swapchain=0;
    if(recreate() || dev.swapchain || made || !live.empty() || !chains.empty())return 12;
    query_result=0;image_count=0;destroyed_swapchain=0;
    if(recreate() || dev.swapchain || made || !live.empty() || !chains.empty())return 13;
    image_count=4;
    if(!recreate())return 14;
    create_result=-1;idle=false;
    if(recreate() || dev.swapchain || !live.empty() || !chains.empty()) {
        std::puts("FAIL: retired swapchain remains published after creation failure");return 15;
    }
    if(dev.swap_spare) {std::puts("FAIL: images to spare still counted with no swapchain");return 32;}
    if(dev.swap_slots!=3) {std::puts("FAIL: frame slots of a swapchain that is gone still counted");return 33;}
    create_result=0;
    if(!recreate() || !dev.swapchain || dev.swap_images.size()!=4)return 16;
    data_result=1; // VK_INCOMPLETE from the image-data query must not publish a partial chain.
    if(recreate() || dev.swapchain || !live.empty() || !chains.empty())return 17;
    data_result=0;
    if(!recreate())return 18;
    dev.swap_resize.store(pack(1720,720));preflight_fail=true;
    count=acquires;auto active=dev.swapchain;
    present_begin();
    if(acquires!=count || dev.swap_resize.load()!=pack(1720,720) || dev.swapchain!=active)return 19;
    newer_resize=pack(3440,1440);
    present_begin();
    if(acquires!=count || dev.swap_resize.load()!=newer_resize)return 20;
    preflight_fail=false;newer_resize=0;
    present_begin();
    if(acquires!=count+1 || called_width!=3440 || called_height!=1440 || dev.swap_resize.load())return 21;
    create_result=-1;dev.swap_resize.store(pack(1920,1080));count=acquires;
    present_begin();
    if(acquires!=count || dev.swapchain || dev.swap_resize.load()!=pack(1920,1080))return 22;
    create_result=0;present_begin();
    if(acquires!=count+1 || !dev.swapchain || called_width!=1920)return 23;
    stale=true;create_result=-1;count=acquires;present_begin();
    if(acquires!=count || dev.swapchain || !dev.swap_resize.load())return 24;
    stale=false;create_result=0;dev.swap_resize.store(0);present_begin();
    if(acquires!=count+1 || !dev.swapchain)return 25; // Null-chain recovery without a resize notification.
    opts.hdr=1;preflight_fail=true;count=acquires;active=dev.swapchain;present_begin();
    if(acquires!=count || dev.swapchain!=active || !dev.swap_resize.load())return 27;
    preflight_fail=false;present_begin();
    if(acquires!=count+1 || !dev.swapchain || dev.swap_resize.load())return 28;
    // The display thread chose present-ahead on a chain with two spare images.
    PendingPresent deferred{};
    present_defer_into=&deferred;
    image_count=3;
    dev.swap_resize.store(pack(1280,720));
    count=acquires;
    present_begin();
    if (dev.swap_spare!=1 || acquires!=count) return 40;
    // The next ordinary frame can use the smaller replacement.
    present_defer_into=nullptr;
    present_begin();
    if (acquires!=count+1) return 41;
    const PendingPresent pending_old{true, 0, dev.swapchain, dev.swapchain_generation};
    idle=false;
    acquire_with_result(VK_SUCCESS);
    acquire_with_result(VK_ERROR_SURFACE_LOST_KHR);
    if (after_acquire!=1 || surfaces_destroyed!=1 || dev.surface || dev.swapchain ||
        dev.present_queue_supported || bad_destroy || present_commit(pending_old)) return 33;
    dev.surface=2;
    next_chain=pending_old.swapchain-1;  // A driver is free to reuse the old handle.
    if (!recreate() || dev.swapchain!=pending_old.swapchain || present_commit(pending_old) || committed) return 34;
    const PendingPresent pending_new{true, 0, dev.swapchain, dev.swapchain_generation};
    if (!present_commit(pending_new) || committed!=1) return 35;
    idle=false;
    if (present_with_result(VK_ERROR_SURFACE_LOST_KHR, true) || surfaces_destroyed!=2 ||
        dev.surface || dev.swapchain || bad_destroy || present_commit(pending_new)) return 36;
    if (!present_with_result(VK_SUCCESS, false) || surfaces_destroyed!=2) return 37;
    dev.surface=3;
    if (!recreate()) return 38;
    create_result=VK_ERROR_SURFACE_LOST_KHR;
    idle=false;
    if (recreate() || surfaces_destroyed!=3 || dev.surface || dev.swapchain || bad_destroy) return 39;
    if(bad_destroy || bad_chain)return 8;
    reflex_swapchain_destroyed();
    if(cached.swapchain || cached.latency || cached.frame_marked || cached.mode!=-1)return 26;
    // (longshot) The close itself, run as the source wires it (close_now is close_seq, verbatim
    // from mkwvk_close_asked): it sets the flag, drains the presenter, retires under the close's
    // locks (the Deck's Game Mode compositor makes its final display commit as the window goes
    // away; pinned swapchain images and an un-idle device starve that commit, and the player
    // sees the last frames as garbage over the UI). After the retire, present_image must NOT
    // make a chain again: the window is closing, and a new chain would take the screen back
    // from the compositor mid-transition.
    dev.surface=3;create_result=0;idle=false;
    if(!recreate() || !dev.swapchain)return 44;   // (a live chain to close over)
    idle=false;
    close_now();   // (the real close wiring: flag, drain, retire under both locks)
    if(!idle || dev.swapchain || bad_destroy)return 42;   // (the device idled, the chain gone, destructions in order)
    if(!dev.closing.load())return 45;   // (and the flag it sets is the close's own, not the fixture's)
    count=acquires;
    present_begin();   // (the real present_image prefix: under closing it returns before any acquire or create)
    if(acquires!=count || dev.swapchain || bad_chain)return 43;
    // (the review's catch, pinned): a close over a surface the lost-surface path ALREADY let go
    // must not wait on the device or destroy again — after a lost device that wait is what hangs
    // an exit instead of ending it. Retire is gated on a chain actually being held.
    dev.closing.store(false); dev.surface=3; create_result=VK_ERROR_SURFACE_LOST_KHR; idle=false;
    if(recreate())return 48;                       // (surface loss: the lost-surface path let the chain go)
    if(dev.swapchain)return 49;                    // (nothing held to let go anymore)
    idle=false; destroyed_swapchain=0;
    close_now();                                   // (close over a window with no chain)
    if(idle || destroyed_swapchain)return 50;      // (no device wait, no destroy: it lets go only what is held)
    if(!dev.closing.load())return 51;              // (the flag still goes up: no present takes a chain back)
    create_result=0;                               // (this case's surface-loss result: not the next case's)
    // (start's guard, verbatim): a window already closing during startup gets no initial chain;
    // a window not closing does. The lock is the close's, so the two cannot cross.
    dev.closing.store(false); dev.swap_resize.store(0); preflight_fail=false; newer_resize=0;
    if(!startup_guard(640,480) || !dev.swapchain)return 46;
    close_now();
    if(startup_guard(640,480) || dev.swapchain)return 47;
    // (the round-one majors, pinned): the close only destroys when the presenter is proven idle,
    // and it never hangs behind a present that holds the mutex. A close while the mutex is held
    // and the drain answers false waits a bounded few seconds, then leaves the chain to the
    // process exit — exactly what happened before this block existed — raising the flag all the
    // same. When the way is clear again, the same close retires normally.
    dev.closing.store(false); dev.swap_resize.store(0); idle=false; destroyed_swapchain=0;
    if(!recreate())return 52;                       // (a chain held again for these cases)
    std::atomic<bool> hold_go{true}, hold_has{false};
    std::thread holder_t([&]{ std::lock_guard g(present_image_mu); hold_has=true; while(hold_go) std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
    while(!hold_has) std::this_thread::sleep_for(std::chrono::milliseconds(5));   // (the present really holds the mutex before the close tries)
    drain_says=false;
    auto t0=std::chrono::steady_clock::now();
    close_now();                                    // (patience, then give up: no destroy under a present in flight)
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
    hold_go=false; holder_t.join();                 // (the present over)
    if(ms<4500 || ms>7000)return 53;                // (the bounded wait, not a hung close)
    if(!dev.swapchain || idle || destroyed_swapchain)return 54;   // (chain left to the exit, nothing destroyed)
    if(!dev.closing.load())return 55;               // (the flag went up all the same)
    drain_says=true;
    close_now();                                    // (the present over: the close retires as usual)
    if(dev.swapchain || !idle || !destroyed_swapchain)return 56;
    // (and the drain's answer alone gates the destroy: mutex free, presenter stuck mid-present:
    // destroying would leave it touching a chain that is gone)
    dev.closing.store(false); idle=false; destroyed_swapchain=0;
    if(!recreate())return 57;
    drain_says=false;
    close_now();
    if(!dev.swapchain || idle || destroyed_swapchain)return 58;
    drain_says=true;
    // (the review's other catch, pinned): the queue's lock is held across vkQueueSubmit2 by
    // queue_flush, so a submit the driver stalled leaves the image mutex free and the presenter
    // idle: the close must not wait on that lock without end either. It waits its bounded
    // patience, changes nothing, and retires once the queue is free.
    dev.closing.store(false); idle=false; destroyed_swapchain=0;
    if(!recreate())return 59;
    const uint64_t q_chain=dev.swapchain, q_destroyed=destroyed_swapchain;
    const bool q_idle=idle;                        // (recreate's own state: what "changed nothing" is measured from)
    std::atomic<bool> q_go{true}, q_has{false};
    std::thread q_holder([&]{ std::lock_guard g_q(dev.present_mu); q_has=true; while(q_go) std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
    while(!q_has) std::this_thread::sleep_for(std::chrono::milliseconds(5));   // (the queue really held before the close tries)
    auto q0=std::chrono::steady_clock::now();
    close_now();                                    // (patience on the queue's lock, then give up)
    auto qms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-q0).count();
    q_go=false; q_holder.join();
    if(qms<4500 || qms>7000)return 60;              // (the bounded wait, not a hung close)
    if(dev.swapchain!=q_chain || idle!=q_idle || destroyed_swapchain!=q_destroyed)return 61;   // (nothing changed: the chain is left to the exit)
    if(!dev.closing.load())return 62;               // (the flag went up all the same)
    close_now();                                    // (the queue free again: the close retires as usual)
    if(dev.swapchain || !idle || destroyed_swapchain!=q_chain)return 63;
    std::puts("PASS: per-image semaphores, retirement, query/allocation/create failures, latest-resize retry and acquire guard");
    std::puts("PASS: acquire/present/create surface loss, ordered destruction, and deferred present invalidation with handle reuse");
    std::puts("PASS: a smaller replacement blocks deferred acquire and allows the ordinary path");
    std::puts("PASS: a close retires the chain with the device idled, and no present makes a new one after");
    std::puts("PASS: the close and the startup guard run as the sources wire them (the flag and locks are the source's)");
    std::puts("PASS: the close gives up its wait and skips the destroy when a present is still running");
    std::puts("PASS: the close gives up its wait on the queue's lock too, and retires once the queue is free");
}
'''
with tempfile.TemporaryDirectory(prefix='present-semaphores-') as directory:
    folder = Path(directory)
    cpp, exe = folder/'fixture.cpp', folder/'fixture.exe'
    cpp.write_text(fixture)
    subprocess.run([a.cxx, '-std=c++20', '-include', ENV_ON_H, '-O1', str(cpp), '-o', str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
