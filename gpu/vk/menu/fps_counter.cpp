// The Insert menu's FPS counter rows (#1633): on the Gameplay tab, first (before Menu size), the counter's level, corner and size
// and the row that opens its details page; on that page, the version at every level and each of the Dev level's lines
// (opts.fps_dev_show, in fps_stats.h's DevLine order, then the footer's CPU, GPU and driver, and render lines).
#include "menu_module.h"

#include "fps_stats.h"
#include <string>

namespace mkwvk {

namespace {
const char* tx_fps_level(int v) { return fps_stats::level_name(v); }
const char* tx_fps_corner(int v) { return fps_stats::corner_name(v); }
const char* tx_fps_size(int v) {   // (the FPS counter size row: Auto, then the Menu size row's names)
    static const char* t[] = {"Auto", "Small", "Medium", "Large"};
    return v >= 0 && v <= 3 ? t[v] : t[0];
}
MODULE_HINTS(hn_fps, 0, nullptr, "The frame rate. With frame generation on, the game's own rate too.",
             "And the game's frame time, with its 1% low: the mean time of its slowest 1% of frames over the last 10 seconds.",
             "And a graph of the last 4 seconds' frame times, with a line at the Game frame rate cap's.",
             "And where each frame's time goes, as colours in the graph: the game, the renderer, its waits on the render thread "
             "and the GPU, pacing (frame rate cap, vsync); with threaded rendering, a lane for the render thread's time; the "
             "GPU's own time; red marks where shaders were built or draws left out.",
             "And, for debugging, the version, the CPU, GPU and driver, and the renderer's own numbers: video memory, textures, draws, "
             "dispatches and render passes a frame, draws skipped, texture uploads and pipelines built.")
const char* const kFpsDevLineHelp = "Whether the FPS counter's Dev level shows this line.";   // (the details page's rows)
} // namespace

MENU_MODULE(fps_counter) {
    menu::tab("gameplay")
        .row({.key = "fps_counter",
              .label = "FPS counter",
              .help = "Frames per second in the top right corner, and as much more as you choose.",
              .value = &opts.show_fps,
              .lo = 0, .hi = fps_stats::kLevels - 1,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_fps_level(v)); },
              .hint = hn_fps}, menu::before("bug_report").order(100))
        .row({.key = "fps_counter_position",
              .label = "FPS counter position",
              .help = "The corner the FPS counter shows in. While this menu is open it shows at the top right.",
              .value = &opts.fps_corner,
              .lo = 0, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_fps_corner(v)); }}, menu::before("bug_report").order(110))
        .row({.key = "fps_counter_size",
              .label = "FPS counter size",
              .help = "How large the FPS counter is drawn. Auto grows with the window, as the menu does; Small, Medium and Large keep one size "
                      "whatever the window, the size Auto has in a 720p, 1080p and 1440p window.",
              .value = &opts.fps_size,
              .lo = 0, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_fps_size(v)); }}, menu::before("bug_report").order(120))
        .row({.key = "@FPS counter details",
              .label = "FPS counter details",
              .help = "Which lines the FPS counter's Dev level shows, and whether the version and hardware show at every level.",
              .kind = menu::Kind::Page,
              .opens = "fps_details"}, menu::before("bug_report").order(130));
    menu::page("fps_details")
        .row({.key = "version_and_hardware_at_every_level",
              .label = "Version and hardware at every level",
              .help = "The version, as this menu's banner shows it, the CPU, the GPU and its driver, and the render size and upscaler at the foot of the FPS counter at every "
                      "level, not only at Dev: for screenshots and recordings. The CPU, GPU and render rows below still turn those off.",
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_version_always,
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "vram_line",
              .label = "VRAM line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[0],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "textures_line",
              .label = "Textures line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[1],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "draws_line",
              .label = "Draws line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[2],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "dispatches_line",
              .label = "Dispatches line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[3],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "render_passes_line",
              .label = "Render passes line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[4],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "draws_skipped_line",
              .label = "Draws skipped line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[5],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "texture_uploads_line",
              .label = "Texture uploads line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[6],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "pipelines_built_line",
              .label = "Pipelines built line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[7],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "gpu_slot_waits_line",
              .label = "GPU slot waits line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[8],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "queue_submits_line",
              .label = "Queue submits line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[9],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "write-backs_line",
              .label = "Write-backs line",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[10],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "cpu_and_gpu_lines",
              .label = "CPU and GPU lines",
              .help = kFpsDevLineHelp,
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[11],
              .lo = 0, .hi = 1,
              .preset_exempt = true})
        .row({.key = "render_lines",
              .label = "Render lines",
              .help = "Whether the FPS counter's footer shows the resolution the 3D is drawn at and what upscales it, as the last "
                      "frame drew them.",
              .kind = menu::Kind::Toggle,
              .value = &opts.fps_dev_show[12],
              .lo = 0, .hi = 1,
              .preset_exempt = true});
}

} // namespace mkwvk
