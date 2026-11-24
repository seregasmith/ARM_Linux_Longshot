/* Where the rx_* calls go (gpu/vk/vk_api.h): the Vulkan back end in longshot_vk.dll (longshot_vk.so) beside the
   executable, the only renderer. Loaded at rx_start, before any other rx_* call; a DLL that is missing or finds no usable
   Vulkan device leaves the wgr_* stand-ins in place (host/rx_nowgpu.c, host/posix/linux_stubs.c: once the WebGPU
   renderer's functions, gpu/render, which is built no more), and nothing is drawn (logged). */
#include "hos.h"
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/longshot_env.h"
#endif
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../gpu/vk/vk_api.h"
#include "../gpu/vk/dlss_math.h"
#include "control.h"
#include "../gpu/vk/compat/longshot_env.h"   /* (env_value_off: the switches' one rule) */
#include "audio_output_devices.h"
int hos_monitor_count(void);
int hos_monitor_info(int index, MkwVkMonitor *out);
void hos_monitor_select(const char *id);

/* the stand-ins when there is no Vulkan back end (host/rx_nowgpu.c, host/posix/linux_stubs.c) */
int wgr_start(void *hwnd);
void wgr_resize(int width, int height);
const char *wgr_describe(void);
void wgr_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines);
void wgr_nvn_submit(const uint64_t *handles, int n);
uint64_t wgr_nvn_fence(void);
uint64_t wgr_nvn_pushed(void);
uint64_t wgr_nvn_replayed(void);
int wgr_nvn_wait(uint64_t fence, uint64_t timeout_ns);
void wgr_wait_writebacks_reading(const void *p, uint64_t n);
void wgr_nvn_finish(void);
void wgr_nvn_present(const nvn_tex *t, const int crop[4]);
void wgr_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v);
void wgr_nvn_register_sampler(int id, const nvn_samp_par *p);
void wgr_nvn_set_texture_pool_size(int n);
void wgr_nvn_texture_released(const nvn_tex *t);
void wgr_nvn_program_released(const nvn_prog *p);
void wgr_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                         uint64_t row, uint64_t img);
void wgr_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                          const uint8_t *src, uint64_t row, uint64_t img);
void wgr_nvn_invalidate(uint64_t gpu, uint64_t size);

/* what the Vulkan back end is given */
uint8_t *hos_guest_base(void);
uint64_t hos_guest_size(void);
uint8_t *hos_gpu_ptr(uint64_t va, uint64_t *avail);
uint64_t nvn_gpu_guest(uint64_t gpu);
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n);
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n);
uint32_t *rx_collect_written(int64_t *count);   /* gpu/render/rx_core.cpp: the soft write watch's pages */
void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);
int nvn_depth_minus_w(void);
int hos_gpu_pools(uint64_t (*out)[2], int max);
uint32_t hos_gpu_maps_generation(void);
uint32_t nvn_vmap_generation(void);
uint64_t hos_reserved_cpus(void);   /* hos_thread.c */
extern float hos_aspect_ratio;      /* hos_main.c: the guest camera aspect applied at startup, or 0 */
uint32_t hos_pad_raw(void);         /* nn_hid.c: the gamepads and the controls, for the Insert menu */
int hos_binding(int set, int button, int slot);
int hos_mic_count(void);            /* nn_audio_in.c: the microphones, for the Insert menu */
const char *hos_mic_name(int index);
void hos_mic_select(int mode, const char *name, int index);
int hos_mic_test(int enabled, int gain, int *levels);
void hos_sdl_rumble_test(void);   /* pad_sdl.c */
int hos_nfc_count(void);            /* nn_nfp.c: the player's NFC tags, for the Insert menu's NFC tab */
const char *hos_nfc_name(int index);
int hos_nfc_tap(int index);
int hos_nfc_state(void);
const char *hos_nfc_dir(void);
int hos_nfc_scan(int start);
const char *hos_nfc_scan_name(void);
void hos_bind(int set, int button, int slot, int code);
int hos_reset_binding(int button);
int hos_binding_is_default(int button);
int hos_rebind_control(int set, int button, int slot, int code, uint64_t approved, uint64_t *conflicts);
int hos_prompts_face(void);         /* prompts.c */
int hos_prompts_live_parts(void);   /* prompts.c */
const char *hos_perf_info(char *map, int cap, int64_t movie[5], float pose[5], int *has_pose);   /* hos_main.c */
void hos_perf_tracking(int enabled);   /* hos_main.c: read Link's pose only for the opt-in performance summary */
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
void hos_mark_written(const void *p, size_t n);
#endif

static void host_log(const char *line) { hos_log("%s", line); }
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
int hos_gpu_guest_ranges(uint64_t (*out)[2], int max);
/* The soft write watch's marks (rt/a64.h A64_MARK, hos_mark_written) over the GPU-visible ranges, read and cleared:
   the Vulkan back end's own collection (gpu/render's rx_collect_written works only with the WebGPU renderer started).
   One consumer: with Vulkan the WebGPU renderer never runs. */
/* (256 marks, none set? Read as 32 words in one pass the compiler makes four AVX2 loads of: most of the ranges hold no
   mark at a submit, and at 32 marks a step the walk itself, not the memory, was what a scan cost) */
static inline int marks_clear_256(const uint8_t *m) {
    uint64_t w[32], any = 0;
    memcpy(w, m, sizeof w);
    for (int i = 0; i < 32; i++) any |= w[i];
    return !any;
}
/* (the vector walk, on x86-64 and arm64: the marks read 256 to a test while none is set, else 64 to a test, in vector loads
   from any page (SSE2's 16-byte loads, AVX2's 32-byte ones in a build for AVX2, NEON's 16-byte ones on arm64), then each
   mark read set taken in turn. The word loads (marks_clear_256, and collect_in's 32- and 8-mark tests) test only from a
   page aligned to their width: a range's unaligned head and tail, and the pages around each set mark, go 32, 8 and 1 at a
   time. HOS_COLLECT_VECTOR=0: the word walk, for a same-build A/B.
   The vector loads are the asm's own, not C accesses, so they race nothing in the C model (a memcpy of the marks is a
   plain read of bytes the writers store atomically): arm64's ld1 of .16b elements is sixteen single-copy atomic byte
   reads, and x86's movdqu and vmovdqu read each byte whole; the writers' granule. Like the word loads they order nothing:
   they only choose the pages the walk reads again and clears with the same relaxed load and acquire exchange, so the
   pages found, their order, the marks cleared and what a found page's exchange makes visible are the word walk's, and so
   is what a pass does with a mark a producer publishes while it runs: a page after one just taken is decided by a read
   made after that take (a window's mask is used on only while its next set mark is the next page, else the marks are read
   again from the page after the take), so it is taken in this pass if its mark is set by then and left set for the next
   pass if not. The asm is GCC's and clang's: another compiler, or another CPU, builds the word walk alone) */
