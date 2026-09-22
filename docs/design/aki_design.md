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
- `start_file_transfer(receiver, TransferId, FileMetadata)` /
  `pause_transfer` / `resume_transfer` / `cancel_transfer(TransferId)`：文件传输
  接口面（第 7 节）。M4 前仅签名与 TransferId 语义——一个 `TransferId` 对应一个
  传输会话，不可重复启动；文件本体不经本接口传输（`RULE-05`）。

入站（Adapter → 应用）经 `HeyakiAdapterSink` 纯虚接口投递，方法与第 10 节 9 类事件
一一对应（`on_device_discovered` / `on_device_connected` / `on_device_disconnected` /
`on_message_received` / `on_message_delivered` / `on_transfer_started` /
`on_transfer_progress` / `on_transfer_completed` / `on_connection_path_changed`），
返回值表示投递是否被接受（校验失败或下游背压拒绝可见）。其中
`on_transfer_completed` 的 `final_state` 仅取 `Completed` / `Failed` / `Cancelled`
（第 10.1 节）。

纪律：Adapter 回调只做有界校验与投递（`EXEC-02`），业务处理一律在 Manager 的执行
上下文（M1-05）；事件从 Sink 到 Application State 的桥接由应用层完成——Sink 实现把
事件写入第 10.1 节的事件入口并提交对应的状态更新。测试与冒烟宿主使用同目录的
`FakeHeyakiAdapter`：以 `inject_*` 编程式注入上述入站事件，注入路径即 `EXEC-02`
回调路径（有界校验 + 投递），不做任何真实 I/O。

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

9 类 Sink 事件由 `app/application` 内单一 `RouterSink`（实现 `HeyakiAdapterSink`）
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
| `on_transfer_started` | TM：`UpsertTransfer` | TransferStarted |
| `on_transfer_progress` | TM：`UpdateTransferProgress` | TransferProgress |
| `on_transfer_completed` | TM：`CompleteTransfer(final_state)` | TransferCompleted |
| `on_connection_path_changed` | DM：`SetConnectionPath(to)` | ConnectionPathChanged |

执行上下文与任务承载（`EXEC-04` / `EXEC-05` / `EXEC-07`）：

- 每个 Manager 采用“单飞有界排空”泵：工作项入收件箱 → CAS 抢占单飞标志 →
  `submit_auto` 一个排空任务（有界批量，保留并消费 `TaskSubmission.future`；释放
  单飞标志后复查收件箱，防止丢失唤醒）。事件与宿主命令（发送、传输控制、
  `ensure_conversation`）共用同一收件箱串行处理，Manager 内部状态（如传输会话
  句柄表）只在排空上下文访问。
- 长任务（传输会话类）用 `submit_cancellable` + `StopToken`：Manager 按业务稳定 ID
  持有 `TaskHandle` + future 作为成员；取消一律经
  `executor().request_task_cancel(handle)` 发起——运行期任务协作轮询
  `stop_requested()` 自行退出，排队期取消由 Executor 以
  `TaskCancelled(Explicit)` 结算且不产生 failure 事件；不经 StopSource 直发，业务
  代码不得主动抛 `TaskCancelled` 做控制流（无取消请求时按任务异常计入 failure）。
- 取消断言经 `get_cancellation_status()` / `ExecutorSnapshot.cancellation`（独立
  计数，不入 failure）；超时断言经 failure 体系 `timeout_count`——`task_timeout_ms`
  是排队软超时（config 级），不打断运行中任务。M1 不引入 `TimerHandle` /
  周期任务：presence 与传输进度均以事件到达，Manager 无自驱周期负载；首个真实
  周期负载（presence 刷新 / 进度采样）出现时再按 `EXEC-04` 启用。
- Manager 必须先于其任务终结：宿主关闭钩子（下文装配顺序）保证先取消并消费在途
  任务 future，再进入 `ExecutorOwner` 的 `EXEC-01` 步骤 4/5。

装配与所有权（`EXEC-07`；宿主组合根顺序，自 M1-06 console 起进程内使用）：

1. `ExecutorOwner.initialize()`；
2. `AppStateOwner`；
3. 四个 Manager 构造注入 `executor::Executor&`、`AppStateOwner&`、各自所需的
   `HeyakiAdapter&` 与容量预算；
4. `RouterSink` 经 `FakeHeyakiAdapter::set_sink` 注册。

M1 冒烟宿主（仓库根 `main.cpp`，M1-06）是该组合根的最小进程内实现，进程内 owner
自本项起改用正式 `ExecutorOwner`（见第 8.2 节落点说明）。设备信任确认属用户流程
（第 4 节）：M1 无 Trust Manager 与 UI，由宿主经 `AppStateOwner` 的 `UpsertDevice`
更新指令模拟用户确认（owner 侧按信任状态机合法边校验），UI 于 M5 接入；会话建立
经 `ConversationManager::ensure_conversation`（宿主/用户流程调用）。

受控关闭的 `EXEC-01` 步骤 1 钩子由宿主按序组合：请求取消各 Manager 在途可取消
任务（`request_task_cancel`，句柄由 Manager 发起）→ flush 各 Manager 至泵静止并
消费在途 future → 停 Adapter 投递（fake：`set_sink(nullptr)` + `stop_discovery`）→
`AppStateOwner.close()`；其后才进入 `ExecutorOwner` 的步骤 2~5。M1-05 不启用
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
        string path
        int state
    }
```

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
│   └── migration
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
