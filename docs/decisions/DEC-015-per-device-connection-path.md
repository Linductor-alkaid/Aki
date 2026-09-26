# DEC-015：逐设备连接路径状态模型

> 状态：Accepted
> 日期：2026-09-27
> 负责人：Linductor
> 冻结里程碑：M5-04 开工编码前（Devices 页 SCOPE-04「连接方式」行属性）
> 替代/被替代：退役 M1-06 起的全局 `SetConnectionPath` 更新类型与
> `LatestMailbox<ConnectionPath>` 单值摘要（同变更内移除）

## 背景与问题

SCOPE-04 把「连接方式」列为设备行属性（与名称/类型/OS/在线状态并列），
M5-08 退出-1 要求 SCOPE-01~12 逐项归档。现状态模型把连接路径压平为全局
单值：`SetConnectionPath{path}`（无 device 字段）落
`LatestMailbox<ConnectionPath>`，消费侧（M5-03 `consume_ui_state`）把这一个
值 stamp 到每一行。且该全局值本身常为陈旧/错误：peer_sessions diff 只在
「已认证→已认证且路径变化」时发路径事件，初连只发不带路径的
`on_connected`（宿主把初连路径硬编码为 Lan）；`LatestMailbox` 初值 Unknown
只会被「中途换路」事件更新——稳定连接的设备列表将显示 Unknown 或**别的
设备**最后一次换路的值。逐设备路径数据在源头就存在（PeerSessionView 逐
peer 携带 data_path/signaling_route，diff 产出逐设备事件），「降级」是自家
状态边界的建模选择而非数据限制。

## 决策

- **DeviceStore 增逐设备易失路径集合**：
  `DeviceConnectionPathEntry{DeviceId device; ConnectionPath path}` 向量
  （设备预算 256，线性查找可预算），放在 DeviceStore 级而非
  DeviceIdentity 字段——`apply_impl(UpsertDevice)` 是整行替换，路径放
  identity 字段会被发现事件载荷覆写；仅由新更新类型写入。
- **新增部分更新类型 `SetDeviceConnectionPath{device, path}`**（SetPresence
  先例）：未知 device id 明确拒绝（计入 updates_rejected，RULE-09 可观测）；
  已知 id 按设备键 upsert 条目并置快照脏。连接路径**不持久化**（沿
  SetPresence 易失先例，恢复后默认无条目=Unknown）。
- **退役全局摘要**：删除 `SetConnectionPath` 更新类型与
  `LatestMailbox<ConnectionPath>` 成员及其访问器/统计——同一事实两个来源
  需消费侧定优先级、永久双水位复杂化；退役是净简化。保留
  `ConnectionPathChangedEvent`（主路径诊断事件）与 SPI
  `on_connection_path_changed`（`from` 为诊断信息，`SetDeviceConnectionPath`
  只需 `to`）。
- **断连置 Unknown**：路径是当前会话属性，离线不展示陈旧路径——
  DeviceManager 断连处理同批提交 `SetDeviceConnectionPath{device, Unknown}`。
- **初连路径补发**：`PeerSessionEvents::on_connected` 增携带映射路径
  （diff 时 `data_path`/`signaling_route` 在手），宿主删除初连 Lan 硬编码。
- 组件选型无 executor 能力缺口（无需 9.4 台账）：LatestMailbox 为单槽
  latest-wins、无 per-key 维度（pinned 通信卡），逐设备状态的正确组件本就
  是既有 `DoubleBuffer<AppState>` 快照；drain 每批至多发布一次快照，路径
  更新被自然合并不引入发布风暴。

## 备选方案

- **B（否决）：全局摘要降级展示 + 登记 SCOPE-04 偏差**——展示值跨设备错误
  归因（两台设备路径不同时至少一行是错的）；全局值本身常为 Unknown/陈旧
  （初连不发路径事件 + Lan 硬编码），要变「对」同样必须修管线，管线修复是
  两方案共同成本；SCOPE-04 是可控建模选择而非 M3-09/M4-07 那类不可控环境
  限制，挂偏差违反完成定义且污染会话头部路径徽标（aki_ui_design §3）。
- **子备选 1（否决）：路径放 DeviceIdentity 字段镜像 presence**——
  UpsertDevice 整行替换，连接中被重新发现上报时易失字段会被覆写。
- **子备选 2（否决）：保留全局 mailbox + 新增逐设备双轨**——同一事实两个
  来源，消费侧需定优先级、永久双水位复杂化。
- **子备选 3（否决）：每设备一个 LatestMailbox**——组件形态错误（单槽
  latest-wins 无 per-key 维度），且 map of channels 失去快照合并性质。

## 影响与风险

- 同一变更内同步：app_state（DeviceStore）、app_state_updates
  （增/删类型）、app_state_owner（成员/访问器/apply）、device_manager
  （Connected 提交路径 + Disconnected 置 Unknown + Changed 改逐设备）、
  peer_sessions_pipeline（on_connected 增路径）、宿主管线钩子（删 Lan
  硬编码）、ui/models（派生改 store 内 join、删路径水位分支）、受影响测试
  （test_app_state/test_app_managers/test_peer_sessions_loopback/
  test_heyaki_adapter/test_ui_models/test_peer_sessions_pipeline）与设计
  §8.3 路由表/§9.1 消费面/§10.1 comm 映射文字。
- 派生测试必须覆盖「两台设备两条不同路径同时正确展示」（B 无法表达的形态）
  与 连接→换路→断连→重连 全序列。
- 关联发现（另行登记，不在本决策范围）：UpsertDevice 整行替换对易失字段
  presence 的覆写隐患（连接中被重新发现可能 Online→Offline）——map 形态
  使路径免疫，presence 仍在替换集内。
- 已知存量消费口径：全局 mailbox 的「不洪泛」性质由 drain 合并保持
  （test_peer_sessions_loopback 原断言语义由逐设备 upsert 同样满足）。

## 验证方式

M5-04 实施并验证：①单测「两设备两路径同时正确」+ 连接→换路→断连→重连
序列；②未知 id 拒绝计入 updates_rejected；③快照发布频次经
snapshots_published 确认无风暴；④全局 mailbox 移除后全图编译零残留
（grep SetConnectionPath/try_load_connection_path 零命中）；⑤debug/release
全量 ctest 零回归；⑥Devices 页本机截图（两行不同路径徽标需双端环境，
单端以本地身份行 + 语义色对表呈现，双端归 M5-08）。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)§8.3（路由表）、§9.1（消费面）、
  §10.1（comm 映射）、§4（设备行展示）
- [DEC-006](DEC-006-heyaki-api-contract.md)（映射 5 路径事件——事件面不变）
- [M5：EUI-NEO UI 与 MVP 验收](../plans/m5-eui-neo-ui-mvp.md)（`M5-04`；
  SCOPE-04/SCOPE-10）
