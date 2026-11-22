// Game-free documentation capture. Compile the maintained menu in this translation unit;
// never initialize the game, the Vulkan renderer, menu persistence or game-provided fonts.
#include "gpu/vk/vk_menu.cpp"
#include "gpu/vk/vk_fsr4.h"   // (Fsr4Model, for the FSR 4 model row's stand-ins below)
#include <SDL.h>
#include <png.h>
#include <imgui_internal.h>
#include <limits>
#include <memory>
#include <stdexcept>

// Any accidental attempt to resolve a game directory fails closed.
extern "C" DWORD GetModuleFileNameA(HMODULE, LPSTR, DWORD) { std::abort(); }
extern "C" DWORD GetFileAttributesA(LPCSTR) { std::abort(); }
extern "C" HANDLE FindFirstFileA(LPCSTR, WIN32_FIND_DATAA*) { std::abort(); }
extern "C" BOOL FindNextFileA(HANDLE, WIN32_FIND_DATAA*) { std::abort(); }
extern "C" BOOL FindClose(HANDLE) { std::abort(); }
extern "C" BOOL PostMessageA(HWND, UINT, WPARAM, LPARAM) { std::abort(); }   // (there is no window to post to)

// Explicit fixture capabilities: no GPU-specific upscaler or latency SDK. These are
// display inputs, not probes of the capture machine or claims about supported hardware.
namespace mkwvk {
// No host: no callback is set, so no row has a tag, a mod, a monitor or a microphone to list. The NFC Scan row asks the host
// whether a scan is under way until its text says no more, so the host's scan is idle.
static int idle_scan(int) { return 0; }
static MkwVkHost empty_host = [] { MkwVkHost h{}; h.nfc_scan = idle_scan; return h; }();
const MkwVkHost* host = &empty_host;
Device dev{}; // No physical device: hide hardware-specific Debug actions in this UI-only fixture.
std::atomic<bool> present_native_pq{false}; // Synthetic SDR presentation, never a live HDR surface.
void logf(const char*, ...) {}
std::vector<std::string> gpu_device_names;
std::vector<gpu_choice::Card> gpu_device_cards;
std::string options_path() { std::abort(); } // Capture never reads or writes player settings.
bool dlss_built() { return false; }
DlssWhy dlss_why() { return DLSS_NOT_BUILT; }
uint64_t device_vram_bytes() { return 0; } // No physical device: the DLSS model row as on an unknown card (K).
int dlss_fg_skip(unsigned* most) { if (most) *most = 0; return FG_SKIP_NOT_BUILT; }
int dlss_fg_unavailable() { return FG_SKIP_NOT_BUILT; }
bool dlss_fg_forced() { return false; }
int dlss_fg_most() { return 0; }   // (no DLSS FG top: the Frame generation row's own values)
int framegen_capped(int framegen, int) { return framegen; }
// No FSR 4 (the FSR 4 model row): no switch over it, no Lite, nothing to set
int fsr4_model_env() { return -1; }
bool fsr4_lite_available() { return false; }
void fsr4_set_model(Fsr4Model) {}
// No ReShade runtime (gpu/vk/menu/reshade.cpp's rows): Off, no user presets, nothing to preview
namespace reshade {
void select(int) {}
void preview_bypass(bool) {}
void preview_select(int, std::string) {}
void preview_end(int) {}
void select_user(std::string) {}
std::vector<std::string> user_presets() { return {}; }
std::string user_selection() { return {}; }
std::string status() { return {}; }
}   // namespace reshade
bool fsr4_built() { return false; }
FsrWhy fsr4_why() { return FSR_NOT_BUILT; }
bool xess_built() { return false; }
XessWhy xess_why() { return XESS_NOT_BUILT; }
bool fsr3_wanted() { return false; }
namespace fsr3 { bool supported() { return false; } }
namespace fsr3fi { uint32_t frames_wanted() { return 0; } }
bool reflex_available() { return false; }
bool antilag_available() { return false; }
int temporal_mode() { return 0; }
bool temporal_off() { return false; }
FgMaker framegen_maker() { return static_cast<FgMaker>(0); }
namespace bug_report {
State state() { return kIdle; }
std::string shown() { return {}; }
void fill_here(Inputs&) {}   // (the menu's Bug report row is drawn, never run)
}
// What the menu's rows ask the renderer for when they act or describe the device: no window, no device, nothing listed.
HWND app_window() { return nullptr; }
std::string driver_text() { return {}; }
std::string device_report_text() { return {}; }
std::string state_list_snapshot(bool* present) { if (present) *present = false; return {}; }
void samplers_changed() {}
void menu_debug_rows(int, int) {}
}

// The banner's logo, as vk_imgui.cpp makes it for the renderer: assets/longshot.png's straight RGBA pixels in a colour texture
// (null until main() loads it). render() below uploads it to SDL like the font atlas.
namespace mkwvk::ui {
static ImTextureData* banner_logo = nullptr;
ImTextureData* logo(float* aspect) {
    *aspect = banner_logo ? float(banner_logo->Width) / banner_logo->Height : 1.f;
    return banner_logo;
}
}