#ifndef HOS_MARKS_VECTOR
#if (defined(__x86_64__) || defined(__aarch64__)) && (defined(__GNUC__) || defined(__clang__))
#define HOS_MARKS_VECTOR 1
#else
#define HOS_MARKS_VECTOR 0
#endif
#endif
#if HOS_MARKS_VECTOR
#if defined(__aarch64__)
#include <arm_neon.h>
#define MARKS_LOADS "16-byte loads (NEON)"
typedef uint8x16_t marks16;
static inline marks16 marks_load16(const uint8_t *m) {
    marks16 v;
    __asm__ __volatile__("ld1 {%0.16b}, [%1]" : "=w"(v) : "r"(m), "m"(*(const uint8_t (*)[16])m));
    return v;
}
#elif defined(__AVX2__)
#include <immintrin.h>
#define MARKS_LOADS "32-byte loads (AVX2)"
static inline __m256i marks_load32(const uint8_t *m) {
    __m256i v;
    __asm__ __volatile__("vmovdqu %1, %0" : "=x"(v) : "m"(*(const uint8_t (*)[32])m));
    return v;
}
#else
#include <emmintrin.h>
#define MARKS_LOADS "16-byte loads (SSE2)"
typedef __m128i marks16;
static inline marks16 marks_load16(const uint8_t *m) {
    marks16 v;
    __asm__ __volatile__("movdqu %1, %0" : "=x"(v) : "m"(*(const uint8_t (*)[16])m));
    return v;
}
#endif
/* (bit i: m[i] read set, i < 64) */
static inline uint64_t marks_set64(const uint8_t *m) {
#if defined(__aarch64__)
    const marks16 a = marks_load16(m), b = marks_load16(m + 16), c = marks_load16(m + 32), d = marks_load16(m + 48);
    if (!vmaxvq_u8(vorrq_u8(vorrq_u8(a, b), vorrq_u8(c, d)))) return 0;   /* (none set: nearly every test) */
    /* (each set mark's bit of its byte, summed in pairs three times: byte j then holds marks 8j to 8j + 7, each its own
       bit, so no sum carries) */
    static const uint8_t bits[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t k = vld1q_u8(bits);
    const uint8x16_t ab = vpaddq_u8(vandq_u8(vtstq_u8(a, a), k), vandq_u8(vtstq_u8(b, b), k));
    const uint8x16_t cd = vpaddq_u8(vandq_u8(vtstq_u8(c, c), k), vandq_u8(vtstq_u8(d, d), k));
    const uint8x16_t abcd = vpaddq_u8(ab, cd);
    return vgetq_lane_u64(vreinterpretq_u64_u8(vpaddq_u8(abcd, abcd)), 0);
#elif defined(__AVX2__)
    const __m256i a = marks_load32(m), b = marks_load32(m + 32), o = _mm256_or_si256(a, b);
    if (_mm256_testz_si256(o, o)) return 0;   /* (none set: nearly every test) */
    const __m256i z = _mm256_setzero_si256();
    const uint64_t za = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, z)),
                   zb = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, z));
    return ~(za | zb << 32);   /* (the marks not 0) */
#else
    const marks16 a = marks_load16(m), b = marks_load16(m + 16), c = marks_load16(m + 32), d = marks_load16(m + 48);
    const __m128i z = _mm_setzero_si128();
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d)), z)) == 0xFFFF) return 0;
    const uint64_t za = (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(a, z)), zb = (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(b, z)),
                   zc = (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(c, z)), zd = (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(d, z));
    return ~(za | zb << 16 | zc << 32 | zd << 48);   /* (the marks not 0) */
#endif
}
/* (the 256 marks from m all clear? The widest step of the word walk (marks_clear_256) in the vector loads, from any page:
   with 64 a test the long ranges, where nearly every mark is clear, were no faster than the word walk's 256, or slower
   (SSE2: 0.9 times its speed game-free). Four accumulators, so the loads do not wait for each other) */
static inline int marks_vclear_256(const uint8_t *m) {
#if defined(__aarch64__)
    marks16 a = marks_load16(m), b = marks_load16(m + 16), c = marks_load16(m + 32), d = marks_load16(m + 48);
    for (int i = 64; i < 256; i += 64) {
        a = vorrq_u8(a, marks_load16(m + i)), b = vorrq_u8(b, marks_load16(m + i + 16));
        c = vorrq_u8(c, marks_load16(m + i + 32)), d = vorrq_u8(d, marks_load16(m + i + 48));
    }
    return !vmaxvq_u8(vorrq_u8(vorrq_u8(a, b), vorrq_u8(c, d)));
#elif defined(__AVX2__)
    __m256i a = marks_load32(m), b = marks_load32(m + 32);
    for (int i = 64; i < 256; i += 64) a = _mm256_or_si256(a, marks_load32(m + i)), b = _mm256_or_si256(b, marks_load32(m + i + 32));
    a = _mm256_or_si256(a, b);
    return _mm256_testz_si256(a, a);
#else
    marks16 a = marks_load16(m), b = marks_load16(m + 16), c = marks_load16(m + 32), d = marks_load16(m + 48);
    for (int i = 64; i < 256; i += 64) {
        a = _mm_or_si128(a, marks_load16(m + i)), b = _mm_or_si128(b, marks_load16(m + i + 16));
        c = _mm_or_si128(c, marks_load16(m + i + 32)), d = _mm_or_si128(d, marks_load16(m + i + 48));
    }
    return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d)), _mm_setzero_si128())) == 0xFFFF;
#endif
}
_Static_assert(A64_DIRTY_PAGES >= 64, "the vector walk reads 64 marks inside the array");
/* (the marks of pages p, p + 1, ... below p1 read set: bit i for page p + i. With fewer than 64 left, the 64 marks that
   end at p1, or that begin at page 0, are read and those outside [p, p1) dropped: all inside the array, as
   p1 <= A64_DIRTY_PAGES) */
