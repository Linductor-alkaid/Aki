# Heyaki Adapter 层（设计第 8、14 节）：把 Heyaki 能力转换为应用语义。
# 真实 Heyaki 依赖的接入随 DEC-003（来源锁定）与 DEC-006（API 契约）解锁；
# M1 先以 SPI 抽象接口 + FakeHeyakiAdapter 交付。
aki_add_skeleton_library(aki_heyaki)
