# Aki 设计方案

## 1. 背景

Heyaki 已经具备设备身份、发现、P2P 建链、Relay
和文件传输等基础能力。Aki 建立在这些能力之上，作为 Heyaki
面向实际使用场景的上层客户端。一方面，它给多设备之间的通信提供统一入口；另一方面，Heyaki
的连接切换、Relay
fallback、断线恢复和大文件传输等能力也能在长期运行的真实客户端中持续接受检验。

交互方式沿用 QQ
一类桌面即时通信软件的基本模型，但把通信主体从"用户"换成"设备"。电脑、手机、服务器、机器人以及其他运行
Heyaki
的终端各自持有独立身份，可以被发现、建立信任关系，并与另一台设备保持
Conversation。

聊天窗口在这里也是设备交互入口。第一阶段处理文本、图片、视频和文件；后续的设备状态、命令、终端和
Agent 消息继续进入同一套 Conversation 模型。

``` text
Device A <──── Heyaki P2P / Relay ────> Device B
   │                                      │
Identity                               Identity
Presence                               Presence
Trust                                  Trust
   │                                      │
   └──────────── Conversation ────────────┘
                      │
                   Message
```

## 2. 设计目标

实际开发环境通常同时存在桌面机、笔记本、手机、服务器、开发板和机器人。设备之间的操作目前分散在不同工具里：SSH
负责远程操作，SCP、SFTP 或网盘负责文件传输，IM
负责聊天，设备状态又往往需要单独的 Web 页面。

Aki 将这些终端统一表示为 `Device`。两个设备之间建立
`Conversation`，文本、媒体和文件都挂在会话下。以后增加状态查询、远程命令或
Agent 时，仍然沿用 Device → Conversation
的关系，不再为每类功能单独建立一套入口。

``` mermaid
graph TD
    Device[Device] --> Conversation[Conversation]
    Conversation --> Text[Text]
    Conversation --> Image[Image]
    Conversation --> Video[Video]
    Conversation --> File[File Transfer]
    Conversation -. 扩展 .-> Status[Device Status]
    Conversation -. 扩展 .-> Command[Command]
    Conversation -. 扩展 .-> Agent[Agent Message]
```

Aki 同时承担 Heyaki reference application
的角色。相比只在单元测试中验证底层接口，它会长期覆盖设备发现、连接路径变化、Relay
fallback、断线恢复和大文件传输等真实使用链路。

## 3. 设备身份

每台设备直接作为独立通信主体，不再增加传统 IM 中的 User → Device
两层账户模型。设备持有长期密码学身份：

``` cpp
struct DeviceIdentity {
    DeviceId id;
    std::string display_name;
    DeviceClass device_class;
    PublicKey public_key;
    DeviceCapabilities capabilities;
    TrustState trust_state;
    PresenceState presence;
};
```

`PresenceState` 取 `Online / Offline` 两值，由 Device Manager 依据 presence
事件维护；在线/离线之间的中间形态（如连接中）不在第一阶段引入。

`DeviceId` 直接复用 Heyaki
的身份体系，并与设备长期公钥保持稳定关系。Aki
不另外维护用户名、密码或中心账户。

界面中的联系人也就是设备本身：

``` text
● alpha17001
  Linux · Direct

● humanoid-01
  Robot · LAN

● Miracle-Pixel
  Android · Relay

○ home-pc
  Last seen 2h ago
```

设备类型、操作系统和当前连接方式用于展示和辅助判断连接状态；稳定身份仍由
Heyaki 的密码学身份确定。

## 4. 设备发现与信任

设备可以通过局域网发现、已知设备记录、Relay、邀请链接或手动输入 Device
ID 进入发现流程。来源不同，但应用层统一表示为 `DiscoveredDevice`：

``` cpp
struct DiscoveredDevice {
    DeviceIdentity identity;
    DiscoveryMethod method;
    EndpointInfo endpoint;
};
```

新发现的设备默认处于未信任状态。客户端显示设备名称、类型、Device
ID、公钥指纹和发现来源，用户确认后建立信任关系。在这个模型下，"添加联系人"对应的实际操作就是信任一个设备。

``` text
Unknown -> Pending -> Trusted
                  ├-> Rejected

Trusted -> Revoked
```

转移规则固定为：`Unknown -> Pending`（进入信任确认）、`Pending -> Trusted`（确认）、
`Pending -> Rejected`（拒绝）、`Trusted -> Revoked`（撤销已建立的信任）。
`Revoked` 表示对既有信任的收回，只能从 `Trusted` 进入；`Rejected` 与 `Revoked`
是终态。新增状态或转移必须先更新本节，不允许代码私有状态。

`Trusted`
只表示身份已经得到认可，不包含远程操作授权。终端、屏幕控制、机器人控制等能力需要继续经过
capability 和 permission 判断，避免把设备信任直接等同于控制权限。

## 5. Conversation

`Conversation`
是应用层组织通信的主要对象。第一阶段只处理设备间一对一会话：

``` cpp
struct Conversation {
    ConversationId id;
    DeviceId local_device;
    DeviceId remote_device;
    ConversationState state;
};
```

Conversation
不绑定具体网络路径。同一段会话可能最初走局域网直连，之后切换到 Internet
P2P，在无法直连时再经过 Relay。路径切换不创建新的
Conversation，也不改变已有消息历史。

`ConversationState` 取 `Active / Disconnected / Archived`：
`Active <-> Disconnected` 表达远端可达性的变化（断线恢复后回到
`Active`，不新建会话）；`Active / Disconnected -> Archived` 由用户或清理策略触发；
`Archived` 是终态。

``` text
Conversation
     │
     ▼
Heyaki Session
     │
 ┌───┼────┐
 ▼   ▼    ▼
LAN P2P Relay
```

当前连接路径可以作为状态信息显示在 UI
中，方便排查连接问题。业务层只依赖稳定的 Session/Conversation
语义，不根据 LAN、P2P 或 Relay 分别组织业务状态。

## 6. 消息模型

消息从第一版开始使用 typed message：

``` cpp
struct Message {
    MessageId id;
    DeviceId sender;
    DeviceId receiver;
    Timestamp timestamp;
    MessageType type;
    MessagePayload payload;
    DeliveryState state;
};
```

第一版支持 `Text`、`Image`、`Video`、`File` 和
`System`。协议兼容的前提下，后续可以继续加入
`Command`、`CommandResult`、`DeviceStatus`、`DeviceEvent`、`Telemetry`、`PermissionRequest`
和 `AgentMessage`。

`DeliveryState` 取 `Queued -> Sending -> Sent -> Delivered` 的正向链，任意非终态
可以进入 `Failed`；`Delivered` 与 `Failed` 是终态。收到的消息在本地记录为
`Delivered`。终态幂等：迟到的状态回报不得让已终结的消息重新进入活动状态。

聊天窗口后续会承担设备状态和操作入口，如果消息层只提供字符串，命令、状态和
Agent 输出最终都需要再次编码和解析。Typed message 直接保留消息语义，也让
UI 可以根据类型渲染对应组件。

`ImagePayload` 为 `FileMetadata media` + `TransferId transfer_id`（镜像
`FilePayload` 形状，M4-03 起；消息面仅 metadata + TransferId，图片本体经传输
链路，`RULE-05`）。wire 契约与收发状态联动见第 6.1 节（M4-03 固化；
`VideoPayload` 的同构 transfer_id 缺口在 M4 范围外，接入时沿本节先例）。

### 6.1 消息 wire 契约与收发状态联动（M4 契约，M4-03）

本小节固化 Image typed 消息的 wire 契约与 `DeliveryState`/`TransferState`
联动语义（[DEC-010](../decisions/DEC-010-image-message-contract.md)；
DEC-006 冻结常量与映射 4 扩展为契约权威，本节为应用侧集成契约）。

**① Image 消息 wire 契约**：出站信封 `MessageEnvelope{message_id =
aki MessageId 16B 双射, type = "aki.image", schema_version = 1,
delivery_mode = peer_acked}`（同 `aki.text` 的 MessageId 双射与 ack 模式，
DEC-006 映射 4）；`schema_version` 语义为 **aki 载荷 schema 版本**（heyaki
协议层仅校验非零，RPC 描述子与事件发布同此约定）。envelope.payload 承载
`ImagePayload`（FileMetadata + TransferId）的 aki 自有最小编码——冻结字段号
schema v1（protobuf-wire 形态，编解码器落第 14 节预留的 `conversation/codec/`）：

| 字段号 | wire 类型 | 载荷 | 界限 |
| --- | --- | --- | --- |
| 1 | length-delimited | `name`（原始字节） | ≤512B（对齐 heyaki `max_logical_name_bytes`） |
| 2 | varint | `size_bytes` | — |
| 3 | length-delimited | `mime_type`（原始字节） | ≤128B |
| 4 | length-delimited | `transfer_id`（规范字符串） | `hyt1_` 前缀 + 26 个 base32 字符 = 31 字符（heyaki `TransferId` 规范形式，DEC-006 冻结常量） |
| 5 | length-delimited | `stored_sha256`（M4-04 起承载） | 64 字符小写 hex（codec 常量 `kImageSha256MaxBytes`）；可选字段——缺省视为无，旧接收端跳过（DEC-010 前向兼容条款落地） |

