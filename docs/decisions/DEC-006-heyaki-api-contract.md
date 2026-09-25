# DEC-006：Heyaki API 契约版本与目标级集成方式

> 状态：Accepted
> 日期：2026-09-23
> 负责人：Linductor
> 冻结里程碑：M3 开始前（本记录即冻结，调研依据见文末）
> 替代/被替代：无（对 [DEC-003](DEC-003-dependency-locking.md) 的 executor
> 构建图接入条款作显式修订，见"影响与风险"）

## 背景与问题

M3 要以 pinned `third_party/heyaki` 替换 `FakeHeyakiAdapter`（[DEC-002](DEC-002-layering-and-state-boundary.md)），
开工前必须冻结总计划遗留的暂定默认值 `DEC-006`：以哪个 heyaki 版本的哪些公开
面为契约基线、以何种方式进入 Aki 构建图、运行期 heyaki 并发与 Aki
`ExecutorOwner`（设计第 8.2 节，`EXEC-01` 唯一 owner 纪律）如何协调，以及设计
第 8.1 节 SPI 九类事件到 heyaki 公开 API 的映射。不冻结则全部真实接入工作项
无法在既定架构约束内推进。

## 决策

- **契约基线**：以 pinned heyaki v1.0.1-38（commit `e114508ab32d496d52e9db9bac26eb1cc88c4ae7`，
  与 [DEC-003](DEC-003-dependency-locking.md) 锁文件一致，2026-09-23 会话内
  `git -C third_party/heyaki rev-parse HEAD` 复核）的公开头文件
  `third_party/heyaki/include/heyaki/` + `docs/api.md` + `docs/client-library.md`
  为契约权威；wire 协议版本 `{1,3}`（api.md「Protocol compatibility」节冻结，
  N-1 同大版本互操作）。pinned commit 不随本决策变更；升级走独立变更并回填本记录。
- **构建接入**：单一构建图内 `add_subdirectory(third_party/heyaki)`，只链接
  `heyaki::client`（`heyaki::services` 为仅含锚点编译单元的伞 target，无符号
  收益，不链）。executor target 由 heyaki 的 `third_party/executor` 提供
 （heyaki `CMakeLists.txt:129` 无条件 `add_subdirectory(third_party/executor)`，
  目标名全局为 `executor`），**Aki 移除根 `CMakeLists.txt` 中自己的
  `add_subdirectory(third_party/executor)`**——两侧 pin 同为
  `74a94198fbe0f2a4081cd260658a26f969986870`（Aki 锁文件、heyaki
  `third_party/dependencies.lock` 与实际 checkout 三方一致，本会话核实），
  单一构建图只编译一份 executor。构建开关冻结：`HEYAKI_BUILD_APPS=OFF`、
  `HEYAKI_AUTO_INSTALL=OFF`，heyaki 测试不进入 Aki 构建面。
- **运行期 executor 协调**：Aki `ExecutorOwner` 仍是进程内唯一 executor 实例与
  唯一 owner；宿主以 `heyaki::Runtime::create_borrowed(executor_owner.executor(),
  cfg)`（`runtime.hpp:205`）创建借用型 Runtime 并经 `NodeConfig.runtime` 注入
  `Node::create`。borrowed Runtime 的 AsioWorker/FileIoWorker 经
  `executor_->start_worker(BlockingWorkerSpec)` 挂在借用的 executor 上（worker
  名 `heyaki-asio` / `heyaki-asio-file-io`），heyaki 并发对 Executor 完全可见；
  Runtime 关闭不触碰宿主 executor 生命周期。禁止 `create_owned` /
  `NodeConfig.runtime=nullptr`（进程内第二 executor 实例，违反 `EXEC-01`）。
  关闭顺序：`Node::shutdown()` + `Runtime::shutdown()` 编入 `EXEC-01` 步骤 1
  的停止生产者钩子（早于 owner 步骤 2/3/5）；断言
  `RuntimeShutdownReport.executor_shutdown_performed == false`（借用模式不代收
  宿主 executor）。非 Executor 能力缺口，不进 9.4 台账。
