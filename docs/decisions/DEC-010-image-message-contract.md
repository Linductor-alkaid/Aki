# DEC-010：图片消息 wire 契约与收发状态联动

> 状态：Accepted
> 日期：2026-09-26
> 负责人：Linductor
> 冻结里程碑：M4（`M4-03` 实现前冻结；本记录即冻结，调研依据见文末）
> 替代/被替代：无（对 [DEC-006](DEC-006-heyaki-api-contract.md) 冻结常量与
> 映射 4 作 M4-03 扩展，扩展细节以本记录与设计 §6.1 为权威）

## 背景与问题

M4-03 交付 `MessageType::Image` typed 消息收发（`SCOPE-07`），开工前必须冻结
三项契约，否则各工作项将各自发明编码与状态语义：

1. **wire 契约**：aki.text 把原始 UTF-8 字节作为 envelope.payload，但图片消息
   载荷是结构化数据（FileMetadata + TransferId）——需要确定信封 type、载荷
   序列化格式与版本规则。同时 `ImagePayload` 领域类型现仅含 `FileMetadata`
   （`conversation/message/message_types.hpp:109`），无 TransferId——违反
   M4-03「消息面仅 metadata + TransferId」范围与 `RULE-05`，join 键不存在。
2. **收发状态联动**：图片消息同时触发两条通道（消息信封 + 文件传输），
   `DeliveryState` 与 `TransferState` 的关系（正交/传导/合成）未定义——
   同一次断线在两通道产生故意不同的结局（消息 ack_timeout 终态失败 vs 传输
   paused 可续传），任何运行期跨通道传导都会破坏可续性或在信封已送达时误报。
3. **出站 SPI 形态**：图片发送面以何方法进入 `HeyakiAdapter` SPI（新增专用
   方法 vs 泛化 `send_message(Message&)`）。

本记录冻结三项契约；heyaki API 侧映射权威仍为 DEC-006（映射 4 扩展 +
冻结常量 `aki.image`），应用侧集成契约为设计 §6.1/§8.1。

## 决策

**① Image 消息 wire 契约**（细节与解析规则见设计 §6.1①）：

- 信封：`type = "aki.image"`（heyaki `name_token` 合法字符集
  `[A-Za-z0-9_.-]` 内）、`delivery_mode = peer_acked`（同 aki.text）、
  `schema_version = 1`——该字段语义为 **aki 载荷 schema 版本**（heyaki 协议层
  仅校验非零，RPC 描述子与事件发布同此约定），可安全承载 aki 载荷版本。
- 载荷：`ImagePayload`（FileMetadata + TransferId）以 **aki 自有的
  protobuf-wire 格式最小编解码器**（约 100~150 行，落设计 §14 已预留的
  `conversation/codec/` 空目录）序列化进 envelope.payload。冻结字段号
  schema v1：`1=name`（≤512B，对齐 heyaki `max_logical_name_bytes`）、
  `2=size_bytes`（varint）、`3=mime_type`（≤128B）、`4=transfer_id`
  （规范字符串 `hyt1_` 前缀 + 26 base32 字符 = 31 字符；解析侧先经 codec 结构
  谓词（前缀/长度/字符集/尾部位填充）校验，Adapter 层再经
  `heyaki::parse_transfer_id` 权威校验，非规范 → 有界拒绝可见）、
  `5=stored_sha256`（预留 M4-04，v1 解码器跳过）。
- 解析：跳过未知字段（前向容忍）；缺 1~4、超限、非规范 transfer_id 或总量
  超 aki 侧上限 4KiB（冻结 codec 常量，紧于 heyaki 1MiB envelope 上限）→
  有界拒绝可见（入站不投递 sink、Adapter 拒绝计数；出站 admission false）。
- 版本规则：追加可选字段不 bump schema_version；破坏性变更才 bump。
- 字符集：仅长度界，不校验 UTF-8（沿 aki.text 姿态，防止实现各自加码）。
- 领域类型：`ImagePayload` 补 `TransferId transfer_id` 字段（镜像
  `FilePayload` 形状）；持久化 `message.media_transfer_id` 列已存在
  （`persistence/migration/schema_v1.cpp:36`），Image 分支补双向读写，无迁移。