解析规则（前向容忍与有界拒绝）：解码**跳过未知字段号**（前向兼容——新增可选
字段不破坏旧接收端）；缺字段 1~4 任一、字段超限、transfer_id 非规范形式
（前缀/长度/字符集/尾部位填充任一不符）或载荷总量超 aki 侧上限（4KiB，冻结于
codec 常量，紧于 heyaki 1MiB）→ **有界拒绝可见**：入站不投递
`on_message_received`、经 Adapter 拒绝计数可观测（`RULE-09`）；出站编码失败
→ SPI admission false。字符集校验沿 `aki.text` 姿态：仅长度界，不校验 UTF-8。
版本规则：**追加可选字段不 bump schema_version**（旧接收端跳过、新接收端缺省
视为无）；破坏性变更（删字段/改语义）才 bump——M4-04 的 `stored_sha256`（字段
5）即按追加字段保持 v1。

**② DeliveryState ↔ TransferState 联动**：两通道为**正交生命周期**——
`DeliveryState` 只反映消息信封的协议层投递（peer ack），`TransferState` 只反映
文件本体传输（§7）；唯一关联键是 `TransferId`（**消费侧 join**：消息侧经
`ImagePayload.transfer_id` 持久化到 `message.media_transfer_id` 列；
`transfer.message_id` FK 保持可空——M4-03 不回填，重启恢复后 join 依持久化键
保持）。唯一传导规则是**发送侧准入期闸门**（编排层承载——组合根/应用层辅助
函数，非任一 Manager 内部职责）：

1. **先传输准入、后发消息**（闸门判据为 **enqueue 级 admission**——两命令
   面均异步，返回值只代表收件箱受理）：`start_transfer` enqueue 拒绝（TM
   收件箱满）则不发送消息，消息行记 `Failed`（Adapter 零 send 调用）。
   消息半边自 M4-04 起为 **hash-first 排序**（§7.1④/DEC-011①）：消息发送
   （`stored_sha256` 随 `FileMetadata` 携带，DEC-010 字段 5）等发送前
   SHA-256 完成后经注入延续在 TM 泵上下文触发——闸门第 2 步（MM enqueue
   拒绝 → 对刚准入的传输发 `cancel_transfer`，传输行 `Cancelled`）因此
   发生在延续内，降级结果不可同步返回，经各 Manager 计数与 Adapter 命令
   面可观测（v1 同步编排 `send_image_message_with_transfer` 保留，其
   `ImageSendFlowResult` 的 `*RowLost`/`*CancelLost` 两级降级档语义不变：
   补偿 `Failed` 行被拒 → 无消息行；补偿取消被拒 → 传输持续在飞、传输行
   停留 `Queued`——补偿写入自身也是 enqueue admission，降级不吞掉，
   AGENTS 规则 10）；归档失败/无 IO 承载时延续以空 hash 触发且编排不发
   消息（wire 侧自行终结，零传导语义保持）。**重复 TransferId 不在闸门
   可见面内**：其 enqueue admission 照常受理（闸门通过、消息照常发送），
   业务拒绝发生在 TM 排空 handler 的会话表守卫（经 `handler_rejections`
   可见，RULE-09）——不新增传输行、不替换旧在飞会话，消息引用的仍是
   既有 TransferId；TransferId 唯一性由调用方生成保证——生成入口
   `NodeSession::new_transfer_id()`：16 随机字节（`std::random_device`，
   全零重抽）→ `heyaki::to_string` 规范串（`hyt1_` + 26 base32；M4-04
   定案，DEC-011④）。
2. **运行期零传导**：peer ack 事件（`on_message_delivered`/
   `on_message_send_failed`）与传输终态事件（`on_transfer_completed`）互不
   跨通道回写——`Delivered→Failed` 非法（§6 状态机）、`ack_timeout` 两可语义
   下取消传输会破坏断点续传（§7.1）；传输 `Completed` 是比消息 `Delivered`
   更强的独立指示（文件已落对端盘），UI 各自渲染、不合成复合状态。
3. **接收侧消息一律按 §6 记 `Delivered`**（协议层已先行去重 + ACK，app 无法
   撤回）；入站传输状态仅经 transfers Store 经 TransferId join 可见，不推进
   消息 `DeliveryState`。

已知边角（如实登记，非状态规则）：发送方消息 ack 失败但文件已推完 → 接收侧
孤儿传输行；接收侧仅一侧到达（消息无传输行/传输无消息卡）→ M5 UI 兜底与后续
GC 议题。`VideoPayload` 同构接入沿本节先例（先更新本节再实现）。

## 7. 文件传输

文件消息与文件数据分开处理。Conversation 中保存文件 metadata 和 Transfer
ID，文件本体通过独立的 Transfer Session 传输：

``` text
Conversation
     │
     └── File Message
             │
             └── TransferId
                    │
                    ▼
              Transfer Session
                    │
                    ▼
               Binary Stream
```

这样传输数 GB 的模型、日志或数据集时，文件数据不会占用普通消息的数据流。

``` cpp
struct Transfer {
    TransferId id;
    DeviceId sender;
    DeviceId receiver;
    FileMetadata file;
    uint64_t transferred;
    uint64_t total;
    TransferState state;
};
```

Transfer 至少包含
`Queued`、`Negotiating`、`Transferring`、`Paused`、`Completed`、`Failed`
和 `Cancelled`
状态。聊天窗口用文件卡片展示当前会话中的任务，同时提供独立的 Transfers
页面查看正在进行和已经结束的传输。

### 7.1 传输集成契约（M4 契约，M4-01；M4-04 修订承载形态，
[DEC-011](../decisions/DEC-011-transfer-io-bearing.md)）

本小节固化传输的应用层集成契约，供 M4-02~05 直接实现（沿第 11.1 节先例；
引擎语义以 DEC-004/DEC-006 为准）。③ 的承载形态由 M4-04 按 DEC-011 定案
（M4-01 的「分块 IO 不经 DB 通道」硬结论维持，承载主体修订为专用 worker）。

**① TransferManager 职责切分（DEC-008 模式扩展；M4-04 修订）**：TransferManager
只写 transfers Store；出站四接口（发起/暂停/恢复/取消）与入站文件事件在其
单飞排空泵上下文串行处理；**传输无池上会话长任务**（M4-04 定案，DEC-011）——
wire 侧状态由 heyaki 文件事件驱动（`set_file_event_observer` → RouterSink →
TM 泵，EXEC-02 既有路径），Aki 侧归档由 IO 作业完成事件驱动，取消经
「worker StopToken（全局）+ 会话控制位（块间检查）+ 续接抑制」表达、不经
`request_task_cancel`（无池任务句柄）；会话按业务稳定 ID（TransferId）登记
于会话表（`EXEC-07` 语义），有界收件箱 + 排空泵沿 DEC-008 模式。发送侧链路：
`start_file_transfer` → heyaki `push_file`（wire 侧 heyaki 自读源文件）→
文件观察事件 → typed 更新；Aki 侧归档拷贝 `source → files/tmp/<id>.part`
经专用 transfer IO worker 分块承载（见 ③）。接收侧（M4-05 落地，
[DEC-012](../decisions/DEC-012-receive-merge-bearing.md)）：对端
push 的分块由 heyaki 接收根落盘，`Committed` 事件后 Aki 从接收根合并到
`DEC-004` 存储布局（合并承载见 ③/§11.1④——经 DatabaseWorker 终态作业组
供源参数化，不上 transfer IO worker）；接收侧无 Aki 归档相位、无终态闸门、
无接收会话（wire 终态直达 `CompleteTransfer`），行由入站首个状态事件建行
（`file.name`=wire `logical_name`、`size`=`bytes_total`、sender=peer、
receiver=local），入站/出站判别以「无发送会话 / receiver==local」为准。
暂停/恢复：`pause_file_transfer`/
`resume_file_transfer` 驱动状态机 `Transferring ↔ Paused`（wire 侧 +
归档续接抑制/放行）；heyaki 侧分块进度保持（断点续传），
Aki 侧 `.part` 与已落库进度保持。取消：`cancel_file_transfer` →
`Cancelled` 终态 + `.part` 幂等删除作业（M2-06 discard 作业组）。
图片/文件消息的发送侧准入闸门（先传输准入、后发消息）属编排层契约，
见第 6.1 节 ②——TransferManager 不感知消息域，闸门由组合根编排承载。