- **设计第 8.1 节 SPI ↔ heyaki API 映射**（实现权威，M3 各工作项依据）：
  1. 身份/配置：`ProfileStore::create/open` + `initialize_local` +
     `endpoint_for(application_id)`；`DeviceId` = 32 字节 `Identifier`，
     `derive_device_id(public_key)` 实现公钥稳定绑定；aki `DeviceId.value` 取
     `heyaki::to_string(device_id)` 的规范字符串形式（M3-03 实测澄清：为带
     kind 前缀的 `hy1_` 编码串，56 字符，非裸 hex——原「规范 hex」措辞按实现
     修正，权威规则「value = to_string(device_id)」不变）。
  2. 发现：Node 常驻 LAN 广播/监听，无 start/stop discovery API——SPI
     `start_discovery`/`stop_discovery` 在 Adapter 层落为「启停观察管道」：
     `submit_periodic` 轮询 `endpoints()` diff 合成 `on_device_discovered`；
     `DiscoveredDevice` ← `EndpointDirectoryEntrySnapshot`（含
     `identity_public_key`）。已知设备（记录型来源）= ProfileStore 持久化
     TrustGrant/端点记录启动恢复，触发语义由 M3-02 细化设计第 8.1 节。
  3. 信任/配对：`Unknown→Pending` ← `peer_sessions().state==pairing_restricted`；
     `Pending→Trusted` ← `pair_peer` + `set_pairing_observer` 一次性结果；
     配对失败映射 `Rejected`；`revoke_trust_grant` 映射 `Revoked`；指纹确认
     数据 = `LanEndpointSnapshot.identity_public_key`（Ed25519 32B）。配对申请
     scope：`message.send`（前缀通配语义，api.md scope 节）。
  4. 文本消息：`send_text_message` → `send_message(peer, MessageEnvelope{
     message_id=aki MessageId 16B 双射（规范字符串形式：`to_string` /
     `parse_message_id`，`hym1_` 前缀编码——M3-05 实测澄清，非裸 hex）,
     type="aki.text", delivery_mode=
     peer_acked})`；`Result` 失败（如 peer_offline）→ SPI 返回 false 可见
     （`RULE-09`）；`DeliveryState` 映射 queued→`Sent`、acked→`Delivered`（经
     `on_message_delivered`）、send_failed/peer_rejected/ack_timeout/
     session_closed→`Failed` 终态；`on_message_received` ←
     `set_message_inbound_handler`（协议层先去重+ACK）。
     **图片消息面扩展（M4-03，[DEC-010](DEC-010-image-message-contract.md)；
     应用侧集成契约见设计 §6.1/§8.1）**：`send_image_message` → 同型信封
     `MessageEnvelope{message_id 双射同上, type="aki.image", schema_version=1
     （aki 载荷 schema 版本，协议层仅校验非零）, delivery_mode=peer_acked,
     payload=ImagePayload（FileMetadata+TransferId）冻结字段号编码}`；
     `DeliveryState` 映射同 aki.text 四态；入站信封 `type` 分发收敛在 Adapter
     层——`aki.image` → codec 解码 → `on_message_received(Image typed)`，
     未知 type 或解码失败（缺字段/超限/TransferId 非规范）→ 有界拒绝可见
     （不投递 sink、Adapter 拒绝计数，`RULE-09`）；图片本体不经消息通道
     （`RULE-05`），传输面经映射 7。
  5. Presence/路径：`on_device_connected/disconnected` ← `peer_sessions()` diff
     （authenticated↔closed）；`on_connection_path_changed` ← data_path +
     signaling_route diff；映射 direct+lan→`Lan`、direct_srflx→`P2p`、
     turn_*→`Relay`、unknown→`Unknown`。
  6. 会话重建：`restart_session` 同 SessionId、epoch+1、接口变化自动触发——
     `RULE-06`「路径切换不新建会话」的直接依据（`SCOPE-11`）。
  7. 传输四接口（M4 实现级细化，M4-01；2026-09-24 评审定案参数来源与
     paused 投递面，DOD-04 设计先行沿第 10 方法先例）：`start_file_transfer` →
     `push_file(peer, root, logical_name, source_path, transfer_id)`
     （transfer_id 由应用以业务稳定 ID 提供，支持断点续传；`source_path`
     为发送侧本地文件路径，经 SPI 签名扩展传入——
     `start_file_transfer(receiver, TransferId, FileMetadata,
     std::filesystem::path source_path)`，不进入对端可见的 `FileMetadata`；
     `root` 为 heyaki 逻辑根，由组合根经存储配置注入 Adapter 选项，非 SPI
     参数；`logical_name` ← `FileMetadata.name`）；
     `pause_transfer` → `pause_file_transfer`；`resume_transfer` →
     `resume_file_transfer`；`cancel_transfer` → `cancel_file_transfer`；
     `set_file_event_observer` → 状态映射：`transferring`（bytes_done 变化）→
     `UpdateTransferProgress`；`paused` → `UpsertTransfer`（`Paused`；对端
     驱动——含断线自动暂停，可发生于接收侧——经 sink 第 11 方法
     `on_transfer_paused(TransferId)` 投递，M4-05 落地；本地暂停确认后同此
     映射）；
     `committed` → `CompleteTransfer`（`Completed`）；`failed` →
     `CompleteTransfer`（`Failed`）；`cancelled` → `CompleteTransfer`
     （`Cancelled`，`.part` 删除作业组）；`probing`/`offered`/`verifying`
     中间态 → `Negotiating`/`Transferring` 推进不单独持久化；
     `pull_file`（拉取方向）M4 暂不接入（接口预留）。设计侧契约见设计
     §7.1（M4-01 固化：含 BLAKE3 wire 校验与 SHA-256 存储哈希层次澄清、
     分块 IO 不经 DatabaseWorker 通道的承载结论）。原条目：
     `push_file`/`pause`/`resume`/`cancel` +
     `set_file_event_observer` → `on_transfer_*`。
  - 回调线程：Node 回调在 executor 上下文触发（api.md），与 `EXEC-02`
    「有界校验 + 投递」对齐；业务 handler 一律在 Manager 执行上下文。
