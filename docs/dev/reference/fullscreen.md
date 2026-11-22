# Fullscreen monitor

The Graphics tab's **Fullscreen monitor** row picks the screen that borderless fullscreen uses. **Auto (current
monitor)** uses the window's current screen. Fullscreen is borderless, not an exclusive display mode, and the choice
is a screen, not a GPU.

## Behaviour

- **Fullscreen**, F11 and Alt+Enter all go through the same host transition.
- While windowed, a change sets the screen for the next fullscreen. While fullscreen, it moves the game at once.
  Leaving fullscreen restores what was saved when fullscreen began (Windows: the whole window placement; Linux: the
  position).
- A saved screen that is not connected stays selected, shown as `<name> (not connected)`; the current screen is used
  until it comes back.
- A saved identity that matches two screens is never resolved to one of them at random (`hos_monitor_find` refuses
  it). On Linux, screens with the same SDL name get no identity and are left out of the row.
- Screen size, aspect hints and mode lists are refreshed whenever the actual screen changes, including between two
  screens of the same size.

## Saved identity

The choice is saved in `longshot_options.ini` as `fullscreen_monitor_id` (the identity) and `fullscreen_monitor_name`
(the label, for the "not connected" text). List positions, monitor handles and SDL display ids are never saved.
`HOS_FULLSCREEN` overrides the saved `fullscreen=` row at start.

| Platform | Identity | Label |
|---|---|---|
| Windows (`host/display.c`) | The monitor's device interface path (`EnumDisplayDevicesW`'s `DeviceID`) | The friendly name from `DisplayConfigGetDeviceInfo`, else the driver's description |
| Linux (`host/hos_sdl.c`, `host/posix/display_sdl.c`) | `sdl<2|3>:<video driver>:<display name>` | `SDL_GetDisplayName` |

On Linux an identity depends on the SDL major version and video driver, and on the output's name: renaming an output or
switching SDL backend makes a saved choice unavailable. On Wayland, targeting goes through SDL's display targeting,
and the compositor may move the window later or refuse; the published current screen is the one observed. With SDL2,
the host leaves fullscreen, moves, and enters again, since SDL2 ignores a fullscreen request whose flags have not
changed.

## Interface

The host publishes a copied snapshot of the screens; the renderer's menu reads it and queues a preference; the window
thread does every platform window call. No callback calls SDL.

| Piece | Where |
|---|---|
| `MkwVkMonitor`: opaque `id`, UTF-8 `name`, desktop `width`/`height`, `HOS_MONITOR_CURRENT`/`HOS_MONITOR_PRIMARY` flags | [`gpu/vk/vk_api.h`](../../../gpu/vk/vk_api.h) |
| `MkwVkHost` callbacks `monitor_count`, `monitor_info`, `monitor_select` (added in API version 29) | `gpu/vk/vk_api.h` |
| Snapshot, saved preference, lookup that refuses duplicates (`hos_monitor_find`) | [`host/fullscreen_monitor.h`](../../../host/fullscreen_monitor.h), [`host/fullscreen_monitor.c`](../../../host/fullscreen_monitor.c) |
| The row and its saved keys | `gpu/vk/menu/display.cpp` |

`tests/test_fullscreen_monitor_menu.py` and `tests/test_fullscreen_monitor_platform.py` compile the menu, policy and
transition code against stand-in window and SDL functions; they do not open a window.