**② 传输 typed 更新 → DB 作业映射（§11.1 ① 补充）**：在既有
`UpsertTransfer`/`UpdateTransferProgress`/`CompleteTransfer` 之上，
`Transferring ↔ Paused` 与取消路径不新增独立 DB 作业类型——状态推进经
`UpsertTransfer`（整行 upsert 幂等）承载，进度经 `UpdateTransferProgress`
（部分列更新）承载；`.part` 生命周期经既有 M2-06 作业组
（Completed 终态组 / discard 删除组）承载。暂停/恢复的**请求路径**不经
DB：`pause_file_transfer` / `resume_file_transfer` 指令仅作用于内存会话
状态与 heyaki 传输，不产生 DB 作业；**指令导致的状态推进本身经既有作业
承载**——`paused` 与恢复后的 `Transferring` 推进按本节 ⑤ 的映射入队
`UpsertTransfer` 作业（进度列已持久化，恢复后从 `transferred` 列续传），
与 §11.1 ① 的 TRANSFER 作业模型一致。失败语义沿 §11.1 ①：入队拒绝与执行
失败可观测，不回滚内存态。进度更新经泵侧每会话最新进度槽聚合（单飞 dirty
工作项、每次排空至多一个 `UpdateTransferProgress`，见 ③）。

**③ `.part` 写入承载与通道容量（M4-04 定案，DEC-011；DEC-009 预算复核
结论维持）**：发送侧为「事件驱动会话状态机 + 专用 blocking worker 分块
IO」。Aki 侧文件 IO（发送前流式 SHA-256 + 归档拷贝 `source →
files/tmp/<transfer_id>.part`）走**新增专用 transfer IO worker**（名
`aki.transfer-io`，与 `aki.db-worker` 同款 `IBlockingIoWorker` + 有界
`MpscChannel` 作业通道形态，EXEC-04；承载接口声明于
`transfer/storage/transfer_io.hpp`，实现于 `persistence/storage/`）——分块
IO **不经 DatabaseWorker 通道**（M4-01 硬结论维持且更干净：DB drain 不等待
任何分块）。作业为 offset 基础的无状态分块（对齐断点续传：恢复后从持久化
`transferred` 列续传），**每会话单飞**（per-session single-flight：每会话
至多一个在飞分块作业，完成事件回到泵后续接下一块——「顺序写 `.part`」
不变量由单飞保持；通道有界满即拒绝可见，RULE-09）。进度列以 wire 事件
（heyaki `bytes_done`）为准，泵侧每会话最新进度槽 + 单飞 dirty 工作项——
**每次排空至多一个 `UpdateTransferProgress`**（不可逐分块入箱：heyaki 逐
分块事件率可达每秒数千，TM 收件箱仅 256）；仅进度列更新与终态作业组经
DatabaseWorker 通道，批上限维持 64×2≤256 不变、drain 预算（2s）不受影响。
终态闸门：`CompleteTransfer(Completed)` 仅在归档 `.part` 写完后放行入队
（M2-06 终态作业组对不完整 `.part` 按契约明确失败）；`Failed`/`Cancelled`
终态不等待归档（在飞块结束后幂等清理）。**废除形态**（M4-04 定案）：池上
`submit_cancellable` 会话长任务内联写 + `sleep_for` 轮询（M1/M4-02 骨架）——
每会话停占一个池 worker，2 核设备 ≥2 并发传输即饥饿 Manager 泵（M4-03
观察③ CI 实证）。hash-first 排序（④ 的发送前 SHA-256）：hash 分块作业流
先行，消息发送（`stored_sha256` 随 `FileMetadata`）等 hash 完成后经注入
延续在泵上下文触发；`push_file` 不等 hash（wire 完整性是 heyaki BLAKE3
职责）——大文件的消息可见延迟 = hash 单遍时长；归档 hash 失败时延续以空
hash 触发、调用方不发消息，wire 侧自行终结。
**接收侧承载（M4-05 定案，[DEC-012](../decisions/DEC-012-receive-merge-bearing.md)）**：
接收侧合并不上 transfer IO worker、不新增 worker——扩展现有 M2-06 终态
作业组的供源（见 ④/§11.1④），仍整体经 DatabaseWorker 通道；接收侧无 Aki
归档相位、无终态闸门（无 SendSession 的 wire 终态直达即为正确形态）；
`.part` 幂等删除作业维持现状（接收侧无 `.part` 时幂等 no-op）。TM 的
flush/IO 归零语义对接收路径不变（无接收 IO 会话）。

**④ BLAKE3 wire 校验与 SHA-256 存储哈希的关系（澄清结论）**：heyaki 传输
协议在 wire 层以 BLAKE3（manifest 32B + 每分块 32B，file.hpp）做传输完整
性校验（`verifying` 阶段 whole-file digest + fsync + rename）；`DEC-004`
存储哈希为接收方对最终落盘文件的全量流式 SHA-256（M2-06 终态作业组回写
`stored_sha256`）。两者层次不同、互不替代：BLAKE3 校验「传输过程中字节
未损坏」，SHA-256 校验「落盘后的存储内容」——接收方在 heyaki `Committed`
之后对已落盘文件计算 SHA-256 并回写，**无冲突**，不需要统一决策。发送方
SHA-256 在发送前对源文件计算（M4-04 落地：hash-first 分块作业流，见 ③），
随文件 metadata 携带（`FileMetadata.stored_sha256`，DEC-010 冻结字段号 5；
**wire + 内存字段**——message 表无对应列，消息行重启重建时为空，传输行
`stored_*` 回写列是持久权威），接收方校验时对账。
**供源参数化与对账策略（M4-05，DEC-012①⑤）**：complete 作业组供源扩展为
`.part` → heyaki 接收根文件（`<接收根>/<logical_name 段>`，段拼接前有界
校验）→ final 恢复分支 → 明确失败；接收源就位与原件删除折进同一作业
（CompleteTransfer(Completed) 仍为 1 作业）。收发 SHA-256 对账不在作业内
执行（发送方哈希无持久面）——由持有消息载荷的消费者执行（M4-06 回环断言
/M5 UI），失配即断言失败/可见呈现，不静默。

**⑤ DEC-006 映射 7 实现级细化**：`start_file_transfer` →
`push_file(peer, root, logical_name, source_path, transfer_id)`
（transfer_id 由应用以业务稳定 ID 提供，支持断点续传）。参数来源（2026-09-24
评审定案，DOD-04 设计先行——SPI 修订沿 M3-05 第 10 方法先例，M4-02 落地）：
`source_path` 为发送侧本地文件路径，经出站 SPI 签名扩展传入（§8.1：
`start_file_transfer(receiver, TransferId, FileMetadata, source_path)`，
`source_path` 为 `std::filesystem::path`）；**不进入对端可见的
`FileMetadata`**——该结构随消息载荷发给对端，携带发送方本地文件系统路径
即信息外泄（`FileMetadata` 面向 M4-04 另补 `stored_sha256` 等对端可校验
字段）；`root` 为 heyaki 逻辑根，由组合根经存储配置注入 Adapter 选项（非
SPI 参数，Aki 侧固定使用会话默认文件根）；`logical_name` ←
`FileMetadata.name`；
`pause_transfer` → `pause_file_transfer`；`resume_transfer` →
`resume_file_transfer`；`cancel_transfer` → `cancel_file_transfer`；
`set_file_event_observer` → 状态映射（**2026-09-26 评审修正**，按 pinned 源
定论：`probing`/`offered` 为**发送端专属事件**——file_service.cpp 仅发送侧
发出 :210/:252/:604；**接收端首个事件是 `transferring`** :1147/:1368；接收侧
push 事件 `direction` 恒为 `push`（:1147/:1368/:1441，`pull_initiated=false`）、
`cancelled` 恒为 `pull`（:380）——**判向不经 `direction`**）：
`probing`/`offered` → `on_transfer_started`（发送行状态推进——行由
`StartTransferWork` 先建、TM 以泵内已知行缓存合并，Adapter 构造的
sender/receiver 不参与；行缺失时补建 `sender=local` 行）；
`transferring`/`verifying` → 未见 started 的 id 由**首个事件建行**
（`on_transfer_started`，接收行 Negotiating：`sender=peer`、`receiver=local`、
`file.name`=**剥根段** wire `logical_name`——wire manifest `logical_name` =
join(root, name)（file_service.cpp :556），heyaki 落盘为剥根段相对名（:1075）
而事件携带含根前缀（:1147/:1368/:1441），行名与供源推导
（`receive_source_path`）须与落盘相对名一致；Adapter 以有界记名簿去重、
容量满计数可见）+ `on_transfer_progress`，后续 `transferring`（含 bytes_done
变化）→ `UpdateTransferProgress`；`paused` → `UpsertTransfer`（`Paused`；对端
驱动——含断线自动暂停，可发生于接收侧——经 sink 第 11 方法
`on_transfer_paused(TransferId)` 投递，§8.1；本地暂停确认后同此映射，
不新增 AppEvent 主路径类型，状态可见于 Store 快照）；
`committed` → `CompleteTransfer`（`Completed`，M2-06 终态作业组）；
`failed` → `CompleteTransfer`（`Failed`，事件 error 入观测日志）；
`cancelled` → `CompleteTransfer`（`Cancelled`，`.part` 删除作业组；终态事件
不建行——行缺失由 owner 拒绝可见，已知边角）。`pull_file`（接收方向拉取）
M4 暂不接入（接收侧以 push 接收为主），接口预留。

## 8. 应用结构

网络能力继续由 Heyaki 提供，包括设备身份、发现、信令、P2P 建链、Relay 和
transport。Aki 负责应用状态、Conversation、消息、传输任务和
UI。