**② DeliveryState ↔ TransferState 联动**（细节见设计 §6.1②）：两通道为
**正交生命周期**，以 TransferId 为唯一关联键（消费侧 join；`transfer.message_id`
FK 保持可空——M4-03 不回填，join 依 `message.media_transfer_id` 单向保持，
重启恢复后一致）。唯一传导规则是**发送侧准入期闸门**（编排层承载，非任一
Manager 内部职责）：先传输准入、后发消息——传输准入失败则不发送消息（消息行
记 `Failed`，Adapter 零 send 调用）；消息准入失败则对刚准入的传输发
`cancel_transfer`（传输行 `Cancelled`；本地确定性决策、处于 Negotiating 前的
准入窗口无竞态）。**运行期零传导**：peer ack 事件与传输终态事件互不跨通道
回写（`Delivered→Failed` 非法边；`ack_timeout` 两可语义下取消传输会破坏
断点续传）；传输 `Completed`（heyaki `sender_committed` = 接收方 BLAKE3
verify → fsync → atomic rename 后才 committed）是比消息 `Delivered` 更强的
独立指示，UI 各自渲染、不合成复合状态。**接收侧消息一律记 `Delivered`**
（§6 冻结语义；协议层已先行去重 + ACK，app 层无法撤回），入站传输状态仅经
transfers Store 经 TransferId join 可见。已知边角如实登记（发送方 ack 失败
但文件已推完 → 接收侧孤儿传输行；接收侧单侧到达）为 M5 UI/GC 议题，不冒充
已处理。

**③ 出站 SPI 形态**：新增专用方法
`send_image_message(receiver, MessageId, FileMetadata, TransferId)`（bool
admission 语义同 `send_text_message`），不泛化为 `send_message(to, id,
Message&)`——泛化需一次性改动 M3 文本路径与全部调用点/测试，无对应收益；
沿增量先例（M3-05 第 10 方法、M4-02 签名扩展）。入站复用既有
`on_message_received(Message)`，不新增 sink 方法——信封 type 分发（aki.text/
aki.image/未知）收敛在 Adapter 层，未知 type 与解码失败均为有界拒绝可见。

## 备选方案

**wire 载荷格式**（均否决）：

- **JSON（nlohmann 等）**：需新增 pinned 依赖（DEC-003 供应链流程 + fetch/CI
  改造），且 aki.text 载荷为原始字节、两套风格并存；仓库无 JSON 库可用
  （dependencies.lock.json 核实仅 executor/EUI-NEO/heyaki/sqlite）。
- **heyaki MessageEnvelope.headers 承载字段**：header value 上限 256B
  （message.hpp）小于 heyaki 逻辑名上限 512B（file.hpp），payload 置空与
  aki.text 先例背离，map 重复键语义需另立策略。
- **直接用 heyaki third_party 的 protobuf**：非公开 API（DEC-006 契约权威仅
  公开头文件 + docs），给 aki 构建引入 protoc 代码生成，破坏锁定纪律。
- **复制 heyaki proto_codec.hpp 进 aki**：上游无 fork 路径、注释明示不安装
  不公开，漂移无追踪；自写同形小编解码器不等同复制。
- **无版本化的 ad-hoc 拼接编码**：M4-04 即需加 stored_sha256 字段（§7.1⑤），
  无前向容忍则接收端直接弃包；M4-06 往返断言无稳定契约。

**SPI 形态**：`send_message(to, id, Message&)` 泛化——一次性改动 M3 文本
路径与全部调用点/测试，无对应收益；保留为后续重构选项（否决，沿增量先例）。

**状态联动**（均否决）：

- **复合状态传导**（传输终态 Failed/Cancelled ⇒ 消息 Failed；传输 Completed ⇒
  消息 Delivered）：需 `Delivered→Failed` 非法边或重定义 Delivered 语义
  （破坏 M3 文本消息一致性，DEC-006 映射 4 已冻结）；ack_timeout 与传输完成
  竞态下还需 OR 合成语义，复杂且无消费者价值。
- **接收侧以传输状态推进 DeliveryState**：违反 §6 冻结语义「收到的消息在本地
  记录为 Delivered」与 MM 既有实现；协议层已先 ACK，app 无法撤回；入站传输
  状态已由 transfers Store 完整表达。
- **消息运行期失败自动取消传输**：ack_timeout 语义两可（消息可能实际已到
  对端），取消造成对端有卡无文件；断线时传输自动 paused 可续，取消破坏断点
  续传；孤儿传输行在 Transfers 页可见（§7），不是状态机问题。
- **接收侧从传输行自动合成消息卡**：§6 消息只能来自 wire；合成引入去重/重复
  问题，超出 M4；孤儿传输行与无传输行消息卡属 M5 UI 兜底与后续 GC 议题。
