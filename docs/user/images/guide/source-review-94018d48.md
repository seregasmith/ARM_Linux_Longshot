# Graphics guide image: source review against 94018d48

Retain `settings-graphics.png` after reviewing the eight recorded sources changed since the review at main `a2dc5385`,
through `0e32bc100`. The image shows the first Graphics view with Quality preset selected; its rows, selected help,
tab strip and keyboard footer remain unchanged. The guide describes this as an illustrative UI-only capture.

| Source | Change and retention decision |
|---|---|
| `gpu/vk/device_policy.h` | GPU architecture classification, BC copy policy and Threaded rendering Auto policy change; none changes the pictured rows or their selected help. |
| `gpu/vk/menu/threads.cpp` | Threaded rendering Auto's help now says both threads start at four cores. Its row is below the pictured view; the guide's setting table agrees. |
| `gpu/vk/threaded_rendering.h` | Auto now picks both threads from four cores; its row is below the pictured view. |
| `gpu/vk/vk_api.h` | Adds the Windows Vulkan loader resolver to the host interface; no pictured UI change. |
| `gpu/vk/vk_dlss.h` | Documents retrying a DLSS refusal for lack of video memory; no pictured UI change. |
| `gpu/vk/vk_menu.cpp`, `gpu/vk/vk_menu.h`, `gpu/vk/vk_recorder.h` | Comments reflect the Threaded rendering Auto change; no pictured UI change. |

The PNG, capture provenance and fixture fingerprints are unchanged. No new rendering, pixel comparison, game run or
capture is claimed. The review revision is the SHA-256 of the sorted `<path> <sha256>` lines for all 131 reviewed
source files, with a final newline.