static void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

struct SdlSession {
    ~SdlSession() { SDL_Quit(); }
};

struct BannerTexture {
    ImTextureData texture{};

    explicit BannerTexture(const char* filename) {
        png_image logo{};
        const std::unique_ptr<png_image, decltype(&png_image_free)> read(&logo, png_image_free);
        logo.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_file(&logo, filename)) throw std::runtime_error(logo.message);
        constexpr png_uint_32 max_side = 4096;
        if (!logo.width || !logo.height || logo.width > max_side || logo.height > max_side ||
            uint64_t(logo.width) * logo.height * 4 > uint64_t(std::numeric_limits<int>::max()))
            throw std::runtime_error("banner logo dimensions exceed the supported RGBA texture size");
        logo.format = PNG_FORMAT_RGBA;
        texture.Create(ImTextureFormat_RGBA32, int(logo.width), int(logo.height));
        if (!png_image_finish_read(&logo, nullptr, texture.GetPixels(), 0, nullptr))
            throw std::runtime_error(logo.message);
        texture.UseColors = true;
        ImGui::RegisterUserTexture(&texture);
        mkwvk::ui::banner_logo = &texture;
    }

    ~BannerTexture() {
        ImGui::UnregisterUserTexture(&texture);
        mkwvk::ui::banner_logo = nullptr;
    }
};

