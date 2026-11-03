#include "kid_app/home_menu.h"

namespace kid_app {

bool HomeMenu::Move(int delta) {
    int next = static_cast<int>(index_) + delta;
    if (next < 0) next = 0;
    if (next >= static_cast<int>(kItemCount)) next = static_cast<int>(kItemCount) - 1;
    if (next == index_) return false;
    index_ = static_cast<uint8_t>(next);
    return true;
}

bool HomeMenu::Select(uint8_t index) {
    if (index >= kItemCount) return false;
    index_ = index;
    return true;
}

}  // namespace kid_app