static inline uint64_t marks_from(uint64_t p, uint64_t p1) {
    if (p + 64 <= p1) return marks_set64(&a64_dirty[p]);
    const uint64_t w = p1 >= 64 ? p1 - 64 : 0;
    return (marks_set64(&a64_dirty[w]) >> (p - w)) & ((1ull << (p1 - p)) - 1);
}
#endif
static int64_t collect_in(const uint64_t (*ranges)[2], int nranges, uint32_t **pages) {
    /* (HOS_COLLECT_WIDE=0: 32 marks a step at most, as before; the pages found are the same either way, so HOS_AB may flip it
       between two walks: host/hos_ab.c) */
    static int wide_set = -1;
    if (wide_set < 0) {
        const char *e = getenv("HOS_COLLECT_WIDE");
        wide_set = !(e && e[0] == '0');
    }
    const int wide = hos_ab_value("HOS_COLLECT_WIDE", wide_set);
#if HOS_MARKS_VECTOR
    /* (HOS_COLLECT_VECTOR=0: the word walk, as before; read at the first collection, and logged once) */
    static int vector_on = -1;
    int vector = __atomic_load_n(&vector_on, __ATOMIC_RELAXED);
    if (vector < 0) {
        const char *e = getenv("HOS_COLLECT_VECTOR");
        int unread = -1;
        vector = !(e && env_value_off(e));   /* (the switches' one rule: =0, false, off, no or empty turn it off) */
        if (__atomic_compare_exchange_n(&vector_on, &unread, vector, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            hos_log("mem: the write marks' walk: %s", vector ? "256 marks a test, else 64, in " MARKS_LOADS " (HOS_COLLECT_VECTOR=0: word loads)"
                                                             : "word loads (HOS_COLLECT_VECTOR=0)");
    }
#endif
    size_t n = 0, cap = 4096;
    uint32_t *out = malloc(cap * sizeof *out);
    for (int r = 0; r < nranges; r++) {
        const uint64_t lo = ranges[r][0], hi = ranges[r][1] < hos_guest_size() ? ranges[r][1] : hos_guest_size();
        uint64_t p1 = (hi + 4095) >> 12;
        if (p1 > A64_DIRTY_PAGES) p1 = A64_DIRTY_PAGES;
#if HOS_MARKS_VECTOR
        uint64_t at = 0, set = 0;   /* (the vector walk: the marks of pages at, at + 1, ... read set and not yet taken) */
#endif
        for (uint64_t p = lo >> 12; p < p1;) {
#if HOS_MARKS_VECTOR
            if (vector) {   /* (256, else 64 marks a test, from any page; then each mark read set, in order) */
                if (!set) {
                    if (p + 256 <= p1 && __builtin_expect(marks_vclear_256(&a64_dirty[p]), 1)) {   /* (256 clear marks) */
                        p += 256;
                        continue;
                    }
                    set = marks_from(p, p1);
                    at = p;
                    if (!set) {
                        p += 64;
                        continue;
                    }
                }
                p = at + (uint64_t)__builtin_ctzll(set);
                set &= set - 1;
            } else
#endif
            {
                if (wide && !(p & 255) && p + 256 <= p1 && marks_clear_256(&a64_dirty[p])) {   /* (256 pages at a time while none is marked) */
                    p += 256;
                    continue;
                }
                if (!(p & 31) && p + 32 <= p1) {   /* (32 pages at a time while none is marked) */
                    uint64_t w[4];
                    memcpy(w, &a64_dirty[p], 32);
                    if (!(w[0] | w[1] | w[2] | w[3])) {
                        p += 32;
                        continue;
                    }
                }
                if (!(p & 7) && p + 8 <= p1) {   /* (eight pages a load while none is marked) */
                    uint64_t w;
                    memcpy(&w, &a64_dirty[p], 8);
                    if (!w) {
                        p += 8;
                        continue;
                    }
                }
            }
            if (__atomic_load_n(&a64_dirty[p], __ATOMIC_RELAXED) && __atomic_exchange_n(&a64_dirty[p], 0, __ATOMIC_ACQUIRE)) {
                if (n == cap) out = realloc(out, (cap *= 2) * sizeof *out);
                out[n++] = (uint32_t)p;
            }
            p++;
#if HOS_MARKS_VECTOR
            /* (the reads that decide the pages after one taken come after it, as the word walk's do: the window's mask is
               kept only while the next mark it read set is the next page; before a gap the marks are read again from here, so
               a mark published while this page was taken, in the gap, is taken in this pass) */
            if (vector && set && at + (uint64_t)__builtin_ctzll(set) != p) set = marks_from(p, p1), at = p;
#endif
        }
    }
    *pages = out;
    return (int64_t)n;
}
static int64_t host_collect(uint32_t **pages) {
    static uint64_t (*ranges)[2];
    if (!ranges) ranges = malloc(sizeof *ranges * 4096);
    return collect_in(ranges, hos_gpu_guest_ranges(ranges, 4096), pages);
}
/* (only within the given guest ranges: the submit-time capture of the mirrored pools, a small part of the 20 ms the
   whole collection takes) */
static int64_t host_collect_in(const uint64_t (*ranges)[2], int n, uint32_t **pages) { return collect_in(ranges, n, pages); }
#else
static int64_t host_collect(uint32_t **pages) {
    int64_t n = -1;
    *pages = rx_collect_written(&n);
    return n;
}
static int64_t host_collect_in(const uint64_t (*ranges)[2], int n, uint32_t **pages) { (void)ranges, (void)n; *pages = 0; return 0; }
#endif
static void host_free_pages(uint32_t *pages) { free(pages); }
static void host_mark_written(const void *p, uint64_t n) {
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
    hos_mark_written(p, (size_t)n);
#else
    (void)p, (void)n;
#endif
}

/* (the renderer's own guest memory: the address space above the heap, as the game's own regions, committed and
   zeroed; the region allocator never hands it to the game) */
static uint64_t host_guest_alloc(uint64_t size) {
    const uint64_t a = hos_region_alloc(size, 0x10000);
    /* (marked written after the zeros: the region may lie where a freed one did, under an import the renderer still
       holds with its copies of the old bytes) */
    if (a) memset(hos_guest_base() + a, 0, size), host_mark_written(hos_guest_base() + a, size);
    return a;
}

void hos_watch_guest_writes(const uint8_t *data, uint64_t size);   /* nvn_native.c */
static void host_watch_guest_writes(uint64_t guest, uint64_t size) { hos_watch_guest_writes(hos_guest_base() + guest, size); }
static float host_aspect_ratio(void) { return hos_aspect_ratio; }
#ifndef _WIN32
int hos_sdl_desktop_size(int *width, int *height);   /* hos_sdl.c */
#endif
/* (longshot) the main screen's size, as host/hos_main.c takes its shape (MkwVkHost.screen_size) */
static int host_screen_size(int *width, int *height) {
    int hos_monitor_screen_size(int *, int *);
    if (hos_monitor_screen_size(width, height)) return 1;
#ifdef _WIN32
    const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return 0;
    *width = w, *height = h;
    return 1;
#else
    return hos_sdl_desktop_size(width, height);
#endif
}
static float host_wide_menus(uint32_t keep_mask) {   /* (MkwVkHost.wide_menus) */
    extern volatile uint32_t hos_wide_keep_mask;
    extern volatile float hos_wide_margin;
    static const char *forced;
    static int looked;
    if (!looked) looked = 1, forced = getenv("HOS_WIDE_KEEP");
    hos_wide_keep_mask = forced ? (uint32_t)strtoul(forced, NULL, 16) : keep_mask;
    extern volatile float hos_wide_ratio;   /* (narrower than 16:9: the menus fit its width whatever the Ultrawide menus row says,
                                               never stretched up and down; the row is about wider screens) */
    if (!forced && hos_wide_ratio > 0.0f && hos_wide_ratio < 16.0f / 9.0f * 0.999f) hos_wide_keep_mask &= ~2u;
    void hos_wide_ui_tick(void);   /* (hos_wide_ui.c: the HUD's layout for the picture's shape, each present) */
    hos_wide_ui_tick();
    return hos_wide_margin;
}
int hos_mouse_pointer(MkwVkPointer *out);   /* display.c, posix/display_sdl.c */
int hos_sdl_gyro_status(void);   /* pad_sdl.c */
extern const char *hos_romfs_dir;   /* nn_fs.c, set by hos_main.c */
static const char *host_romfs_dir(void) { return hos_romfs_dir; }
const char *hos_options_file(void);   /* options_path.c */

#ifdef _WIN32
static unsigned char menu_key_press[256];
void hos_vk_note_key_press(unsigned key) {
    if (key < sizeof menu_key_press) __atomic_store_n(&menu_key_press[key], 1, __ATOMIC_RELAXED);
}
int hos_vk_take_key_press(int key) {
    return (unsigned)key < sizeof menu_key_press &&
           __atomic_exchange_n(&menu_key_press[key], 0, __ATOMIC_RELAXED);
}
#define host_take_key_press hos_vk_take_key_press
#else
#define host_take_key_press NULL
#endif

/* (longshot) the FPS counter's Breakdown: this thread's sleeps so far (host/hos_thread.c) */
extern _Thread_local int64_t hos_slept_ns[2];
extern volatile int64_t hos_poll_slept_ns, hos_acquire_slept_ns;
extern volatile int hos_fps_timing;
static void host_fps_timing(int on) { __atomic_store_n(&hos_fps_timing, on != 0, __ATOMIC_RELAXED); }
int hos_thread_times_of(const char *name, int64_t out[3]);   /* (hos_thread.c) */
static int host_main_thread_times(int64_t out[3]) { return hos_thread_times_of("sead::MainThread", out); }
/* (version 28) the renderer's C++ allocations that its new_handler gave up on: the size is not known there */
static void host_out_of_memory(const char *what) { hos_alloc_gave_up(0, what); }
static void host_slept_ns(int64_t out[2]) {
    out[0] = __atomic_load_n(&hos_poll_slept_ns, __ATOMIC_RELAXED);
    out[1] = hos_slept_ns[1] + __atomic_load_n(&hos_acquire_slept_ns, __ATOMIC_RELAXED);
}

/* (longshot) MkwVkHost.ab_value (version 36): host/hos.h's hos_ab_value, an inline function, as one the renderer can call. Its
   cost outside an A/B is one load, as for the host's own readers */
static int host_ab_value(const char *name, int normal) { return hos_ab_value(name, normal); }

/* (longshot) typed text for the Insert menu's text fields (MkwVkHost.take_text): the window's thread puts each character and
   editing key in (host/display.c WM_CHAR and WM_KEYDOWN, host/posix/display_sdl.c SDL's text input and key events), the menu
   takes them out at its frames, in order. Nothing is kept while no field has the keyboard, and the window then takes no text
   (SDL's text input stopped: no input method's window over the game) */
void hos_display_paste(void);   /* display.c, posix/display_sdl.c: the clipboard's first line put in, on the window's thread */
enum { TEXT_QUEUE = 256 };
static SRWLOCK text_lock = SRWLOCK_INIT;
static uint32_t text_queue[TEXT_QUEUE];
static unsigned text_first, text_n;
static volatile LONG text_focus;
/* whether a field has the keyboard (the window's thread: whether to take text and editing keys) */
int hos_text_focus(void) { return InterlockedCompareExchange(&text_focus, 0, 0); }
/* a character typed (a Unicode code point) or HOS_TEXT_KEY | an editing key; dropped while no field has the keyboard, or when
   the menu has not taken the last TEXT_QUEUE (it takes them at every frame) */
void hos_text_put(uint32_t c) {
    if (!hos_text_focus()) return;
    AcquireSRWLockExclusive(&text_lock);
    if (text_n < TEXT_QUEUE) text_queue[(text_first + text_n++) % TEXT_QUEUE] = c;
    ReleaseSRWLockExclusive(&text_lock);
}
/* UTF-8 typed (SDL's text input) or pasted (first_line: up to its first line break), a character at a time; a broken
   sequence is left out */
void hos_text_put_utf8(const char *s, int first_line) {
    while (s && *s) {
        const unsigned char b = (unsigned char)*s++;
        const int more = b < 0x80 ? 0 : (b & 0xe0) == 0xc0 ? 1 : (b & 0xf0) == 0xe0 ? 2 : (b & 0xf8) == 0xf0 ? 3 : -1;
        uint32_t c = more == 0 ? b : more == 1 ? b & 0x1fu : more == 2 ? b & 0x0fu : b & 0x07u;
        int got = 0;   /* (continuation bytes taken; the byte that broke a sequence is read again as a character's first) */
        while (more > 0 && got < more && ((unsigned char)*s & 0xc0) == 0x80) c = c << 6 | ((unsigned char)*s++ & 0x3fu), got++;
        if (more < 0 || got < more) continue;
        if (first_line && (c == '\n' || c == '\r')) break;
        hos_text_put(c);
    }
}
static int host_take_text(int focus, uint32_t *out, int max) {
    InterlockedExchange(&text_focus, focus != HOS_TEXT_OFF);
    if (focus == HOS_TEXT_PASTE) hos_display_paste();
    int n = 0;
    AcquireSRWLockExclusive(&text_lock);
    if (focus == HOS_TEXT_OFF) text_n = 0;
    for (; n < max && text_n; n++, text_n--, text_first = (text_first + 1) % TEXT_QUEUE) out[n] = text_queue[text_first];
    ReleaseSRWLockExclusive(&text_lock);
    return n;
}

/* (hookshot) the Mods tab: host/mods.c, host/cheats.c */
int hos_mod_info(int i, const char **name, const char **detail, const char **note, int *enabled);
void hos_mod_edit(int i, int op);
uint64_t hos_mods_changed(void);
int hos_cheat_info(int i, const char **name, const char **mod, const char **note, int *kind, int *on);
void hos_cheat_set(int i, int on);

int nvn_output_preview_request(uint32_t width, uint32_t height);
/* (longshot, issue 1113) MkwVkHost's input recordings (version 42): host/input_movie.c's start, stop and status, for the
   Insert menu's Debug tab */
#include "input_movie.h"
int hos_input_record_start(const char *path, int route);
void hos_input_record_stop(void);
int hos_input_play_start(const char *path, int sync, float sync_max);
void hos_input_play_stop(void);
void hos_input_movie_status(HosInputMovieStatus *out);
static int host_input_record(int start) {
    if (start) return hos_input_record_start(NULL, 0);
    hos_input_record_stop();
    return 1;
}
int hos_input_recording_at(int i, char *out, size_t cap);
static int host_input_play(int start, int sync, const char *path) {
    if (start) return hos_input_play_start(path, sync, 1.0f);
    hos_input_play_stop();
    return 1;
}
static void host_input_status(MkwVkInputStatus *out) {
    HosInputMovieStatus s;
    hos_input_movie_status(&s);
    memset(out, 0, sizeof *out);
    out->mode = s.mode, out->started = s.started, out->held = s.held, out->diverged = s.diverged;
    out->frame = s.frame, out->end = s.end, out->link_apart = s.link_apart, out->syncs = s.syncs, out->largest = s.largest;
    snprintf(out->path, sizeof out->path, "%s", s.path);
    snprintf(out->why, sizeof out->why, "%s", s.why);
}
#ifdef _WIN32
static uintptr_t host_vulkan_loader_proc(void) {
    HMODULE loader = LoadLibraryExA("vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) return 0;
    FARPROC resolver = GetProcAddress(loader, "vkGetInstanceProcAddr");
    if (!resolver) FreeLibrary(loader);
    return (uintptr_t)resolver;
}
#endif

static const MkwVkHost vk_host = {
    MKWVK_API_VERSION, host_log, hos_guest_base, hos_guest_size, hos_gpu_ptr, nvn_gpu_guest, nvn_gpu_read,
    nvn_gpu_write, host_collect, host_free_pages, host_mark_written, hos_ticks, hos_now_ns, nvn_tex_layout,
    nvn_tex_level_offset, nvn_fmt_bpb, nvn_tex_level_tiles, nvn_tex_linear_stride, nvn_depth_minus_w, hos_gpu_pools,
    hos_gpu_maps_generation, nvn_vmap_generation, hos_reserved_cpus, host_collect_in, host_guest_alloc,
    host_watch_guest_writes, hos_pad_raw, hos_binding, hos_bind, host_aspect_ratio, host_take_key_press,
    hos_mouse_pointer, hos_mic_count, hos_mic_name, hos_mic_select, hos_sdl_gyro_status, hos_log_flush, hos_prompts_face,
    host_romfs_dir, host_wide_menus, host_slept_ns, host_fps_timing, hos_prompts_live_parts, host_screen_size,
    host_take_text, hos_options_file,
    hos_audio_output_count, hos_audio_output_info, hos_audio_output_select, hos_audio_output_state,
    hos_mic_test, hos_perf_info, host_main_thread_times, host_out_of_memory,
    hos_monitor_count, hos_monitor_info, hos_monitor_select, hos_perf_tracking,
    hos_sdl_rumble_test,
    hos_nfc_count, hos_nfc_name, hos_nfc_tap, hos_nfc_state, hos_nfc_dir, hos_nfc_scan, hos_nfc_scan_name,
    hos_ab_frame,   /* (version 34, longshot: the frame trace's ab_side, host/hos_ab.c) */
    hos_mod_info, hos_mod_edit, hos_mods_changed, hos_cheat_info, hos_cheat_set,
    host_ab_value,  /* (version 36, longshot: a renderer switch's A/B value, host/hos_ab.c) */
    nvn_output_preview_request,
    host_input_record, host_input_play, host_input_status,   /* (version 42, longshot: input recordings, host/input_movie.c) */
    hos_input_recording_at,
    hos_reset_binding,
    hos_rebind_control,
    hos_binding_is_default,
#ifdef _WIN32
    host_vulkan_loader_proc,
#else
    NULL,
#endif
};

static const MkwVkRenderer *vk;   /* the Vulkan back end, when it was chosen and started */
/* (arm64 port) the one being started: the game's thread goes on meanwhile (3 s of shader loading on the Odin 3), and the
   memory pools it makes then are told to it (rx_nvn_pool_created), not lost: where the back end places its own memory under
   the pools (gpu/vk/vk_memory.cpp placed_*), a pool it never heard of is never placed, and nothing in it reaches the GPU
   (the render target pool, the queues' event memory: a black screen with the game running) */
static const MkwVkRenderer *volatile vk_starting;
static HMODULE vk_module;   /* (the last back end loaded: rx_close_asked) */

/* The game registers its first textures and samplers before the window's thread starts the renderer (NVN is up
   first): those calls are kept here and given to the renderer rx_start chose, in order. Without that the Vulkan back
   end never knew the first ~0x60 texture slots, and lit geometry sampled them as its black dummy.
   The texels the game writes then (WriteTexels) are kept too, with a copy of their data: without a renderer they went
   nowhere (no WebGPU renderer to take them), and a texture filled so at boot kept zeros in its storage. OoT's
   4x4 white placeholder is one: bound in place of the ground fog's depth, it read 0 and hid the ground fog at some camera
   angles. That happened when the window's thread started the renderer after the game had begun (windowed, not headless). */
typedef struct {
    int kind;   /* 0 texture registration, 1 sampler registration, 2 texture pool size, 3 texture finalized, 4 texture
                   made, 5 texels written */
    int id;
    nvn_tex t;
    nvn_view v;
    int has_view;
    nvn_samp_par p;
    /* (5: WriteTexels) */
    unsigned level, layer0;
    NVNcopyRegion r;
    uint64_t row, img;
    uint8_t *data;
} early_call;
static early_call *early;
static size_t n_early, cap_early;
static volatile LONG started;   /* rx_start chose (the calls go straight to the renderer from then on) */
static SRWLOCK early_lock = SRWLOCK_INIT;

static void forward(const early_call *c);
/* Keeps a call made before rx_start; 0 when the renderer is chosen already (the caller forwards it) */
static int keep_early(const early_call *c) {
    if (started) return 0;
    AcquireSRWLockExclusive(&early_lock);
    if (started) {
        ReleaseSRWLockExclusive(&early_lock);
        return 0;
    }
    if (n_early == cap_early) {
        cap_early = cap_early ? cap_early * 2 : 1024;
        early = (early_call *)realloc(early, cap_early * sizeof *early);
    }
    early[n_early++] = *c;
    ReleaseSRWLockExclusive(&early_lock);
    return 1;
}
/* rx_start: the kept calls given to the renderer chosen, then the gate opened */
static void flush_early(void) {
    AcquireSRWLockExclusive(&early_lock);
    size_t texels = 0;
    for (size_t i = 0; i < n_early; i++) {
        forward(&early[i]);
        if (early[i].kind == 5) texels++, free(early[i].data);
    }
    if (n_early)
        hos_log("rx: %zu texture and sampler calls made before the renderer started (%zu texel writes), given to it", n_early,
                texels);
    free(early);
    early = NULL, n_early = cap_early = 0;
    InterlockedExchange(&started, 1);
    ReleaseSRWLockExclusive(&early_lock);
}

/* dll: longshot_vk.dll (longshot_vk.so on Linux), the Vulkan back end: one export */
static const MkwVkRenderer *load_vulkan(const char *dll) {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n ? (strrchr(path, '\\') ? strrchr(path, '\\') : strrchr(path, '/')) : NULL;   /* (linux: '/') */
    if (!slash || (size_t)(slash + 1 - path) + strlen(dll) >= sizeof path) return NULL;   /* (a path near MAX_PATH) */
    memcpy(slash + 1, dll, strlen(dll) + 1);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
#ifdef _WIN32
        hos_log("rx: %s did not load (%lu): nothing is drawn", path, GetLastError());
#else   /* (why, as dlerror() said: a missing file, a glibc too old, a wrong ELF class. The line names the file
           LoadLibraryA opened, longshot_vk.so, and drops dlerror()'s leading "<that file>: ", so the path, home folder
           included, is in it once. wgr_start draws nothing) */
        char *ext = strrchr(path, '.');
        if (ext && !strcmp(ext, ".dll")) strcpy(ext, ".so");
        const char *why = hos_load_error();
        const size_t n_path = strlen(path);
        if (!strncmp(why, path, n_path) && !strncmp(why + n_path, ": ", 2)) why += n_path + 2;
        hos_log("rx: %s did not load (%s): nothing is drawn", path, why);
#endif
        return NULL;
    }
    vk_module = m;
    mkwvk_init_fn init = (mkwvk_init_fn)(void *)GetProcAddress(m, "mkwvk_init");
    if (!init) {
        hos_log("rx: %s has no mkwvk_init: nothing is drawn", path);
        return NULL;
    }
    const MkwVkRenderer *r = init(&vk_host);
    if (!r) {
        hos_log("rx: mkwvk_init returned NULL: nothing is drawn");
        return NULL;
    }
    if (r->version != MKWVK_API_VERSION) {
        hos_log("rx: mkwvk_init version mismatch (%u vs %u): nothing is drawn", r->version, MKWVK_API_VERSION);
        return NULL;
    }
    return r;
}

int rx_start(void *hwnd) {
    /* (longshot: Vulkan, the only renderer; RX_BACKEND, which once picked the WebGPU one, is not read) */
    const char *dll = "longshot_vk.dll";
    const MkwVkRenderer *r = load_vulkan(dll);
    /* (the crash report's renderer: host/hos_crash_dialog.h. Before start(), which prepares the shaders: a crash in there
       is a crash of this renderer too) */
    if (r) hos_log("report: renderer Vulkan");
    vk_starting = r;
    if (r && r->start(hwnd)) {
        vk = r;
        hos_log("rx: renderer: Vulkan (%s)", dll);
        flush_early();
        return 1;
    }
    vk_starting = NULL;
    if (r) hos_log("rx: the Vulkan back end could not start on the window: nothing is drawn");
    const int ok = wgr_start(hwnd);
    if (ok) hos_log("report: renderer WebGPU (no usable Vulkan device)");
    flush_early();
    return ok;
}
/* (longshot) longshot --prepare-shaders: the Vulkan back end started with no window and no game, which prepares
   every shader in its start (gpu/vk/vk_main.cpp started_up) and is then let go. 0 when there is no usable Vulkan */
int rx_prepare_shaders(void) {
    const MkwVkRenderer *r = load_vulkan("longshot_vk.dll");
    return r && r->start(NULL);
}
/* (longshot) the window is being closed: a back end still preparing its shaders stops and keeps what it made first (a few
   seconds at most). One without the export (an older one) has nothing to keep */
#ifndef _WIN32
/* Startup failure and all close callers share one claim, before either can end the process or clean up. */
static int rx_end_claim;   /* 0 neither, 1 close, 2 startup failure */
int rx_start_failure_claim(void) {
    int expected = 0;
    return __atomic_compare_exchange_n(&rx_end_claim, &expected, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif
void rx_close_asked(void) {
#ifndef _WIN32
    int expected = 0;
    if (!__atomic_compare_exchange_n(&rx_end_claim, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && expected == 2) {
        /* The renderer has already claimed the failure and will exit; do not race it with close cleanup. */
        for (;;) Sleep(1000);
    }
#endif
    /* (first, before anything that takes time: the player asked to quit, so however the process ends from here (a second
       close of a window that stopped answering) the crash window says nothing; a crash still counts: host/hos_crash_dialog.h) */
    void hos_crash_quit_asked(void);
    hos_crash_quit_asked();
    if (!vk_module) return;
    void (*close_asked)(void) = (void (*)(void))(void *)GetProcAddress(vk_module, "mkwvk_close_asked");
    if (close_asked) close_asked();
}
void rx_resize(int width, int height) { vk ? vk->resize(width, height) : wgr_resize(width, height); }
const char *rx_describe(void) { return vk ? vk->describe() : wgr_describe(); }
void rx_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) {
    vk ? vk->stats(draws, skipped, pipelines) : wgr_stats(draws, skipped, pipelines);
}
void rx_nvn_submit(const uint64_t *handles, int n) { vk ? vk->nvn_submit(handles, n) : wgr_nvn_submit(handles, n); }
void rx_nvn_submit_q(const uint64_t *handles, int n, uint64_t queue) {
    if (vk && vk->nvn_submit_q) vk->nvn_submit_q(handles, n, queue);
    else rx_nvn_submit(handles, n);
}
uint64_t rx_nvn_fence(void) { return vk ? vk->nvn_fence() : wgr_nvn_fence(); }
uint64_t rx_nvn_fence_q(uint64_t queue) { return vk && vk->nvn_fence_q ? vk->nvn_fence_q(queue) : rx_nvn_fence(); }
uint64_t rx_nvn_pushed(void) { return vk ? vk->nvn_pushed() : wgr_nvn_pushed(); }
uint64_t rx_nvn_replayed(void) { return vk ? vk->nvn_replayed() : wgr_nvn_replayed(); }
int rx_nvn_wait(uint64_t fence, uint64_t timeout_ns) {
    return vk ? vk->nvn_wait(fence, timeout_ns) : wgr_nvn_wait(fence, timeout_ns);
}
void rx_wait_writebacks_reading(const void *p, uint64_t n) {
    vk ? vk->wait_writebacks_reading(p, n) : wgr_wait_writebacks_reading(p, n);
}
void rx_nvn_finish(void) { vk ? vk->nvn_finish() : wgr_nvn_finish(); }
static volatile LONG presents;   /* (the frames shown, for the camera jitter below) */
static uint64_t control_picture;

static void control_picture_note(const nvn_tex *t, const int crop[4]) {
    const unsigned tw = t ? t->d.width : 0, th = t ? t->d.height : 0;
    const int cropped = crop && crop[2] > 0 && crop[3] > 0;
    const unsigned w = cropped ? (unsigned)crop[2] : tw, h = cropped ? (unsigned)crop[3] : th;
    /* Keep both pairs in one snapshot. 16 bits per side covers the supported output sizes. */
    const uint64_t value = tw && th && tw <= UINT16_MAX && th <= UINT16_MAX && w <= UINT16_MAX && h <= UINT16_MAX
        ? (uint64_t)tw << 48 | (uint64_t)th << 32 | (uint64_t)w << 16 | h : 0;
    __atomic_store_n(&control_picture, value, __ATOMIC_RELAXED);
}

int rx_control_picture(HosControlPicture *out) {
    const uint64_t value = __atomic_load_n(&control_picture, __ATOMIC_RELAXED);
    if (!value) return 0;
    *out = (HosControlPicture){(unsigned)(value >> 48), (uint16_t)(value >> 32),
                              (uint16_t)(value >> 16), (uint16_t)value};
    return 1;
}

static void nvn_present_with_generation(const nvn_tex *t, const int crop[4], uint64_t generation) {
    InterlockedIncrement(&presents);
    hos_slow_frame_present();   /* (HOS_SLOW_FRAMES: a late frame's samples written out, host/slow_frames.c) */
    hos_ab_present();   /* (longshot) HOS_AB: the frame counted for its side (host/hos_ab.c) */
    if (hos_control_on) control_picture_note(t, crop);
    if (vk && generation && vk->nvn_present_trial) vk->nvn_present_trial(t, crop, generation);
    else if (vk) vk->nvn_present(t, crop);
    else wgr_nvn_present(t, crop);
}

void rx_nvn_present(const nvn_tex *t, const int crop[4]) { nvn_present_with_generation(t, crop, 0); }
void rx_nvn_present_trial(const nvn_tex *t, const int crop[4], uint64_t generation) {
    nvn_present_with_generation(t, crop, generation);
}
/* (longshot, nx2/oot_jitter.py) the temporal upscaler's camera jitter (NVIDIA DLSS, AMD FSR 4: gpu/vk/vk_temporal.cpp): this frame's sub-pixel offset of the scene's projection, in clip
   space (what P[0][2] and P[1][2] get), into out, for the projection matrix at guest address projection (row major, as the
   game fills its views' uniform buffers from it); 0 when there is none: DLSS and FSR off (or a renderer without them: rx_option
   HOS_OPT_DLSS_JITTER_W and _H, the scene's size in pixels, are 0), or a projection that is not one of the picture's
   (gpu/vk/dlss_math.h dlss_jitter_clip: a shadow map's, the HUD's). The renderer reads the offset back from
   the camera's uniform buffer (gpu/vk/vk_temporal.cpp), so this only has to pick sub-pixel offsets that cover a pixel evenly:
   the Halton (2, 3) sequence, over as many frames as the upscaler's scale wants (HOS_OPT_DLSS_JITTER_PHASES: gpu/vk/dlss_math.h
   dlss_jitter_phases, 8 for DLAA up to 64). */
int hos_dlss_jitter_clip(uint64_t projection, float out[2]) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static LONG at = -1;
    static MkwVkJitter jitter;
    MkwVkJitter sample;
    AcquireSRWLockExclusive(&lock);
    const LONG frame = presents;
    if (frame != at) {   /* (asked once a frame: the renderer's lock is not taken for every view) */
        at = frame;
        jitter = (MkwVkJitter){0};
        if (vk && vk->jitter_sample) vk->jitter_sample(&jitter);
    }
    sample = jitter;
    ReleaseSRWLockExclusive(&lock);
    float p[16];
    memcpy(p, GP(projection), sizeof p);
    extern volatile float hos_wide_ratio;
    return dlss_jitter_clip(p, hos_wide_ratio, (unsigned)frame, (int)sample.phases, (float)sample.width, (float)sample.height, out);
}
static void forward(const early_call *c) {
    switch (c->kind) {
    case 0: {
        const nvn_view *v = c->has_view ? &c->v : NULL;
        vk ? vk->nvn_register_texture(c->id, &c->t, v) : wgr_nvn_register_texture(c->id, &c->t, v);
        break;
    }
    case 1: vk ? vk->nvn_register_sampler(c->id, &c->p) : wgr_nvn_register_sampler(c->id, &c->p); break;
    case 2: vk ? vk->nvn_set_texture_pool_size(c->id) : wgr_nvn_set_texture_pool_size(c->id); break;
    case 3: vk ? vk->nvn_texture_released(&c->t) : wgr_nvn_texture_released(&c->t); break;
    case 4: if (vk && vk->nvn_texture_created) vk->nvn_texture_created(&c->t); break;
    case 5:
        vk ? vk->nvn_write_texels(&c->t, c->level, c->layer0, &c->r, c->data, c->row, c->img)
           : wgr_nvn_write_texels(&c->t, c->level, c->layer0, &c->r, c->data, c->row, c->img);
        break;
    }
}
void rx_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) {
    if (!started && t) {
        early_call c = {0, id};
        c.t = *t;
        if (v) c.v = *v, c.has_view = 1;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_register_texture(id, t, v) : wgr_nvn_register_texture(id, t, v);
}
void rx_nvn_register_sampler(int id, const nvn_samp_par *p) {
    if (!started && p) {
        early_call c = {1, id};
        c.p = *p;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_register_sampler(id, p) : wgr_nvn_register_sampler(id, p);
}
void rx_nvn_set_texture_pool_size(int n) {
    if (!started) {
        early_call c = {2, n};
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_set_texture_pool_size(n) : wgr_nvn_set_texture_pool_size(n);
}
void rx_nvn_texture_released(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {3, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_texture_released(t) : wgr_nvn_texture_released(t);
}
int rx_output_preview_stage(const nvn_tex *targets) {
    return started && vk && vk->output_preview_stage && vk->output_preview_stage(targets);
}

int rx_output_preview_retired(uint64_t last_serial) {
    return started && vk && vk->output_preview_retired && vk->output_preview_retired(last_serial);
}

int rx_output_trial_phase(uint64_t serial, int cancel) {
    return started && vk && vk->output_trial_phase ? vk->output_trial_phase(serial, cancel) : OUTPUT_TRIAL_EXPIRED;
}

int rx_output_trial_publish(uint64_t serial, void (*apply)(void *, uint64_t), void *state) {
    return started && vk && vk->output_trial_publish && vk->output_trial_publish(serial, apply, state);
}

void rx_nvn_texture_created(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {4, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    if (vk && vk->nvn_texture_created) vk->nvn_texture_created(t);
}
void rx_nvn_program_released(const nvn_prog *p) { vk ? vk->nvn_program_released(p) : wgr_nvn_program_released(p); }
void rx_nvn_program_created(const nvn_prog *p) {
    if (vk && vk->nvn_program_created) vk->nvn_program_created(p);
}
/* (longshot) A texture's texels read or written by the CPU (nvnTextureReadTexels / WriteTexels) wait in the Vulkan back end
   for the GPU to finish everything submitted before (vk_replay.cpp finish), and that work can itself wait for a signal the
   game's main thread makes later (seen with the next frame's work held at OoT's frame limiter). The calling thread held its
   virtual core through that wait, and the main thread's frame needed a worker allowed on that core only: SceneLowPriority's
   WriteTexels at an area change (and BootupBackgroundWorkThread's at the boot) held core 2 for ~1 s (until the present gave
   up waiting for the signal, RX_VK_FRAME_GATE_MS), ModuleSystemWorker2 waited for it and the game stood still for that
   second. The core is given up for the call, as for an invalidate or a remap (below). HOS_TEXELS_KEEP_CORE=1: kept, as
   before (the switches' one rule: 1, true, yes or on) */
static int texels_keep_core(void) {
    static int keep = -1;
    if (keep < 0) keep = env_on("HOS_TEXELS_KEEP_CORE");
    return keep;
}
void rx_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                        size_t row, size_t img) {
    const int away = texels_keep_core() ? 0 : hos_vcore_away();
    vk ? vk->nvn_read_texels(t, level, layer0, r, dst, row, img) : wgr_nvn_read_texels(t, level, layer0, r, dst, row, img);
    if (away) hos_vcore_back();
}
void rx_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                         size_t row, size_t img) {
    if (!started && t && r && src) {   /* (kept with a copy of the data: the caller's buffer is its own again on return) */
        int bw = 1, bh = 1;
        const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (bpb > 0 && bw > 0 && bh > 0 && r->width > 0 && r->height > 0) {
            const uint64_t wb = (uint64_t)((r->width + bw - 1) / bw) * (uint64_t)bpb, hb = (uint64_t)((r->height + bh - 1) / bh);
            const uint64_t depth = r->depth > 0 ? (uint64_t)r->depth : 1;
            /* (the bytes the copy reads: the last row of the last slice ends the data, whatever the strides) */
            const uint64_t n = (depth - 1) * img + (hb - 1) * row + wb;
            early_call c = {5, 0};
            c.t = *t, c.level = level, c.layer0 = layer0, c.r = *r, c.row = row, c.img = img;
            c.data = (uint8_t *)malloc(n);
            if (c.data) {
                memcpy(c.data, src, n);
                if (keep_early(&c)) return;
                free(c.data);
            }
        }
    }
    const int away = texels_keep_core() ? 0 : hos_vcore_away();   /* (the wait for the GPU: texels_keep_core above) */
    vk ? vk->nvn_write_texels(t, level, layer0, r, src, row, img)
       : wgr_nvn_write_texels(t, level, layer0, r, src, row, img);
    if (away) hos_vcore_back();
}
void rx_nvn_invalidate(uint64_t gpu, uint64_t size) {
    /* (longshot) nothing there it can wait for (the back end's lock-free look: nothing ever written back, no texture the GPU
       wrote): done with the core kept, as on the console, where the call is a cache operation. OoT makes it per shape per
       view in its model work, each a release of the core and a wait for one again */
    if (vk && vk->nvn_invalidate_quick && vk->nvn_invalidate_quick(gpu, size)) return;
    /* The callback can wait for replay or GPU write-back: let another guest thread use this core meanwhile. */
    const int away = hos_vcore_away();
    vk ? vk->nvn_invalidate(gpu, size) : wgr_nvn_invalidate(gpu, size);
    if (away) hos_vcore_back();
}
int rx_guest_imported(uint64_t lo, uint64_t hi) {
    /* (mkw-recomp 16bc3bc: it takes the back end's pool lock, which the replay holds at times: no virtual core held) */
    const int away = hos_vcore_away();
    const int r = vk && vk->guest_imported ? vk->guest_imported(lo, hi) : 0;
    if (away) hos_vcore_back();
    return r;
}
/* (nvn_native.c: the CPU set an event's value) in order with the submits, for the GPU's waits on it */
/* (longshot, issue 1113) an input recording's playback answers the Debug tools rows it holds (host/input_movie.c): the
   player's menu changes nothing in a replay, nor the replay the player's menu */
int hos_input_movie_option(int id, int *v);
int rx_option(int id, int otherwise) {
    int v;
    if (hos_input_movie_option(id, &v)) return v;
    return vk && vk->option ? vk->option(id) : otherwise;
}
void rx_option_set(int id, int value) { if (vk && vk->option_set) vk->option_set(id, value); }
/* (longshot) the control bridge (host/control.c): the Vulkan back end's, none without it (0) */
int rx_control_frame_info(int64_t *presents, int64_t *draws, int *width, int *height) {
    if (!vk || !vk->frame_info) return 0;
    vk->frame_info(presents, draws, width, height);
    return 1;
}
int rx_control_shot(const char *path, int64_t after, void (*done)(void *ctx, int64_t present, int ok), void *ctx) {
    return vk && vk->shot ? vk->shot(path, after, done, ctx) : 0;
}
#define CTL_SAME(f) (sizeof ((HosControlSetting *)0)->f == sizeof ((MkwVkSetting *)0)->f)
_Static_assert(CTL_SAME(key) && CTL_SAME(name) && CTL_SAME(text) && CTL_SAME(tab), "control.h and vk_api.h: one setting");
static void control_setting_copy(HosControlSetting *to, const MkwVkSetting *from) {   /* (the same fields, field by field) */
    memcpy(to->key, from->key, sizeof to->key), memcpy(to->name, from->name, sizeof to->name);
    memcpy(to->text, from->text, sizeof to->text), memcpy(to->tab, from->tab, sizeof to->tab);
    to->value = from->value, to->lo = from->lo, to->hi = from->hi, to->step = from->step;
    to->restart = from->restart, to->fixed = from->fixed;
}
int rx_control_setting(int index, HosControlSetting *out) {
    MkwVkSetting s;
    if (!vk || !vk->setting || !vk->setting(index, &s)) return 0;
    control_setting_copy(out, &s);
    return 1;
}
int rx_control_setting_set(const char *key, int value, HosControlSetting *out) {
    MkwVkSetting s;
    memset(&s, 0, sizeof s);
    const int r = vk && vk->setting_set ? vk->setting_set(key, value, &s) : 0;
    if (r) control_setting_copy(out, &s);
    return r;
}
void rx_nvn_event_signal(uint64_t gpu, uint32_t value) {
    if (vk && vk->nvn_event_signal) vk->nvn_event_signal(gpu, value);
}
/* (nvn_native.c: virtual pages about to be unmapped or remapped) the Vulkan back end writes what the GPU drew there into
   their memory first */
void rx_nvn_vmap_changing(uint64_t gpu, uint64_t size) {
    /* (mkw-recomp 16bc3bc: it may wait for the replay and the GPU's texels: no virtual core held meanwhile) */
    const int away = hos_vcore_away();
    if (vk && vk->nvn_vmap_changing) vk->nvn_vmap_changing(gpu, size);
    if (away) hos_vcore_back();
}
void rx_nvn_pool_created(uint64_t gpu, uint64_t size) {
    const MkwVkRenderer *r = vk ? vk : vk_starting;   /* (also while it starts: vk_starting) */
    if (r && r->nvn_pool_created) r->nvn_pool_created(gpu, size);
}
void rx_nvn_pool_finalized(uint64_t gpu, uint64_t size) {
    const MkwVkRenderer *r = vk ? vk : vk_starting;
    if (r && r->nvn_pool_finalized) r->nvn_pool_finalized(gpu, size);
}