``` mermaid
graph TD
    UI[EUI-NEO UI] --> State[Application State]

    State --> DM[Device Manager]
    State --> CM[Conversation Manager]
    State --> MM[Message Manager]
    State --> TM[Transfer Manager]

    DM --> Adapter[Heyaki Adapter]
    CM --> Adapter
    MM --> Adapter
    TM --> Adapter

    Adapter --> Heyaki[Heyaki]

    Heyaki --> Identity[Identity]
    Heyaki --> Discovery[Discovery]
    Heyaki --> Signaling[Signaling]
    Heyaki --> Transport[Transport]

    Transport --> LAN[LAN Direct]
    Transport --> P2P[Internet P2P]
    Transport --> Relay[Relay]
```

业务层通过 Heyaki Adapter 使用底层能力。Adapter 把 Heyaki API
转换为设备发现、消息发送、文件传输等应用语义，使 UI 和 Manager
不需要直接依赖 Heyaki 的具体接口。Heyaki
后续调整接口时，变化也主要收敛在这一层。

### 8.1 Heyaki Adapter SPI（M1 契约）

M1 以纯虚抽象接口固定 SPI（`DEC-002`），真实 Heyaki 在 M3 目标级接入
（`DEC-003` / `DEC-006`）。接口只表达应用语义，不出现 Heyaki / EUI-NEO / 平台 /
executor 类型（`RULE-10`）；heyaki 层仅依赖第 3~7 节领域类型，不反向依赖 app 层
（`RULE-01`）。

出站（应用 → Adapter），`bool` 返回值为有界 admission 结果，拒绝必须可见：

- `start_discovery(DiscoveryMethod)` / `stop_discovery()`：设备发现启停（第 4 节）；
  扫描型来源（LAN 发现 / Relay）启动扫描，记录型来源的接入在 M3 细化。
- `send_text_message(receiver, MessageId, text)`：文本消息发送（第 6 节）；
  `MessageId` 由应用生成并保持稳定（`RULE-08`）。
- `send_image_message(receiver, MessageId, FileMetadata, TransferId)`：图片
  消息发送面（第 6.1 节，M4-03 增补——沿 M3-05 增量先例新增专用方法，不泛化
  为 `send_message(Message&)` 一次性改动全部调用点）；`bool` admission 语义同
  `send_text_message`，载荷超限或
  TransferId 非规范（codec 编码失败）即 admission false 可见（`RULE-09`）；
  消息面仅 FileMetadata + TransferId，图片本体经传输链路（`RULE-05`）。
  入站不新增 sink 方法：复用既有 `on_message_received(Message)`——信封
  `type` 的分发（`aki.text`/`aki.image` → 对应 typed payload；未知 type →
  有界拒绝可见，不投递 sink）收敛在 Adapter 层（第 6.1 节 ①）。
- `start_file_transfer(receiver, TransferId, FileMetadata)` /
  `pause_transfer` / `resume_transfer` / `cancel_transfer(TransferId)`：文件传输
  接口面（第 7 节）。M4 前仅签名与 TransferId 语义——一个 `TransferId` 对应一个
  传输会话，不可重复启动；文件本体不经本接口传输（`RULE-05`）。M4 SPI 修订
  （2026-09-24 评审定案，设计先行沿第 10 方法先例，M4-02 落地；M3 代码保持
  M1 签名）：`start_file_transfer` 增补第四参数
  `std::filesystem::path source_path`（发送侧本地文件路径，第 7.1 节 ⑤ 参数
  来源定案；不进入对端可见的 `FileMetadata`）。

入站（Adapter → 应用）经 `HeyakiAdapterSink` 纯虚接口投递，方法与第 10 节 9 类事件
一一对应（`on_device_discovered` / `on_device_connected` / `on_device_disconnected` /
`on_message_received` / `on_message_delivered` / `on_transfer_started` /
`on_transfer_progress` / `on_transfer_completed` / `on_connection_path_changed`），
返回值表示投递是否被接受（校验失败或下游背压拒绝可见）。其中
`on_transfer_completed` 的 `final_state` 仅取 `Completed` / `Failed` / `Cancelled`
（第 10.1 节）。M3-05 起追加第 10 个方法 `on_message_send_failed(conversation,
message)`——出站文本的投递回报终态失败面（DEC-006 映射 4 的
`send_failed` / `peer_rejected` / `ack_timeout` / `session_closed`）；协议
`acked` 仍走 `on_message_delivered`，映射为 `SetDeliveryState(Failed)`（终态，
`RULE-08`），不产生主路径事件。M4 起追加第 11 个方法
`on_transfer_paused(TransferId)`（2026-09-24 评审定案，M4-05 已落地）
——对端驱动的传输暂停投递面：heyaki `paused` 相位含**断线自动暂停**、
可发生于接收侧（`file.hpp`，`FileTransferPhase::paused`），无此方法则
`paused → UpsertTransfer(Paused)` 对端驱动时无投递路径；映射
`UpsertTransfer`（`Paused`），不新增 AppEvent 主路径类型。

纪律：Adapter 回调只做有界校验与投递（`EXEC-02`），业务处理一律在 Manager 的执行
上下文（M1-05）；事件从 Sink 到 Application State 的桥接由应用层完成——Sink 实现把
事件写入第 10.1 节的事件入口并提交对应的状态更新。测试与冒烟宿主使用同目录的
`FakeHeyakiAdapter`：以 `inject_*` 编程式注入上述入站事件，注入路径即 `EXEC-02`
回调路径（有界校验 + 投递），不做任何真实 I/O。

真实接入映射（M3 契约，权威为 [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)）：
M3 以 pinned heyaki（v1.0.1-38，wire 协议 `{1,3}`）实现本 SPI。消息类入站事件
（`on_message_received` / `on_message_delivered`）走 heyaki 推送回调
（`set_message_inbound_handler` / 送达回报）；发现、Presence 与路径类事件
（`on_device_discovered` / `on_device_connected` / `on_device_disconnected` /
`on_connection_path_changed`）由 Adapter 在 Aki executor 上周期轮询
`endpoints()` / `peer_sessions()` 做 diff 合成（Node 无目录/会话变更推送回调，
LAN 广播/监听随 Node 常驻）。因此 `start_discovery` / `stop_discovery` 的真实
语义是启停观察管道而非启停网络扫描。记录型来源的接入与「发现 → 进入信任确认」
触发语义（M3-02 细化；API 映射权威为 DEC-006）：

- 扫描型 LAN 发现：`start_discovery` / `stop_discovery` 即启停观察管道（周期
  轮询 `endpoints()` diff）；diff 中新出现的端点合成 `on_device_discovered`
  （`DiscoveredDevice`，trust 取 `Unknown`）。停止发现不移除已入 Store 的设备
  （已发现设备的信任确认不因观察管道停止而失效）。
- 「发现 → 进入信任确认」触发：`on_device_discovered` 被 Sink 接受且
  `UpsertDevice` 被 owner 接受后，设备以 `Unknown` 进入 `DeviceStore` 并发布
  `DeviceDiscovered` 主路径事件——该事件即信任确认入口（宿主/UI 据此发起
  `Unknown -> Pending`；确认动作经 `UpsertDevice` 沿第 4 节合法边推进，M1
  契约不变）。同一设备重复出现在 diff 中为幂等 no-op 接受，宿主按
  `trust_state == Unknown` 过滤确认入口，不重复发起。
- 已知设备记录（ProfileStore 持久化的 TrustGrant/端点记录，DEC-006）：随启动
  恢复直接进入 `DeviceStore`（trust 取记录值，`Trusted` / `Revoked` 等按记录
  恢复，presence 恢复为 `Offline`），不重放 `on_device_discovered`（已知设备
  不是新发现）；其再连接经 `peer_sessions()` diff 合成 `on_device_connected`
  （DEC-006 映射）。
- 邀请链接与手动输入：M3 分期（范围与补做条件见里程碑范围条款）；接入时经
  同一 `on_device_discovered` 入口以对应 `DiscoveryMethod` 合成，触发语义与
  本节一致。

`DeviceIdentity` 的
display_name / device_class / os_name / capabilities 在 M3 为占位值（LanPresence
不携带元数据，DEC-006 已记录缺口）。ID 编码冻结常量与 heyaki API 逐项映射见
DEC-006「决策」节。

### 8.2 Executor 生命周期 owner（app/lifecycle）

Executor 的初始化与关闭由 `app/lifecycle` 的唯一 owner（`ExecutorOwner`）承担
（`EXEC-01` / `EXEC-07`，AGENTS 规则 7/8）：owner 以独立实例持有 pinned executor 的
`Executor` facade（非单例，每个 executor 生命周期有且仅有一个 owner），经
`initialize_ex(config)` 显式初始化；`ExecutorConfig::enable_monitoring` 默认开启并随
config 传入，运行期切换（如有）归 owner，Manager/Adapter 不得私调。blocking I/O
worker 的 `WorkerHandle` 由 owner 注册并持有（M1 不启用——无长期阻塞 I/O 负载，M2
起按负载启用，见第 8.3 节），依赖经构造参数或
明确 context 传递，不设隐藏全局实例；`executor()` 访问器的前置条件是已初始化
（facade 对未初始化实例的首次提交会以默认配置懒初始化，绕过 owner 纪律，禁止）。

