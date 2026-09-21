// M0 骨架冒烟测试：验证各领域 include 路径与编译边界（设计第 14 节目录结构）。
// 正式测试框架随 DEC-007 在 M1 引入。
#include <string>

#include "app/skeleton.hpp"
#include "conversation/skeleton.hpp"
#include "device/skeleton.hpp"
#include "heyaki/skeleton.hpp"
#include "persistence/skeleton.hpp"
#include "transfer/skeleton.hpp"
#include "ui/skeleton.hpp"

int main() {
    std::string note = aki::device::kSkeletonNote;
    note += aki::app::kSkeletonNote;
    note += aki::conversation::kSkeletonNote;
    note += aki::transfer::kSkeletonNote;
    note += aki::persistence::kSkeletonNote;
    note += aki::heyaki::kSkeletonNote;
    note += aki::ui::kSkeletonNote;
    return note.empty() ? 1 : 0;
}
