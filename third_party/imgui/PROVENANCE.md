Dear ImGui v1.92.9b, from https://github.com/ocornut/imgui (tag v1.92.9b, archive sha256
21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99), unmodified.
MIT license: LICENSE.txt.

Kept: the core (imgui*.cpp, imgui*.h, imstb_*.h, imconfig.h), the Vulkan renderer backend
(backends/imgui_impl_vulkan.*), and the SDL2 platform and SDL_Renderer backends (backends/imgui_impl_sdl2.*,
backends/imgui_impl_sdlrenderer2.*: added 2026-11-17 for the setup AppImage's window, builder/linux/setup_ui, from the same
tag). Left out: the demo, the examples, the docs, the other backends and misc/.
Settings are given as compiler defines by nx2/build.py, not by editing imconfig.h.
Used by gpu/vk/vk_imgui.cpp and vk_menu.cpp: the Insert menu, the FPS counter and the start-up screen, drawn over the
window's image. ImGui's own widgets and input are not used: the menu draws on its draw lists and keeps its own keys, gamepad
and mouse. Textures go to the GPU through vk_imgui.cpp's own copies, not the backend's (which waits on the queue).
The setup window (builder/linux/setup_ui/setup_ui.cpp, ARM64 Linux) uses ImGui's own widgets, with its gamepad navigation,
through the SDL2 and SDL_Renderer backends.

upstream: https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
upstream-sha256: 21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
upstream-root: imgui-1.92.9b

tree-sha256: aa52d45857708d84a5b9048660ab908fbf22f806a570a6fa96d0bf791a14c712