受控关闭与总计划 `EXEC-01` 五步一一对应，关闭证据为
`ShutdownResult::Completed` + `get_snapshot().lifecycle == Stopped` +
`wait_timeout_count == 0`：

1. **停止任务生产者**：owner 调用应用注入的停止钩子——停 Heyaki 投递 → drain/close
   `executor::comm` 通道 → 停快照发布（第 10.1 节硬约束 3；comm 与 Executor shutdown
   零耦合，不参与 Executor 关闭）。
2. **发出取消/停止请求**：定时任务经 `TimerHandle` 取消（M1 无定时任务，此子句为
   空操作）、运行中任务经
   `request_task_cancel`（句柄由各 Manager 持有并发起，`EXEC-07`），blocking worker
   经 `WorkerHandle::request_stop()`（noexcept 非阻塞）置位停止标志并唤醒。
3. **回收 blocking worker**：`WorkerHandle::stop()`（stop request + wakeup + join），
   重复停止安全；worker 的 `run()` 必须满足 wakeup 可解除阻塞契约——等待原语无法
   直接唤醒（第三方 read/poll）时，以有限 timeout 轮询 StopToken。
4. **有界等待有限任务**：`wait_for_completion_ex(owner 预算)`；该等待只覆盖默认异步
   future 型任务（不含 realtime/GPU/blocking worker），预算归 owner 而非库内 300s
   内部上限，超时记录 `WaitResult` 与诊断快照作为证据，不伪造“干净关闭”。
5. **最终关闭**：由非 worker 线程执行 `shutdown(true)`，返回 `Completed` 才算关闭
   完成；从池 worker 内调用返回 `RequestedFromWorker` 且不完成 teardown，禁止。

关闭后同一 Executor 不可二次初始化（`initialize_ex` 返回 `AlreadyShutdown`）；
进程内需要新一轮生命周期时重建 owner。关闭后的新提交以明确异常结算（如
"Executor is stopped"），不静默。

与 heyaki Runtime 的协调（M3 契约，[DEC-006](../decisions/DEC-006-heyaki-api-contract.md)）：
M3 起 heyaki 并发经借用注入并入本 owner——宿主以
`heyaki::Runtime::create_borrowed(executor_owner.executor(), cfg)` 创建借用型
Runtime 并经 `NodeConfig.runtime` 注入 `Node::create`；borrowed Runtime 的
AsioWorker/FileIoWorker 以 blocking worker 挂在同一 executor 上（worker 名
`heyaki-asio` / `heyaki-asio-file-io`），对 Executor 监控完全可见；Runtime 关闭
不触碰宿主 executor 生命周期（禁止 `create_owned` / `runtime=nullptr`——进程内
第二 executor 实例违反本节唯一 owner 纪律）。关闭顺序：`Node::shutdown()` +
`Runtime::shutdown()` 编入第 8.3 节钩子序列的停止生产者段（`EXEC-01` 步骤 1，
早于 owner 步骤 2/3/5），并以 `RuntimeShutdownReport.executor_shutdown_performed
== false` + owner `fully_stopped()` 为关闭证据。

owner 落点说明：M1-02 / M1-03 的单测 `main` 持有临时 `executor::Executor` 实例作为
该测试进程的 owner（AGENTS 规则 7 的测试形态，生命周期同样显式：非 worker 线程
`shutdown(true)`），正式 owner 即本节 `ExecutorOwner`；自 M1-06 冒烟宿主起进程内
改用 `ExecutorOwner`。

### 8.3 Manager 职责、事件路由与装配（M1 契约，M1-05）

四个 Manager 是 Application 层类，落位 `app/application/`（`DEC-008`）。职责切分与
Store 所有权：

- 每个 Manager 恰好只写自己领域的 Store：DeviceManager → devices、
  ConversationManager → conversations、MessageManager → messages、TransferManager →
  transfers；写入一律以第 10.1 节的类型化更新指令经 `MpscChannel` 汇聚到状态 owner
  （`RULE-02` / `EXEC-03`），Manager 不直写快照。
- 出站操作按域切分：发现启停（`start_discovery` / `stop_discovery`）归
  DeviceManager；`send_text_message` 归 MessageManager；传输四接口
  （`start_file_transfer` / `pause_transfer` / `resume_transfer` / `cancel_transfer`）
  归 TransferManager。ConversationManager 显式提供
  `ensure_conversation(local, remote)`，由宿主 / 用户流程调用，不从事件隐式建会话；
  其自建会话的 id/端点记录仅用于 connected/disconnected 事件的状态推导（创建记录，
  不复制 owner 权威状态），消息或连接事件先于 `ensure_conversation` 到达时，会话
  推导为幂等空操作。

11 类 Sink 事件（9 类主路径 + 出站失败面 `on_message_send_failed` + 传输暂停面
`on_transfer_paused`，均不产新增主路径事件类型）由 `app/application` 内单一 `RouterSink`（实现 `HeyakiAdapterSink`）
路由：回调线程只做有界校验并投递到各 Manager 的私有有界 `MpscChannel` 收件箱
（`EXEC-02`），业务处理一律在 Manager 的执行上下文；Sink 返回值为各路 admission
的合取，部分拒绝必须可见，下游 owner 的状态机拒绝经 `updates_rejected` 可观测。

| Sink 事件 | 路由与状态更新 | 主路径事件 |
| --- | --- | --- |
| `on_device_discovered` | DM：`UpsertDevice` | DeviceDiscovered |
| `on_device_connected` | DM：`SetPresence(Online)`；CM 扇出：已建会话则 `UpsertConversation → Active` | DeviceConnected（仅 DM 投递一次） |
| `on_device_disconnected` | DM：`SetPresence(Offline)`；CM 扇出：已建会话则 `UpsertConversation → Disconnected` | DeviceDisconnected（仅 DM 投递一次） |
| `on_message_received` | MM：`UpsertMessage`（收到的消息本地记录为 `Delivered`，第 6 节） | MessageReceived |
| `on_message_delivered` | MM：`SetDeliveryState(Delivered)` | MessageDelivered |
| `on_message_send_failed`（M3-05） | MM：`SetDeliveryState(Failed)` | —（Failed 为终态，RULE-08；无主路径事件） |
| `on_transfer_started` | TM：`UpsertTransfer`（建行与状态推进——发送行由 `StartTransferWork` 先建、发送端专属的 `probing`/`offered` 事件推进；接收行由**首个 `transferring`/`verifying`** 事件承担（pinned heyaki 接收端无 probing/offered，首个事件即 transferring）——接收行 `file.name`=剥根段 wire `logical_name`、`size`=`bytes_total`、sender=peer/receiver=local；同态重复幂等去重，§7.1⑤） | TransferStarted |
| `on_transfer_progress` | TM：首个进度事件整行 upsert 推进 `Negotiating/Paused → Transferring`，后续 `UpdateTransferProgress` | TransferProgress |
| `on_transfer_completed` | TM：`CompleteTransfer(final_state)`（接收侧无会话直达；发送侧经终态闸门，§7.1③） | TransferCompleted |
| `on_transfer_paused`（M4-05） | TM：`UpsertTransfer`（`Paused`；对端驱动与本地暂停确认同此映射；发送会话归档续接抑制） | —（不新增主路径事件类型，状态经 Store 快照可见，§8.1） |
| `on_connection_path_changed` | DM：`SetConnectionPath(to)` | ConnectionPathChanged |

执行上下文与任务承载（`EXEC-04` / `EXEC-05` / `EXEC-07`）：

- 每个 Manager 采用“单飞有界排空”泵：工作项入收件箱 → CAS 抢占单飞标志 →
  `submit_auto` 一个排空任务（有界批量，保留并消费 `TaskSubmission.future`；释放
  单飞标志后复查收件箱，防止丢失唤醒）。事件与宿主命令（发送、传输控制、
  `ensure_conversation`）共用同一收件箱串行处理，Manager 内部状态（如传输会话
  句柄表）只在排空上下文访问。
- 长任务用 `submit_cancellable` + `StopToken`（M4-04 起**重连循环等等待型长
  任务适用**；传输会话不再有池上长任务——承载形态见第 7.1 节 ③/DEC-011：
  wire 侧事件驱动 + 分块 IO 走专用 transfer IO worker，取消经 worker
  StopToken + 会话控制位）：Manager 按业务稳定 ID
  持有 `TaskHandle` + future 作为成员；取消一律经
  `executor().request_task_cancel(handle)` 发起——运行期任务协作轮询
  `stop_requested()` 自行退出，排队期取消由 Executor 以
  `TaskCancelled(Explicit)` 结算且不产生 failure 事件；不经 StopSource 直发，业务
  代码不得主动抛 `TaskCancelled` 做控制流（无取消请求时按任务异常计入 failure）。
  传输归档长 IO 经 `aki.transfer-io` blocking worker 承载（DatabaseWorker 同款
  关闭纪律：TM flush 至 IO 在飞归零 → owner `EXEC-01` 步骤 2/3 统一
  request_stop/stop 回收，见第 11.1 节 ③）。