- **冻结常量**（防止各工作项各自发明编码）：`application_id = "org.aki.app"`；
  `DeviceId`（32B）/`MessageId`（16B）/`TransferId`（16B）与 heyaki
  `Identifier` 的字节 ↔ 规范字符串（`to_string` 编码形式）双射——TransferId
  规范形式为 `hyt1_` 前缀 + 26 个 base32 字符（31 字符，`parse_transfer_id`
  对非规范形式解码拒绝；M4-03 显式点名，沿 M3-05 `hym1_` 澄清先例）；envelope
  `type = "aki.text"`（文本消息面）与 `type = "aki.image"`（图片消息面，
  M4-03——aki 图片载荷 schema：`schema_version=1`，冻结字段号 1=name(≤512B)/
  2=size_bytes(varint)/3=mime_type(≤128B)/4=transfer_id(字段 4 规范串)/
  5=stored_sha256(预留 M4-04)，解码跳过未知字段（前向容忍）、缺 1~4/超限/
  非规范 → 有界拒绝可见，载荷总量 ≤4KiB（aki 侧上限，紧于 heyaki 1MiB）；权威
  细节见 [DEC-010](DEC-010-image-message-contract.md) 与设计 §6.1①）；
  配对 scope = `message.send`。
- **已知语义缺口（如实记录，不静默）**：LanPresence 不携带
  display_name/device_class/os_name/capabilities 元数据——`DeviceIdentity` 这些
  字段 M3 为占位值，后续经 RPC 能力查询或 heyaki 协议演进解决；发现启停为
  Adapter 观察管道语义（Node 常驻）；Relay/邀请链接/手动输入发现来源分期按
  M3 里程碑范围条款记录。

## 备选方案

- **双 executor 单图**（Aki 保留自己的 executor add + add heyaki）：heyaki
  CMakeLists 无 guard 地 add executor 子目录，目标重名 configure fatal；也不得
  改 third_party（AGENTS.md）。否决。
- **heyaki 官方 SDK 路径**（构建安装 SDK + `find_package(heyaki)`）：heyaki 文档
  支持的消费方式，但 SDK 静态内嵌自己的 libexecutor——继续用自带 executor 则
  进程内两份 executor 代码副本（监控割裂、跨副本传 `Executor&` 属正式 UB）；
  改用 SDK 导出的 executor 包则 executor 变为二进制产物（provenance/tsan preset
  需重造 SDK）。仅当 M3-01 实测单图方案被工具链硬阻塞时凭新证据重启。否决，
  保留为回退。
- **`NodeConfig.runtime=nullptr`（heyaki 默认 owned）**：进程内第二个 executor
  实例，违反 `EXEC-01`/AGENTS 规则 7-8 唯一 owner 与「不得隐藏 Executor 生命
  周期」。否决。
- **链接 `heyaki::services`**：空伞 target（仅 `src/services/module.cpp` 锚点，
  `PUBLIC heyaki::client`），无符号收益。否决。

## 影响与风险

- **DEC-003 显式修订**：Aki 根 CMakeLists 的 executor add 块（含
  `EXCLUDE_FROM_ALL` 与 SYSTEM include/告警豁免设置）移除后，上述设置须转移
  到 heyaki 提供的 executor target；`dependencies.lock.json` executor 条目的
  `used_by` 说明更新为「经 heyaki 单图间接进入构建（同 commit）」；
  configure 期校验扩展为同时核对 Aki lock、heyaki lock 与 checkout 三方
  executor commit 一致（任一侧升级即破坏注入前提）。
