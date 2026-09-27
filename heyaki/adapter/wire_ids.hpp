// Wire 标识生成（M5-05；§6.1/DEC-010 TransferId 生成入口收敛 + MessageId
// 生成入口同型落地）。
//
// UI 出站面（UiActions）发起文本/图片发送时需要规范形式的 MessageId/TransferId：
// 非规范串在真实 NodeSession 的 to_heyaki_*_id 双射处被拒（admission false，
// 消息行记 Failed）——页面层不携带 wire 编码知识（RULE-10），生成收敛于本
// 头文件，经 make_ui_actions 绑定注入（ui/models，组合根形态）。
//
// 与 local_identity.hpp 同款纪律：INTERFACE 头文件库内的单头 inline 实现
//（消费方链接 heyaki::core 提供 include 路径与 to_string 符号）；
// `std::random_device` 为标准库（NodeSession::new_transfer_id M4-04 先例）。
// 生成规则：16 随机字节（全零重抽）→ ::heyaki::to_string 规范串
//（MessageId `hym1_` / TransferId `hyt1_` 前缀 + 26 base32 字符）。
#pragma once

#include "conversation/message/message_types.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <heyaki/ids.hpp>

#include <array>
#include <cstddef>
#include <random>

namespace aki::heyaki {

template <typename WireId, typename AkiId>
[[nodiscard]] AkiId new_identifier() {
    std::random_device random;
    for (;;) {
        typename WireId::Storage bytes{};
        for (auto& byte : bytes) {
            byte = static_cast<std::byte>(random());
        }
        const WireId candidate{bytes};
        if (!candidate.is_zero()) {
            return AkiId{::heyaki::to_string(candidate)};
        }
    }
}

// 规范 MessageId（hym1_ 前缀；真实 wire 双射可解析）。
[[nodiscard]] inline aki::conversation::MessageId new_message_id() {
    return new_identifier<::heyaki::MessageId, aki::conversation::MessageId>();
}

// 规范 TransferId（hyt1_ 前缀；§6.1/DEC-011④ 冻结生成规则——
// NodeSession::new_transfer_id 委托至此，生成入口单一）。
[[nodiscard]] inline aki::transfer::TransferId new_transfer_id() {
    return new_identifier<::heyaki::TransferId, aki::transfer::TransferId>();
}

}  // namespace aki::heyaki