- 取消断言经 `get_cancellation_status()` / `ExecutorSnapshot.cancellation`（独立
  计数，不入 failure）；超时断言经 failure 体系 `timeout_count`——`task_timeout_ms`
  是排队软超时（config 级），不打断运行中任务。M1 不引入 `TimerHandle` /
  周期任务：presence 与传输进度均以事件到达，Manager 无自驱周期负载；首个真实
  周期负载（presence 刷新 / 进度采样）出现时再按 `EXEC-04` 启用。
- Manager 必须先于其任务终结：宿主关闭钩子（下文装配顺序）保证先取消并消费在途
  任务 future，再进入 `ExecutorOwner` 的 `EXEC-01` 步骤 4/5；TransferManager
  的对应形态为「泵静止 + IO 在飞归零」（其 IO 事件回调不得晚于 Manager 终结，
  DEC-011 ③）。

装配与所有权（`EXEC-07`；宿主组合根顺序，M3-02 起固化为下列序（[DEC-009](../decisions/DEC-009-appstate-write-path.md)）；
M1-06/M2-07 console 的「`AppStateOwner` 先于 control」为过渡形态，随 M3-03+
按本序切换）：

1. `ExecutorOwner.initialize()`；
2. 启动恢复（第 11.1 节 ②：主线程同步，产出单一连接 Repositories、FileStore
   与播种数据）；
3. `DatabaseWorkerControl`（锚定恢复移交的单一连接；先于 `AppStateOwner`
   存在，供接受后处理器捕获，第 11.1 节 ②）；
4. `AppStateOwner`（构造入参：初始快照 + 接受后处理器（第 10.1 节，捕获
   control））；
5. 四个 Manager 构造注入 `executor::Executor&`、`AppStateOwner&`、各自所需的
   `HeyakiAdapter&` 与容量预算；
6. `start_blocking_worker` 注册 DatabaseWorker 并 `mark_registered()`（第 11.1
   节 ②③）；
7. `RouterSink` 经 Adapter `set_sink` 注册（接通事件源）。

M1 冒烟宿主（仓库根 `main.cpp`，M1-06）是该组合根的最小进程内实现，进程内 owner
自本项起改用正式 `ExecutorOwner`（见第 8.2 节落点说明）。设备信任确认属用户流程
（第 4 节）：M1 无 Trust Manager 与 UI，由宿主经 `AppStateOwner` 的 `UpsertDevice`
更新指令模拟用户确认（owner 侧按信任状态机合法边校验），UI 于 M5 接入；会话建立
经 `ConversationManager::ensure_conversation`（宿主/用户流程调用）。

受控关闭的 `EXEC-01` 步骤 1 钩子由宿主按序组合：请求取消各 Manager 在途可取消
任务（`request_task_cancel`，句柄由 Manager 发起）→ flush 各 Manager 至泵静止并
消费在途 future → 停 Adapter 投递（fake：`set_sink(nullptr)` + `stop_discovery`）→
`AppStateOwner.close()`（M2 起钩子末尾追加持久化作业排空，见第 11.1 节）；其后才
进入 `ExecutorOwner` 的步骤 2~5。M1-05 不启用
blocking worker：`ExecutorOwner::start_blocking_worker` 仍是唯一注册入口，首次启用
预期为 M2（历史读写 I/O）；M3 托管第三方事件循环时按 pinned 指南
event-loop-interop 模式评估，M4 承载文件 I/O。

## 9. GUI

桌面客户端使用 C++，GUI 采用 EUI-NEO。Aki
的主要界面元素是列表、聊天窗口、消息卡片、文件传输、设备详情、设置和弹窗，与
EUI-NEO 当前的组件化 C++ UI 模型能够直接对应，同时不需要额外引入 WebView
和 JavaScript/TypeScript bridge。

主界面采用常见的三栏布局：

``` text
┌────────┬────────────────────┬──────────────────────────────┐
│        │ Devices            │ alpha17001                   │
│        │                    │ Linux · Online · Direct      │
│   💬   │ ● alpha17001       ├──────────────────────────────┤
│        │                    │                              │
│   🖥   │ ● humanoid-01      │ Hello                        │
│        │                    │                              │
│   📁   │ ○ home-pc          │             Hello 👋         │
│        │                    │                              │
│   ⚙    │ ● Miracle-Pixel    │ 📦 policy.pt                 │
│        │                    │ ███████░░ 73%                │
│        │                    │                              │
│        │                    ├──────────────────────────────┤
│        │                    │ ＋ Message...             ➤ │
└────────┴────────────────────┴──────────────────────────────┘
```

第一阶段左侧导航保留 `Conversations`、`Devices`、`Transfers` 和
`Settings`。Devices 处理发现、身份、信任和连接信息；Conversations
承载日常通信；Transfers 集中管理文件任务。

界面令牌、语义色板、状态视觉语义与组件映射由
[Aki UI 设计规范](aki_ui_design.md)固定。

EUI-NEO 核心采用
Apache-2.0。正式发行前仍需检查实际引入的第三方库、字体、图标、shader
和其他 assets
的许可证，示例项目中来源不明确或带非商业限制的素材不直接进入发行版本。

## 10. 状态管理与线程边界

EUI-NEO 不直接操作 Heyaki transport。Heyaki 产生的异步事件先进入对应
Manager，由 Manager 更新 Application State，UI 只消费状态变化。

``` cpp
struct AppState {
    DeviceStore devices;
    ConversationStore conversations;
    MessageStore messages;
    TransferStore transfers;
};
```

主要事件包括：

``` text
device discovered
device connected
device disconnected
message received
message delivered
transfer started
transfer progress
transfer completed
connection path changed
```

网络线程负责接收数据和产生事件，不直接修改 UI。Application State
是网络侧与渲染侧之间的状态边界。这个边界需要在早期固定，否则大文件传输和多设备连接加入后，网络回调直接进入
UI 很容易形成难以控制的跨线程状态修改。

### 10.1 executor::comm 语义映射

Application State 的跨上下文交付落在 pinned executor（v0.5.0-7 @ `74a9419`）的
`executor::comm` 组件上（总计划 `EXEC-03`）。按交付语义选型，不自建队列，也不以
“共享可变状态 + mutex + 条件变量”替代：

| 交付语义 | 组件 | 说明 |
| --- | --- | --- |
| Application State 本体（网络侧 ↔ 渲染侧状态边界） | `DoubleBuffer<AppState>` | 完整一致快照，单写多读（SWMR）；消费侧持有 `sequence`，经 `try_load` / `load_newer_than` 去重取新 |
| 只关心最新值的单值状态（如当前连接路径摘要） | `LatestMailbox<T>` | 覆盖式 latest-wins；中间值可被覆盖，不承载逐条必达事件 |
| 事件广播（诊断 / 日志 / 后续 Agent 观察者） | `Topic<std::shared_ptr<const AppEvent>>` | in-process、无重放、best-effort；每订阅者独立有界队列（默认 `RejectNewest`），发布方必须检查 `TopicPublishResult` 的拒绝计数 |
| 上表 9 类必达事件的投递主路径 | `MpscChannel<AppEvent>` | 逐条 FIFO 必达；有界容量，满即拒绝（`EXEC-02` 维持不变） |

单写者纪律：SWMR 是 `DoubleBuffer` 的硬契约，多个写者并发发布会互相覆盖更新。各
Manager 的状态更新与事件经 `MpscChannel` 汇聚到单一状态 owner（Manager 侧，
`DEC-002` / `RULE-02`），由 owner 在其执行上下文内合成新 `AppState` 后发布；任何其他
上下文不得直接写快照。事件进入投递主路径时由 owner 统一分配单调递增序列号，消费侧
据此排序与去重。

Manager → owner 的状态更新为类型化指令（`AppStateUpdate`，与 9 类事件配套，路由见
第 8.3 节）：`UpsertDevice` / `UpsertConversation` / `UpsertMessage` / `UpsertTransfer`
（整体 upsert）、`UpdateTransferProgress`（进度部分更新）、`SetPresence`（设备在线
状态，仅 presence 字段，不触发信任状态机）、`SetDeliveryState`（送达回报部分更新）、
`CompleteTransfer`（传输终态宣告，`final_state` 仅取 `Completed` / `Failed` /
`Cancelled`）与 `SetConnectionPath`（覆盖式单值摘要，落 `LatestMailbox`）。owner
逐条校验：目标与当前一致为幂等 no-op；非法转移、终态复活与未知 id 一律拒绝并经
`updates_rejected` 可观测（`RULE-08` / `RULE-09`）。