- **消息先行、传输后启**：消息先 Sent/Delivered 后传输准入失败时，对端已收到
  永不来的传输引用卡，且 Delivered 终态使本地无法修正；传输先行仅在最坏情况
  留下可取消的传输行，代价更小。

## 影响与风险

- **入站包装签名扩展**：NodeSession `set_message_handlers` 入站回调需携带
  信封 type/payload 字节（现压平为 text 串），牵动 `heyaki_node_adapter.hpp`
  与析构中和路径、`test_message_loopback`、`test_heyaki_node_adapter` 全部
  调用点，须同批更新。
- **领域类型变更**：`ImagePayload` 加 transfer_id 牵动 persistence Image 分支
  往返、既有相等性断言、Fake inject 用例；重启恢复一致性按 M4-06 退出-1 复验。
- **测试要求**：编解码往返含截断/超长/非规范 transfer_id/未知字段跳过/缺字段
  各拒收路径（网络无关 unit，沿 test_heyaki_message.cpp 拆分纪律——[skip]
  受控退出不得与既有失败同二进制）；aki 侧载荷上限在 codec 常量冻结；入站未知
  envelope.type 的有界拒绝可观测；闸门两向失败路径（传输准入失败 → Fake 零
  send 调用；消息准入失败 → Fake 收到 cancel_transfer）；运行期解耦断言
  （传输 Failed/Cancelled 而消息保持 Delivered；接收侧各到达顺序解耦）——
  M4-06 回环按本契约补全链路断言，环境受限沿 M3-09 降级纪律。
- **M4-04 前向兼容**：stored_sha256 进 FileMetadata 时保持 wire 字段 5 兼容
  （旧接收端跳过、新接收端缺省视为无）。
- **TransferId 生成入口**：规范串作 `TransferId.value` 意味着新生成
  TransferId 必须以规范形式生成（`hyt1_` + 26 base32，`to_string(TransferId)`
  形式）；现有测试用 `t-1` 风格 ad-hoc 串在 M4-04 push_file 转换处将被拒——
  生成规则随 M4-04 定案冻结（本项 Fake 沿 M4-02 先例参数化接受，不校验规范
  形式；codec/Adapter 层校验仅约束 wire 面）。
- **已知边角**（如实登记）：发送方消息 ack 失败但文件已推完 → 接收侧孤儿
  传输行；发送方传输准入失败但消息已发出（闸门下不应发生，作为断言对象）→
  接收侧无传输行的卡片；两者均为 M5 UI/GC 议题。

## 验证方式

本记录依据 2026-09-26 冻结调研（负责人 Linductor；纯静态证据——pinned
heyaki `message.hpp`/`file.hpp`/`ids.cpp`/`message_protocol.cpp`/`limits.hpp`
与 aki 仓库相关文件逐项核实，未编译未运行）：envelope 结构与校验、
`aki.image` 合法性、schema_version 语义、payload 上限、TransferId 规范形式
（base32 双射 + 非规范拒绝）、aki.text 先例、领域/持久化现状、无 JSON 依赖、
peer ack 语义（不承诺 handler 执行/持久化）、传输 paused/committed 语义。
动态验证归 M4-03 实现（网络无关单测：编解码往返与拒收路径、SPI/Manager
路由、闸门与终态幂等）与 M4-06 回环（双端全链路 + 重启恢复一致），证据记录
于 [M4 里程碑文档](../plans/m4-image-file-transfer.md)。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 6.1 节（本决策的应用侧集成契约
  权威）、§6（消息模型/ImagePayload 形状）、§7.1①（闸门编排层归属）、
  §8.1（SPI 第 `send_image_message` 方法与入站复用声明）
- [DEC-006](DEC-006-heyaki-api-contract.md)（冻结常量 `aki.image` 扩展 +
  映射 4 图片面扩展——heyaki API 侧映射权威）
- [DEC-004](DEC-004-local-persistence-sqlite.md)（message.media_transfer_id
  列与传输行布局）、[DEC-008](DEC-008-manager-routing-and-executor-tasks.md)
  （Manager 职责切分——闸门在编排层而非 Manager 内部）、
  [DEC-009](DEC-009-appstate-write-path.md)（写路径）
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-07`、`RULE-05`、
  `RULE-08`、`RULE-10`、M4）
- [M4：图片消息与文件传输](../plans/m4-image-file-transfer.md)（`M4-03` 实现、
  `M4-04` 字段 5 接入、`M4-06` 回环验证）
