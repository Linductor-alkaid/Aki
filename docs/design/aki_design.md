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