接受后处理器（M3-02 契约，[DEC-009](../decisions/DEC-009-appstate-write-path.md)）：
`AppStateOwner` 构造注入可选的接受后处理器（`PostAcceptHandler =
std::function<void(const AppStateUpdate&)>`，构造入参、默认空——M1/M2 形态兼容；
对齐 `ManagerPump` 的 Handler 先例，`DEC-008`）。`drain_updates` 在 `apply()` 返回
true（更新被接受，含幂等 no-op）后，于 owner 单写者上下文按接受顺序同步调用处理器
恰好一次；被拒绝的更新不调用。处理器供组合根把第 11.1 节 ① 的 DB 作业按接受顺序
入队 `DatabaseWorker`（control 的存在时序见第 11.1 节 ②）。处理器契约：**不得抛出
异常**——owner 侧全捕获并计入 `post_accept_failures`（可观测计数，`RULE-09`），
异常不上浮、不中断本批 drain 的后续更新与事件处理；入队拒绝（通道满/已关闭/未
注册）由处理器侧计数与 `DatabaseWorkerControl` 的 rejected 计数双重暴露，不回滚
已接受的内存更新（状态边界单向）。

三条硬约束：

1. **单写者强制**：`AppState` 快照发布、事件序列号分配与观察者扇出只发生在状态
   owner 的单一执行上下文；Manager 只投递，UI 只读取。
2. **快照复制成本**：`DoubleBuffer` 每次读取整体复制 `T`。M1 的 `AppState` 为带容量
   预算的值语义集合体；当 stores 增长使复制成本可观时，必须改为
   `shared_ptr<const AppState>` 不可变句柄发布（executor API 文档 7.8 建议）。该变更
   属公开契约变更，先更新本节再改代码。
3. **关闭顺序归 `app/lifecycle` owner（`EXEC-01`）**：先停止 Heyaki / Manager 事件
   生产者，再 drain 并关闭各 `MpscChannel` / `Topic`，最后停止快照发布并让 UI 完成
   末次读取。`comm` 组件与 Executor 的 `shutdown` 零耦合，不参与 Executor 关闭；
   关闭后的通道仍可排空存量，之后投递返回 `Closed`；析构前必须保证没有并发成员调用。

第 10 节事件共 9 类；其中 `transfer completed` 承载传输终态结果
（`Completed` / `Failed` / `Cancelled`），迟到的进度或结果事件不得让已终结的传输回到
活动状态（`RULE-08`）。

## 11. 本地数据

本地持久化可以使用 SQLite，保存设备身份、受信设备、Conversation
metadata、消息历史、Transfer history 和应用设置。

大型图片、视频和文件本体保存在文件系统中，数据库记录路径、hash、大小、MIME
type 等 metadata。

``` mermaid
erDiagram
    DEVICE ||--o{ CONVERSATION : participates
    CONVERSATION ||--o{ MESSAGE : contains
    MESSAGE ||--o| TRANSFER : references

    DEVICE {
        string device_id
        string display_name
        blob public_key
        int trust_state
    }

    CONVERSATION {
        string conversation_id
        string remote_device
    }

    MESSAGE {
        string message_id
        string conversation_id
        int type
        blob payload
        int state
    }

    TRANSFER {
        string transfer_id
        string message_id
        string file_name
        int state
        int transferred
        int total
        string path
        string hash
        int size_bytes
        string mime_type
    }
```

表列与 `DEC-004` 对齐，是 `M2-04` schema（4 张表起步版本）的权威来源：`path` 存
POSIX 相对路径，`hash` 为终态流式 SHA-256，`size_bytes` / `mime_type` / `file_name`
为文件本体 metadata（`file_name` 是远端原始文件名，仅供展示、禁止拼入路径）；
`transferred` / `total` 承载传输进度，供 `UpdateTransferProgress` 部分更新与重启
恢复。

### 11.1 持久化集成契约（M2 契约，M2-01；M3-02/DEC-009 修订）

本小节固化持久化与应用层的集成契约。引擎与访问方式、vendored 引入、连接参数、
文件布局与迁移机制以 `DEC-004` 为准；本节固化四类契约——何时写、如何恢复、如何
关闭、文件本体何时动。① 的写入时机经 `M3-02`/[DEC-009](../decisions/DEC-009-appstate-write-path.md)
修订为「接受后处理器」正式落点（第 10.1 节）：M2-07 console 宿主的「快照权威值
镜像」为过渡形态（见其验证记录偏差段，M2 历史记录保持原样），实现自 M3-03+
按本节跟进。

**① DB 写路径映射**：DB 作业由第 10.1 节 typed 更新驱动（权威路径），不由 9 类
事件驱动（事件是通知面）：`UpsertDevice` → DEVICE 行 upsert；`UpsertConversation`
→ CONVERSATION；`UpsertMessage` → MESSAGE；`UpsertTransfer` → TRANSFER；
`UpdateTransferProgress` → TRANSFER 进度字段（`transferred` / `total`）更新
（不新建行）；`CompleteTransfer` → TRANSFER 终态更新；`SetDeliveryState` →
MESSAGE 送达状态列更新（不新建行）——送达回报是消息历史的组成部分，持久化
保证重启恢复后已发消息不丢失送达终态（`M2` 计划退出-1「消息历史逐域一致」）。
`SetPresence` 与 `SetConnectionPath` 不持久化：presence 是易失在线状态（恢复后
默认 `Offline`，由连接事件重建），连接路径是第 10.1 节 LatestMailbox 覆盖式
单值摘要。写入时机（M3-02 正式落点，[DEC-009](../decisions/DEC-009-appstate-write-path.md)）：
更新被状态 owner 接受（计入 `updates_applied`）后，owner 在 `drain_updates` 内调用
其构造注入的接受后处理器（第 10.1 节；对齐 `ManagerPump` Handler 先例，`DEC-008`），
处理器于 owner 单写者上下文按接受顺序同步入队对应 DB 作业——入队不新增执行上下文
（第 10.1 节硬约束 1 的延伸），作业进入 `DatabaseWorker` 的有界工作通道（`EXEC-04`）；
同一实体的作业顺序即更新接受顺序，`DatabaseWorker` 单一 worker 串行消费保持该顺序。
容量预算（`RULE-09`）：owner drain 批 64 × 每接受更新至多 2 个作业（`CompleteTransfer`
的 `Failed` / `Cancelled` 分支产生终态更新 + `.part` 删除两个作业，见 ④）= 128 ≤
通道容量 256；通道满/已关闭/未注册的入队拒绝经处理器侧计数与 `DatabaseWorkerControl`
的 rejected 计数双可见，不静默、不回滚已接受的内存更新；处理器异常策略见第 10.1 节
（不抛出，owner 全捕获并计数）。接受包含幂等 no-op：对已终态传输重复
`CompleteTransfer(Completed)`（第 10.1 节 from==to 视为接受并计入
`updates_applied`）同样入队，由作业侧幂等语义吸收（见 ④ Completed 作业组的
跳过条件），不在入队侧去重——owner 不新增簿记状态。失败语义（`RULE-09`）：作业入队拒绝（通道满或
已关闭）与执行失败（`SqliteError`）均转化为明确结果与可观测事件/计数（
`EXEC-06`），不静默重试、不回滚已接受的内存更新（第 10.1 节状态边界单向）；
内存态与持久化的分歧经失败计数暴露，由上层策略处理。

**② 启动恢复流程**：组合根在 `ExecutorOwner.initialize()` 之后、Manager/Adapter
启动之前，于主线程同步执行恢复——此时尚无并发事件源，不经 blocking worker，
`DatabaseWorker` 在播种完成后才注册：解析数据根目录 → `open`（DB 损坏即干净
失败，不静默）→ `PRAGMA user_version` 迁移 → 逐域加载 DEVICE / CONVERSATION /
MESSAGE / TRANSFER → 清扫无活动 Transfer 行的 `files/tmp/` 残留（依据加载到的
活动行判定，`DEC-004` 崩溃恢复纪律）→ 以加载结果构造 `AppState` 作为初始快照
播种状态 owner（`AppStateOwner` 构造入参；第 10.1 节单写者纪律在启动段的对应
形式：恢复期 owner 尚未运行、无并发写者，恢复数据即首个权威快照）。时序对齐
（M3-02，[DEC-009](../decisions/DEC-009-appstate-write-path.md)）：接受后处理器
捕获的 `DatabaseWorkerControl` 必须先于 `AppStateOwner` 构造存在（组合根在播种前
以恢复移交的单一连接创建 control）；「`DatabaseWorker` 在播种完成后才注册」指
`start_blocking_worker` + `mark_registered()` 的时点，不要求 control 对象晚于
owner 构造。未注册窗口内处理器入队被明确拒绝且 rejected 计数可见（`RULE-09`）——
标准装配序（第 8.3 节）中该窗口无任何更新流（恢复期 owner 未 drain、无事件源），
预期计数为 0，非零即为装配缺陷信号。恢复完成前
不注册 `RouterSink`、不启动发现、不注入任何事件（`EXEC-02` 启动段纪律）；open、
迁移或加载失败时组合根干净退出并输出原因。