- **首次配置网络与 bash**：heyaki 依赖树（boost 模块/libdatachannel/libsodium/
  blake3/sqlite/OpenSSL…）经 `scripts/fetch_third_party.sh` 按 ref+commit 双校验
  拉取（当前 `third_party/heyaki/third_party` 仅锁文件未拉取，已核实）；Windows
  需 bash（Git for Windows）；CI Linux 需网络+bash+libssl-dev，建议
  actions/cache 缓存。与 DEC-003「submodule + configure 校验」的偏差已在本记录
  声明锁链（Aki lock(heyaki@e114508) → heyaki lock(executor@74a9419)）。
- **双 SQLite 符号**：Aki vendored sqlite 3.53.4（persistence）与 heyaki
  `heyaki::sqlite` 3.50.4（profile PRIVATE）在同一静态链接中各含全套
  sqlite3_* 符号——M3-01 必须用 dumpbin/nm 验证最终二进制仅一份 sqlite3 符号
  并记录有效版本；出现行为冲突再立统一决策（改 DEC-004 或 Aki 改链
  heyaki::sqlite），不得静默。
- **三工具链**：MSVC（OpenSSL 3 系统依赖，本地与 CI 需 `OPENSSL_ROOT_DIR` 并
  部署 libssl-3-x64.dll/libcrypto-3-x64.dll；构建树内链 LibDataChannelStatic
  预期无需 datachannel.dll，需实测）；MinGW 维持 configure-only 并如实记录
  限制与补跑条件（pinned executor 本体在 w64devkit 不可构建，M1-02 限制 1，
  heyaki 强依赖 executor 故同样受限）；CI Linux 三档（debug/asan/ubsan/tsan）
  为全量构建主路径。
- **TSAN 联动**：heyaki 将 `EXECUTOR_ENABLE_TSAN` 强制 OFF，但
  `HEYAKI_SANITIZER=thread` 时对 executor target 直接补 `-fsanitize=thread`——
  Aki tsan preset 必须联动 `HEYAKI_SANITIZER=thread`，M3-01 以构建日志/
  ExecutorSnapshot 验证全图插桩（`DOD-03`）。
- **容量预算合并**：heyaki async pool 负载与 2~3 个 blocking worker 并入后，
  `ExecutorOwner` 的 `ExecutorConfig` 需按合并负载显式定容（borrowed 模式下
  RuntimeConfig 的 executor 线程参数不生效，仅 Aki 侧可 sizing）；EXEC-06 对账
  可用 heyaki RuntimeSnapshot 的 executor_* 计数（读自同一实例）。

## 验证方式

本记录依据 2026-09-23 冻结调研（负责人 Linductor）：pinned commit、双侧
executor pin 三方一致、`create_borrowed` 签名、heyaki 无条件 executor 子目录、
wire {1,3} 冻结、services 伞 target 构成等关键事实已在冻结会话内逐项静态核实
（git rev-parse / 头文件 / CMakeLists / api.md 引用见上文行号）。以下动态验证
归 M3-01（调研未编译未运行，全部为静态证据，如实声明）：三套工具链 configure/
build；最终二进制单份 sqlite3 符号与有效版本（dumpbin/nm）；borrowed 关闭路径
（`executor_shutdown_performed==false` + owner `fully_stopped()`）；tsan 全图
插桩；MSVC DLL 部署集。CI 全绿随 M3 各 PR 门禁。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 8.1 节（SPI 映射权威指向本记录）、
  第 8.2 节（executor 协调落点）、第 8.3 节（关闭钩子序列）、第 6.1 节
  （Image 消息 wire 契约与收发状态联动，M4-03）
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-01/02/03/05/06/07/10/11`、
  `EXEC-01`/`EXEC-02`/`EXEC-04`、M3、M4）
- [DEC-002](DEC-002-layering-and-state-boundary.md)（真实 Heyaki 在 M3 接入）、
  [DEC-003](DEC-003-dependency-locking.md)（锁定纪律；executor 接入条款按本记录
  修订）、[DEC-008](DEC-008-manager-routing-and-executor-tasks.md)（回调路由与
  序语义重评条款）、[DEC-010](DEC-010-image-message-contract.md)（图片消息
  wire 契约与收发状态联动——本记录映射 4/冻结常量的 M4-03 扩展权威）
- [M3：Heyaki 真实接入与文本消息](../plans/m3-heyaki-integration.md)（`M3-01`~`M3-09`）、
  [M4：图片消息与文件传输](../plans/m4-image-file-transfer.md)（`M4-03`）
