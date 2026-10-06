# Heyaki Adapter 层（设计第 8、14 节）：把 Heyaki 能力转换为应用语义。
# 真实 Heyaki 依赖的接入随 DEC-003（来源锁定）与 DEC-006（API 契约）解锁；
# M1 先以 SPI 抽象接口 + FakeHeyakiAdapter 交付。
#
# M7/DEC-028：relay enrollment 的证书 pin 计算直接消费 OpenSSL
#（heyaki/session/relay_enrollment.hpp 的 relay_certificate_pin）。
# imported target 作用域限定在调用 find_package 的目录子树（第三方
# heyaki 子目录内的 find 不传导到兄弟目录），此处显式查找；版本要求与
# heyaki M3A 冻结一致（3.x ABI 线）。产品面 heyaki::client 链接不变。
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
aki_add_skeleton_library(aki_heyaki)
target_link_libraries(aki_heyaki INTERFACE OpenSSL::SSL OpenSSL::Crypto)