// SDL's software renderer consumes the real ImGui triangles, clip rectangles and
// dynamic font atlas. No recreation of menu labels, rows or layout lives here.
static void render(SDL_Renderer* renderer, ImDrawData* data) {
    for (ImTextureData* t : *data->Textures) {
        if (t->Status == ImTextureStatus_WantDestroy) {
            SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(t->GetTexID()));
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
            continue;
        }
        if (t->Status != ImTextureStatus_WantCreate && t->Status != ImTextureStatus_WantUpdates) continue;
        check(t->Format == ImTextureFormat_RGBA32, "RGBA font atlas required");
        auto* texture = reinterpret_cast<SDL_Texture*>(t->GetTexID());
        if (t->Status == ImTextureStatus_WantCreate) {
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, t->Width, t->Height);
            check(texture != nullptr, "create atlas texture");
            check(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) == 0, "atlas blending");
            check(SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear) == 0, "atlas filtering");
            t->SetTexID(reinterpret_cast<ImTextureID>(texture));
        }
        check(SDL_UpdateTexture(texture, nullptr, t->GetPixels(), t->GetPitch()) == 0, "upload atlas");
        t->SetStatus(ImTextureStatus_OK);
    }
    for (const ImDrawList* list : data->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            check(cmd.UserCallback == nullptr, "unexpected drawing callback");
            SDL_Rect clip{std::max(0, int(cmd.ClipRect.x)), std::max(0, int(cmd.ClipRect.y)),
                          int(std::min(data->DisplaySize.x, cmd.ClipRect.z)) - std::max(0, int(cmd.ClipRect.x)),
                          int(std::min(data->DisplaySize.y, cmd.ClipRect.w)) - std::max(0, int(cmd.ClipRect.y))};
            if (clip.w <= 0 || clip.h <= 0) continue;
            check(SDL_RenderSetClipRect(renderer, &clip) == 0, "clip rectangle");
            const ImDrawVert* v = list->VtxBuffer.Data + cmd.VtxOffset;
            check(SDL_RenderGeometryRaw(renderer, reinterpret_cast<SDL_Texture*>(cmd.GetTexID()),
                &v->pos.x, sizeof(ImDrawVert), reinterpret_cast<const SDL_Color*>(&v->col), sizeof(ImDrawVert),
                &v->uv.x, sizeof(ImDrawVert), list->VtxBuffer.Size - cmd.VtxOffset,
                list->IdxBuffer.Data + cmd.IdxOffset, cmd.ElemCount, sizeof(ImDrawIdx)) == 0, "menu geometry");
        }
    }
    SDL_RenderSetClipRect(renderer, nullptr);
    SDL_RenderPresent(renderer);
}

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: menu-capture FONT_DIRECTORY LOGO.png graphics|graphics-screen|audio|controls|startup[-classic] OUTPUT.png\n");
        return 2;
    }
    try {
        std::string tab = argv[3];
        const bool classic = tab.size() > 8 && tab.compare(tab.size() - 8, 8, "-classic") == 0;   // (the Classic Menu theme)
        if (classic) tab.resize(tab.size() - 8);
        if (tab != "graphics" && tab != "graphics-screen" && tab != "audio" && tab != "controls" && tab != "startup")
            throw std::runtime_error("unsupported view");
        constexpr int width = 1280, height = 720;
        check(SDL_Init(0) == 0, "SDL initialization");
        const SdlSession sdl;
        const std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(
            SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32), SDL_FreeSurface);
        check(surface != nullptr, "capture surface");
        const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> software(
            SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
        check(software != nullptr, "software renderer");
        SDL_Renderer* renderer = software.get();
        const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(
            ImGui::CreateContext(), ImGui::DestroyContext);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(width, height);
        io.DeltaTime = 1.f / 60;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        auto font = [&](const char* name) {
            std::string path = std::string(argv[1]) + "/" + name;
            ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), 18);
            if (!f) throw std::runtime_error("missing bundled font: " + path);
            return f;
        };
        mkwvk::ui::Fonts fonts{font("LibertinusSans-Regular.ttf"), font("LibertinusSans-Bold.ttf"), font("LibertinusSerif-Semibold.ttf")};
        const BannerTexture banner(argv[2]);
        mkwvk::opts.menu_lang = 1; // English; do not read language.txt or environment-selected game language.
        mkwvk::cur_tab = tab == "audio" ? mkwvk::TAB_AUDIO : tab == "controls" ? mkwvk::TAB_CONTROLS : mkwvk::TAB_GRAPHICS;
        mkwvk::opts.ultrawide = 0; // Explicit 16:9, independent of any real monitor.
        mkwvk::outputs_sync();
        // Snapshot these fixture values as the starting state, without restart_init's language-file read.
        for (int i = 0; i < (int)std::size(mkwvk::rows); ++i) {
            auto& row = mkwvk::rows[i];
            int slot = row.restart && row.value ? mkwvk::restart_state.add(mkwvk::row_restart_value(row)) : -1;
            mkwvk::restart_slot[i] = slot;
            if (slot >= 0) mkwvk::restart_names[slot] = row.name;
        }
        while (mkwvk::rows[mkwvk::tab_row_to_global(mkwvk::cur_tab, mkwvk::tab_selected[mkwvk::cur_tab])].heading)
            ++mkwvk::tab_selected[mkwvk::cur_tab];
        if (tab == "graphics-screen") {
            bool found = false;
            for (int i = 0; i < mkwvk::tab_row_count(mkwvk::cur_tab); ++i) {
                const auto& row = mkwvk::rows[mkwvk::tab_row_to_global(mkwvk::cur_tab, i)];
                if (!row.heading && std::string(row.name) == "Fullscreen") {
                    mkwvk::tab_selected[mkwvk::cur_tab] = i;
                    // A settled Screen view, rather than an off-screen selection or scroll animation.
                    mkwvk::scroll_at[mkwvk::cur_tab] = mkwvk::scroll_want[mkwvk::cur_tab] = (float)std::max(0, i - 1);
                    mkwvk::ease_step = 1.f;
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("Fullscreen row missing from Graphics capture source");
        }
        // Match the renderer's first unscrolled menu view; no real options file is opened.
        float safe = 0;
        float scale = mkwvk::ui::unit(height, mkwvk::opts.menu_size, &safe);
        if (classic) mkwvk::opts.menu_theme = mkwvk::ui::kClassic;
        mkwvk::ui::set_theme(mkwvk::opts.menu_theme);
        if (tab == "startup")   // (the start-up screen part way through its first step, with illustrative counts)
            mkwvk::startup = {mkwvk::version_text(LONGSHOT_VERSION), "STEP 1 OF 2", "Preparing shaders", "32%", "12,345 / 38,809 shaders",
                              "about 40 s left", "", 0.32};   // (the Menu theme's palette, the Options default, as ui_ctx sets it)
        for (int frame = 0; frame < 3; ++frame) {
            SDL_SetRenderDrawColor(renderer, 16, 18, 21, 255);
            SDL_RenderClear(renderer);
            ImGui::NewFrame();
            mkwvk::ui::Ctx c{ImGui::GetBackgroundDrawList(), fonts, scale, width, height, safe, false};
            if (tab == "startup") mkwvk::draw_startup(c);
            else mkwvk::draw_menu(c);
            ImGui::Render();
            render(renderer, ImGui::GetDrawData());
        }
        bool drew_logo = false;
        for (const auto* list : ImGui::GetDrawData()->CmdLists)
            for (const auto& cmd : list->CmdBuffer)
                if (cmd.ElemCount && cmd.GetTexID() == banner.texture.GetTexID()) drew_logo = true;
        if (!drew_logo) throw std::runtime_error("Graphics guide banner did not draw the logo");
        std::vector<unsigned char> rgba(width * height * 4);
        check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, rgba.data(), width * 4) == 0, "capture pixels");
        png_image png{};
        png.version = PNG_IMAGE_VERSION; png.width = width; png.height = height; png.format = PNG_FORMAT_RGBA;
        if (!png_image_write_to_file(&png, argv[4], 0, rgba.data(), width * 4, nullptr)) throw std::runtime_error(png.message);
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 1; }
}
