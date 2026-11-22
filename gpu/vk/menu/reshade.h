#pragma once

namespace mkwvk {
namespace ui { struct Ctx; }
namespace menu_hit { struct Hit; struct Map; }

namespace reshade_menu {
bool comparing();
void draw_comparison(ui::Ctx& c, menu_hit::Map& hits);
bool update_comparison(int horizontal, int vertical, bool confirm, bool cancel, const menu_hit::Hit& click);
}
}