**③ `DatabaseWorker` 在 `EXEC-01` 关闭顺序中的落点**：`DatabaseWorker` 经
`ExecutorOwner::start_blocking_worker` 注册（第 8.2 节，M2 首次启用）。其排空
位于第 8.3 节宿主钩子序列的末尾：`… → AppStateOwner.close() → DatabaseWorker
排空（有界预算）`——`close()` 的排空会让最后一批更新被接受并入队 DB 作业，因此
DB 排空必须在 `close()` 之后；又因第 8.2 节步骤 4 的 `wait_for_completion_ex`
不覆盖 blocking worker，排空必须完成于钩子内（`EXEC-01` 步骤 1），其后步骤 2
`request_stop()`（非阻塞置位 + 唤醒）与步骤 3 `stop()`（request + wakeup +
join）才能在不丢作业的前提下回收 worker。`run()` 以有界超时等待工作通道、在
语句间检查 StopToken（取消粒度为语句间，执行中的语句不被打断）；`wakeup()`
唤醒通道等待，满足第 8.2 节步骤 3 的可解除阻塞契约。排空完成后通道关闭，新
作业入队明确拒绝（`RULE-09`）。**transfer IO worker（`aki.transfer-io`，M4-04，
DEC-011）复用本节关闭纪律**：注册同经 `start_blocking_worker`（句柄归 owner，
步骤 2/3 统一回收）；差异仅在排空语义——归档分块无需 DB 式排空（中断归档由
启动清扫/幂等删除收敛），钩子内只要求 TransferManager flush 至**IO 在飞归零**
（其事件回调不得晚于 Manager 终结，第 8.3 节），未竟归档残留 `.part` 按第
11.1 节 ②④ 收敛。

**④ 文件本体生命周期触发点**：`.part` 写入属传输数据链路（发送侧 M4-04 经
transfer IO worker 真实接线，接收侧无 `.part`——heyaki 接收根落盘；M2 以测试
内字节源驱动），写入期固定为 `files/tmp/<transfer_id>.part`。终态处理由 DB 作业
承载、全部在 `DatabaseWorker`（blocking worker，`EXEC-04`）内执行：
`CompleteTransfer(Completed)` 被接受后排队"TRANSFER 终态更新 + 流式 SHA-256 +
原子改名到 `files/<transfer_id>/<净化文件名>` + `hash` / `size_bytes` 回写
TRANSFER 行"的串行作业组。**供源参数化（M4-05，DEC-012①）**：作业组供源
按序取 `.part` → heyaki 接收根文件（`<接收根目录>/<logical_name 段>`，段
拼接前有界校验——拒绝绝对路径/`..`；接收源的就位与原件删除折进同一作业，
接收侧合并不上 transfer IO worker）→ final 恢复分支 → 明确失败。作业组自身
幂等（与删除作业对称）：重复终态宣告是被
接受的幂等 no-op（第 10.1 节）并重复入队，作业执行时若发现 TRANSFER 行已为
`Completed` 且目标文件已存在（先前执行已完成），整组幂等跳过（计成功，不产生
失败计数）；否则按序执行，此时全部供源缺失即流式 SHA-256 明确失败（`RULE-09`，
不静默、不伪造成功）。收发 SHA-256 对账不在作业内执行（发送方哈希无持久面，
DEC-012⑤）——由持有消息载荷的消费者执行，失配即断言失败/可见呈现。
`Failed` / `Cancelled` 被接受后排队 `.part` 幂等删除作业（接收侧无 `.part`
时幂等 no-op，heyaki 失败/取消自清其接收侧残留）；
启动清扫见 ②（仍仅覆盖 `files/tmp`；接收根残留靠作业幂等重跑收敛，长期 GC
归 §6.1 已登记的 M5 兜底议题）。Manager 不做文件 I/O——第 8.3 节职责切分不变：
TransferManager 只写权威状态并经状态更新触发上述作业。数据根目录解析为
Platform Adapter 职责的最小落点：persistence 层内平台条件编译单元（Windows `%APPDATA%` / Linux XDG）提供
数据根目录解析，公开面仅 `std::string` 路径（`RULE-10`，平台相关编译单元保持
可选）；组合根解析一次后经构造参数注入路径字符串（含 heyaki 接收根目录——
配置在数据根内如 `<data_root>/receive/<root>`，NodeSession Options 注入），
其余 Core/persistence 代码
不见平台类型；平台能力增多时再按需升级为独立 Platform Adapter 小节（届时先
更新本节）。

## 12. Capability 与权限

设备通过 capability 声明自己支持的功能。第一版只需要 messaging 和 file
transfer，协议中预留其他能力：

``` cpp
struct DeviceCapabilities {
    bool messaging;
    bool file_transfer;
    bool status_query;
    bool command_execution;
    bool remote_terminal;
    bool agent;
};
```

后续还可以加入 Clipboard、Screen、Camera、Remote Shell、Robot Control
等能力。

Capability 与 permission 分开处理。设备声明
`remote_terminal`，表示它实现了远程终端能力；某个 Trusted Device
是否能够使用该能力，则由 permission 决定。

``` text
Identity + Trust + Capability + Permission
```

这样 `Trusted`
始终只承担设备信任语义，不会随着控制能力增加逐渐变成拥有所有权限的总开关。

## 13. Agent 扩展

Typed message 也用于后续 Agent 接入。运行 Agent 的设备可以声明 `agent`
capability，并通过 `AgentMessage` 返回文本、结构化数据和可执行 action。

例如服务器可以在 Conversation 中返回：

``` text
alpha17001

训练运行 8h 42m。
Best checkpoint: 18200
Reward: 18.2 -> 24.7

[Download Checkpoint]  [View Logs]
```

UI 可以把这类消息渲染成带操作按钮的消息卡片，底层保存结构化数据，不要求
Agent 先拼成 Markdown 再由客户端解析。

如果后续接入 Mira，组件关系保持为：

``` mermaid
graph LR
    Human --> Messenger
    Messenger --> Mira
    Mira --> Heyaki
    Heyaki --> Remote[Remote Device]
    Remote --> Agent[Device Agent]
```

Heyaki 负责设备间通信，Messenger 提供交互界面，Mira
处理更高层的用户意图。三者通过已有接口连接，各自保留自己的状态和职责。

## 14. 工程目录

代码按领域组织：

``` text
device-messenger/
├── app/
│   ├── application
│   ├── state
│   └── lifecycle
├── device/
│   ├── device
│   ├── discovery
│   ├── presence
│   └── trust
├── conversation/
│   ├── conversation
│   ├── message
│   ├── codec
│   └── history
├── transfer/
│   ├── transfer
│   ├── manager
│   └── storage
├── persistence/
│   ├── database
│   ├── repository
│   ├── migration
│   ├── storage
│   └── recovery
├── heyaki/
│   ├── adapter
│   ├── events
│   └── session
├── ui/
│   ├── components/
│   ├── pages/
│   ├── models/
│   └── theme/
├── assets/
└── main.cpp
```

落位说明（M4-04，DEC-011 ②）：传输会话所有者为 app 层
`app/application/transfer_manager.hpp`（DEC-008 装配），`transfer/manager/`
目录自 M4-04 双组件收口后暂空（原 M4-02 独立组件删除，后续如需拆分再启用）；
`transfer/storage/` 承载归档 IO 抽象面（`transfer_io.hpp`，实现于
`persistence/storage/transfer_io_worker.hpp`）。

依赖方向为：

``` text
EUI-NEO UI
    ↓
Application
    ↓
Domain
    ↓
Heyaki Adapter
    ↓
Heyaki
```

UI 通过 Application 使用领域对象，Domain 通过 Heyaki Adapter 访问
Heyaki。页面代码不直接持有底层 transport 对象。

## 15. 第一阶段范围

第一阶段先跑通一条真实、可持续使用的链路：两台设备启动客户端并建立各自身份，在局域网或
Heyaki 网络中发现对方，完成身份确认和信任后建立
Conversation。双方能够发送文本、图片和文件；客户端重启后仍能读取原有联系人和消息历史；网络中断并恢复后，原
Conversation 可以继续使用。

MVP 需要完成 EUI-NEO 主窗口和基础主题、Device Identity、设备发现、Trust
Device、Device List、Conversation
List、一对一文本消息、图片消息、文件传输、传输进度、消息历史、Presence、连接路径展示和断线恢复。

语音、视频通话、远程终端、屏幕控制、设备命令和 Agent 不作为 MVP
前置条件。这些功能继续复用 Device、Conversation、Capability 和
Permission 模型，在基础通信稳定后逐步加入。

## 16. 后续方向

基础消息和文件传输稳定后，可以增加语音消息、语音通话和视频通话，继续覆盖实时通信场景。

设备交互能力可以从状态查询开始，再加入 Clipboard、Remote Shell、Screen
和 Remote Control。对于服务器，Messenger 可以逐步承接一部分原本通过
SSH/SCP 完成的日常操作；机器人和其他设备则根据各自 capability
提供对应的控制界面。

Agent 接入后，设备可以接收更高层请求、执行任务，再通过同一个
Conversation 返回结果。此时 Conversation
同时承载人与设备、设备与设备以及 Agent
相关的交互，但网络中的第一等主体仍然是 Device。

Heyaki 负责设备发现和数据交换，Aki 负责把这些连接组织成
Conversation 和可操作的界面，Agent
能力按需要建立在这套设备通信模型之上。
