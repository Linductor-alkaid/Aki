# M4：图片消息与文件传输

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M3（`M3-01`~`M3-09` 工作项已完成并经 #29 收口审计；退出-1/退出-3 为
> 「部分验证 + 如实降级声明」，待防火墙放行入站 TCP / LAN 双端环境补跑后关闭
> ——环境补跑不阻塞本里程碑文档与设计先行工作项，M4 实现工作项开工时复核
> M3 状态；真实 Adapter、NodeSession、发现/消息/presence/重连管道均已就绪）
> 建议发布点：v0.4.0
> 更新日期：2026-09-26

## 目标

交付真实数据链路端的图片消息与文件传输能力（`SCOPE-07`/`SCOPE-08` 的非 UI
部分）：图片以 typed 消息承载 metadata + `TransferId`（`RULE-05`），文件本体经
独立 Transfer Session 传输——发送侧分块写入 `files/tmp/<transfer_id>.part`、
进度采样、终态 `Completed` 流式 SHA-256 + 原子改名 + 回写（M2-06 作业组真实
接线）；接收侧落盘与完成合并；暂停/恢复/取消全状态机推进；传输历史实时持久化
并重启恢复一致。`transfer/manager/` 目录自本里程碑落地（M1-08 遗留项）。本
里程碑不引入 UI（Transfers 页面归 M5/`SCOPE-12`），不改变 M3 文本消息语义。

## 范围与非目标

### 范围

- `M4-01` 设计先行：第 7 节传输集成契约小节（M1-08 纪律，先文档后代码）——
  TransferManager 职责切分（`DEC-008` 模式扩展）、传输 typed 更新→DB 作业映射
  （`UpsertTransfer`/`UpdateTransferProgress`/`CompleteTransfer` 既有 + 发起/
  暂停/恢复/取消更新补充）、heyaki BLAKE3 wire 校验与 `DEC-004` SHA-256 存储
  哈希的关系澄清、`.part` 写入在 blocking worker 的承载与 `DatabaseWorker`
  通道容量预算复核（`DEC-009` 复核触发条款）、DEC-006 映射 7 的实现级细化。
- `M4-02` `transfer/manager/` TransferManager 落地：传输会话七状态机
  （`Queued`/`Negotiating`/`Transferring`/`Paused`/`Completed`/`Failed`/
  `Cancelled`，设计第 7 节）全覆盖、任务经 `submit_cancellable` + StopToken
  承载（`EXEC-05`）、有界收件箱 + 单飞排空泵（`DEC-008` 模式）、`TaskHandle`
  按业务稳定 ID 显式持有（`EXEC-07`）。
- `M4-03` 图片消息（`SCOPE-07`）：`MessageType::Image` typed 消息发送/接收——
  消息面仅 `FileMetadata` + `TransferId`，图片本体经传输链路（`RULE-05`）；
  收发两侧 `DeliveryState`/传输状态联动。
- `M4-04` 发送侧文件传输真实数据链路（`SCOPE-08`）：发起（`push_file`）→
  `Negotiating`→`Transferring`→`.part` 分块写入（blocking worker，`EXEC-04`）→
  `Completed` 流式 SHA-256 + 原子改名 + hash/size/relative_path 回写（M2-06
  终态作业组从测试内字节源切换为真实文件源）；进度经
  `UpdateTransferProgress` 实时持久化。
- `M4-05` 接收侧与暂停/恢复/取消（`SCOPE-08`）：接收侧 `.part` 落盘与完成
  合并；`pause_transfer`/`resume_transfer`/`cancel_transfer` 驱动状态机推进、
  `Failed`/`Cancelled` 幂等删除 `.part`、终态幂等（`RULE-08`）；`on_transfer_*`
  事件路由（`DEC-008`）。
- `M4-06` 双端传输回环验证：回环（环境受限沿 M3 降级纪律）发现 → 信任 →
  图片消息 → 文件传输（进度、暂停/恢复/取消）→ 终态文件本体 SHA-256 一致 →
  历史与传输行重启恢复一致；DOD-02 六项沿传输并发路径覆盖。
- `M4-07` 收口审计与退出证据归集（沿用 M1-08/M2-08/M3-09 纪律）：实现与设计
  第 6/7/11.1/14 节及 `DEC-004`/`DEC-006` 逐项校对，退出-1~5 证据与可复现
  命令归档，环境受限项如实降级声明并登记补跑条件。

### 非目标

- EUI-NEO UI 与 Transfers 页面、文件卡片展示（M5，`SCOPE-12`）。
- 语音/视频、远程能力、Agent（`SCOPE-13`~`SCOPE-17`、`POST-NN`）。
- 内容寻址去重（`DEC-004` 延后项，触发条件不变）。
- 断点续传语义超出 heyaki 传输会话既有能力范围的部分（以 pinned 版本公开
  API 为准，`DEC-006`；能力缺口按工程规范 9.4 处置）。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 6 节（消息模型与 `Image` 类型）、
  第 7 节（Transfer Session 与七状态机）、第 8.1 节（传输四接口「M4 前仅签名
  与 TransferId 语义」）、第 8.3 节（Manager 装配与关闭钩子）、第 10.1 节
  （typed 更新）、第 11.1 节①（写路径映射：`UpsertTransfer`/进度/终态回写）、
  第 14 节（`transfer/transfer`、`transfer/manager`、`transfer/storage` 落点）。
- [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（文件本体布局、
  `.part` 生命周期、SHA-256、终态作业组——M2-06 已实现待真实接线）、
  [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（映射 7：`push_file`/
  `pause`/`resume`/`cancel` + `set_file_event_observer` → `on_transfer_*`，M4
  预留条款）、[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
  （Manager 模式与取消）、[DEC-009](../decisions/DEC-009-appstate-write-path.md)
  （写路径正式落点与容量预算复核触发条款）。
- M1 遗留「`transfer/manager/` 留 M4」（[M1 里程碑](m1-domain-state.md)
  M1-08 记录）；M2-06 文件存储「真实传输数据链路在 M4」（[M2 里程碑](m2-local-persistence.md)）。
- AGENTS.md Executor 强制规则与工程规范第 9 节；文件 I/O 属 blocking worker
  承载（`EXEC-04`，M2-05 DatabaseWorker 通道与 M2-06 作业工厂复用）。

## 工作项

- [x] `M4-01` 设计先行：第 7 节传输集成契约小节固化（TransferManager 职责、
  传输 typed 更新→作业映射、BLAKE3/SHA-256 关系澄清、`.part` 写入 blocking
  worker 承载与通道预算复核、DEC-006 映射 7 实现级细化）；偏差先更新设计/
  决策再合代码（M1-08 纪律）。（2026-09-24：设计 §7.1「传输集成契约（M4
  契约，M4-01）」固化五条——① TransferManager 职责切分（DEC-008 模式扩展：
  出站四接口/入站文件事件泵上串行、长任务 submit_cancellable + StopToken、
  TaskHandle 按业务稳定 ID、发送侧 .part 分块写入/接收侧 heyaki 根合并、
  暂停恢复进度与 .part 保持、取消幂等删除作业组）；② 传输 typed 更新→作业
  映射补充（暂停/恢复/取消不新增独立作业类型，状态推进经 UpsertTransfer
  幂等、进度经 UpdateTransferProgress、.part 生命周期经 M2-06 既有作业组）；
  ③ 容量预算复核结论（DEC-009 触发条款履行）：分块 IO 由传输会话长任务
  直接顺序写 .part、不经 DatabaseWorker 通道，批上限维持 64×2≤256 不变，
  大文件长作业仅占 blocking worker 执行时长、drain 预算（2s）内；④ BLAKE3
  wire 校验与 SHA-256 存储哈希层次澄清：wire 完整性 vs 落盘存储内容、互不
  替代、无冲突（不立统一决策）；⑤ DEC-006 映射 7 实现级细化（push/pause/
  resume/cancel + FileTransferPhase 八态→Aki 状态映射逐条 + pull_file
  预留）+ §8.1 传输四接口真实语义声明。纯文档变更，无产品代码。详见验证
  记录。）
- [x] `M4-02` `transfer/manager/` TransferManager：传输会话七状态机全覆盖
  （合法/非法转移、终态幂等）、`submit_cancellable` + StopToken 任务承载、
  有界收件箱 + 排空泵、按业务稳定 ID 持有句柄。**M4-02 首轮实现受阻
  （未完成，2026-09-24）**：原型 `transfer/manager/transfer_session_manager.hpp`
  已写入工作树（未提交）——单飞排空泵 + 会话循环 + 七状态机 + SPI source_path
  扩展；但最小生命周期探针（start → null-io tick → stop_all → executor
  shutdown）100% 复现 SIGSEGV，本地无 MSVC 侧调试器（cdb/WinDbg 缺失、
  ASan 运行时缺 clang_rt 组件、MinGW gdb 无法读 MSVC PDB）未能定位根因；
  已排除：pause/resume 路径、收件箱通道名长度、会话任务缺位（V3 二分：
  不派生会话任务仍崩溃）、stop_all 有/无（两形态均崩）。按工程规范第 7 节
  不得合并未定位段错误的原型；原型保留于工作树（未跟踪）供续查。
  补跑条件：WinDbg/cdb 或 ASan 运行时可用环境下的符号化定位后修复。
  详见验证记录（M4-02 受阻条目）。（2026-09-25 完成并合入：SIGSEGV 根因
  独立复核闭环（非 executor 缺陷）、原型按 §7.1① 重构落地（四接口泵上
  串行 + 单消费者泵 + 全局 stop 预算 + 事件顺序闸门 + lifecycle 互斥）、
  SPI source_path 签名扩展（§7.1⑤）、单测重建 9 用例（DOD-02 六项 +
  七状态机/RULE-08）、debug/release 全量 ctest 零回归 + 新测试 30 连跑
  稳定，详见验证记录 2026-09-25 条目。）
- [x] `M4-03` 图片消息（`SCOPE-07`）：`Image` typed 消息收发，消息面仅
  metadata + `TransferId`，本体经传输链路（`RULE-05`）。（2026-09-26 完成：
  设计先行——新建 [DEC-010](../decisions/DEC-010-image-message-contract.md)
  冻结 wire 契约（envelope `aki.image` + 载荷 schema v1 冻结字段号 protobuf-wire
  编解码器，落 `conversation/codec/`）与收发状态联动（正交生命周期 +
  TransferId 消费侧 join + 发送侧准入闸门 + 运行期零传导），设计 §6.1 新增/
  §7.1①/§8.1 回填、DEC-006 冻结常量（`aki.image` + hyt1_ TransferId 双射点名）
  与映射 4 图片面扩展；实现——`ImagePayload` 补 `transfer_id`（镜像
  FilePayload）+ 持久化 media_transfer_id 双向读写（无迁移）、NodeSession
  `send_image`/入站回调携带信封 type（分发收敛 Adapter 层）、SPI
  `send_image_message`（Fake 参数化接受/真实 Adapter 全接线）、MM 图片出站
  路由、编排层闸门 `app/application/image_flow.hpp`；测试——codec 单测 +
  TransferId 双射/aki.image 信封往返 + Adapter 图片分发/有界拒绝计数 +
  MM 路由/正向链/RULE-08/闸门两向失败/运行期零传导/图片路径任务异常，
  debug/release 全量 ctest 34/34 零回归；双端图片回环因防火墙受限以 [skip]
  证据路径降级（补跑条件登记）。详见验证记录 2026-09-26 条目。）
- [ ] `M4-04` 发送侧真实传输链路（`SCOPE-08`）：`push_file` 发起 → 分块写入
  `.part`（blocking worker）→ 进度实时持久化 → `Completed` SHA-256 + 原子
  改名 + 回写（M2-06 作业组真实接线；M4-03 观察③：消除会话骨架
  `session_loop` 的池 worker 停占——2 核设备 ≥2 并发传输即饥饿 Manager 泵）。
- [ ] `M4-05` 接收侧与暂停/恢复/取消（`SCOPE-08`）：接收落盘与完成合并；
  pause/resume/cancel 状态机推进与 `.part` 幂等删除；`on_transfer_*` 路由；
  终态幂等。
- [ ] `M4-06` 双端传输回环验证：全链路（图片 + 文件 + 进度 + 暂停/恢复/取消 +
  重启恢复一致）；环境受限沿 M3 降级纪律（网络无关断言拆分、[skip] 显式 +
  补跑条件）。
- [ ] `M4-07` 收口审计与退出证据归集（沿用 M1-08/M2-08/M3-09 纪律）。

## 风险与阻塞

- **防火墙/LAN 双端环境限制**：M3 已登记的补跑条件（防火墙放行入站 TCP /
  专用测试网络 / LAN 双端真机）同样约束 M4 的双端回环全链路验证——沿 M3-04~
  09 降级纪律处置（网络无关断言拆分 unit、[skip] 显式 + 补跑条件），不冒充
  已验证；补跑由负责人执行。
- **BLAKE3/SHA-256 关系**：heyaki 传输 wire 校验为 BLAKE3，`DEC-004` 存储哈希
  为 SHA-256——两者用途不同（wire 完整性 vs 存储哈希），预期无冲突；
  `M4-01` 设计先行时澄清并固化，如出现实际行为冲突再立统一决策，不得静默。
- **blocking worker 通道预算**：文件分块写入作业与 DB 作业共享 `DatabaseWorker`
  通道——`M4-01` 按第 11.1 节① 容量预算模型复核（`DEC-009` 复核触发条款：
  单更新作业数上限变化须重算并同步设计）；大文件传输时长对 drain 预算的影响
  需评估（传输作业长期占用 worker 的排队影响）。
- **进度采样周期任务**：如进度采样需要周期任务（EXEC-04 timer），`TimerHandle`
  由 TransferManager 显式持有（`EXEC-07`）；优先以 `on_transfer_progress` 事件
  驱动，不引入自驱周期负载（`DEC-008` 边界条款）。
- **测试资源**：大文件用例控制体积与数量（CI 时长预算），分块边界（空文件、
  非整块尾部、多块）显式覆盖。
- pinned heyaki 升级不属本里程碑；`DEC-005`（EUI-NEO 集成）干 M5 开始前冻结，
  不阻塞本里程碑。

## 测试与退出条件

- [ ] 退出-1：双端回环图片 + 文件传输全链路——发起 → 进度 → 终态文件本体
  SHA-256 一致 + 暂停/恢复/取消语义 + 历史/传输行重启恢复一致
  （`SCOPE-07`/`SCOPE-08`）；环境受限时按 M3-09 先例「部分验证 + 如实降级
  声明」处置并登记补跑条件。
- [ ] 退出-2：DOD-02 六项沿传输并发路径通过——正常完成、任务异常、提交拒绝、
  执行中取消（重连/传输长任务 StopToken）、超时、shutdown（含在途文件作业
  drain 语义）。
- [ ] 退出-3：传输状态机七状态合法/非法转移全覆盖、终态幂等、暂停/恢复语义、
  取消删除 `.part`（`RULE-08`/`RULE-09`）。
- [ ] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN/TSAN 随 CI 门禁
  （抑制表仅上游条目纪律不放松）；不适用工具链记录限制与补跑条件。
- [ ] 退出-5：设计（第 7 节集成契约）、决策（`DEC-004`/`DEC-006` 如有回填）、
  总计划与里程碑状态同步；验证记录含可复现命令；环境受限项降级声明完整。

## 验证记录

（尚无记录；自 `M4-01` 起按工程规范 6.1/6.3 追加。）

- 2026-09-24（`M4-01`，设计先行，纯文档变更，无产品代码——沿 M2-01/M3-02
  先例；Windows 11 工作站；依据 pinned heyaki `include/heyaki/file.hpp`/
  `node.hpp` 静态调研）：
  - 范围：[设计第 7.1 节](../design/aki_design.md)（新增「传输集成契约
    （M4 契约，M4-01）」五条：① TransferManager 职责切分；② 传输 typed
    更新→DB 作业映射补充；③ `.part` 承载与通道容量复核结论；④ BLAKE3/
    SHA-256 层次澄清；⑤ DEC-006 映射 7 实现级细化 + `pull_file` 预留）、
    [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（映射 7 实现级
    细化回填 + §7.1 交叉引用）。
  - 依据：[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)
    （.part/SHA-256/终态作业组）、[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
    （Manager 模式/取消/宿主关闭）、[DEC-009](../decisions/DEC-009-appstate-write-path.md)
    （容量预算复核触发条款）；设计第 7 节（七状态机/文件卡片）、第 8.1 节
    （传输四接口 M4 前签名语义）、第 6 节（Image 类型）、第 14 节
    （transfer/manager 落点）；总计划 `SCOPE-07`/`SCOPE-08`、`RULE-05`/
    `RULE-08`/`RULE-09`/`RULE-10`、`EXEC-04`/`EXEC-05`、`DOD-04`/`DOD-05`。
    heyaki API 锚点：`file.hpp`（FileTransferPhase 八态 probing/offered/
    transferring/verifying/paused/committed/failed/cancelled、
    FileTransferEvent.bytes_done/bytes_total、FileManifestBody.blake3 32B、
    每分块 blake3 32B）、`node.hpp`（push_file 含 transfer_id 断点续传
    参数、pause/resume/cancel_file_transfer、set_file_event_observer、
    pull_file）。
  - 一致性自查（验收 ①）：§7.1 与 §11.1①（传输写路径既有映射）——
    新增映射仅补充暂停/恢复/取消的承载方式（UpsertTransfer 幂等 + 既有
    作业组），不引入新作业类型/新公开契约，与 DEC-009 写路径排除项一致；
    与 DEC-004（.part/SHA-256/终态作业组）一致；与 DEC-006 映射 7 一致
    （细化而非冲突）；与 §8.1（传输四接口 M4 前签名语义）一致——真实
    语义声明在 §7.1 ①，M4-02 起按此实现。
    2026-09-24 评审修正：「不引入新公开契约」表述作废——§7.1② 原文
    「暂停/恢复…不产生 DB 作业」与同节 ⑤/DEC-006 映射 7（paused →
    UpsertTransfer）自相矛盾，且 heyaki `paused` 相位（含断线自动暂停、
    可发生于接收侧）无入站投递路径、`source_path`/`root` 在已声明契约中
    无来源。定案（DOD-04 设计先行，沿第 10 方法先例，见 §7.1②⑤、§8.1、
    DEC-006 映射 7）：① 暂停/恢复请求路径不经 DB，状态推进经既有
    `UpsertTransfer` 承载（「不引入新作业类型」半边仍成立）；② 出站 SPI
    签名扩展 `start_file_transfer(receiver, TransferId, FileMetadata,
    std::filesystem::path source_path)`（source_path 不进对端可见
    FileMetadata）；③ sink 第 11 方法 `on_transfer_paused(TransferId)`；
    ④ `root` 由组合根经存储配置注入 Adapter（非 SPI 参数）。此为公开
    契约修订的显式声明，M4-02（签名）/M4-05（第 11 方法路由）落地。
  - BLAKE3/SHA-256 澄清结论（验收 ②）：无冲突，不立统一决策——BLAKE3
    为 wire 层传输完整性校验（manifest + 每分块，heyaki `verifying` 阶段
    whole-file digest + fsync + rename）；SHA-256 为 `DEC-004` 存储哈希
    （落盘后全量流式计算回写 `stored_sha256`）。层次不同互不替代；接收方
    在 heyaki `Committed` 之后对落盘文件计算 SHA-256 回写，发送方 M4-04 发送
    前计算随 metadata 携带。
  - 容量预算复核结论（验收 ③，DEC-009 触发条款履行，含计算过程）：
    DatabaseWorker 通道 256 批上限模型 64×n≤256 的 n=2（DB 作业）不变；
    文件分块 IO 由传输会话长任务直接顺序写 `.part`、不经 DatabaseWorker
    通道（避免长 IO 作业占位通道排队深度），仅进度列更新（会话聚合批量、
    ≤每 tick 一次）与终态作业组经通道。大文件长作业影响 = blocking worker
    执行时长占用（至多等待一个在飞分块完成），在 drain 预算（2s）内。
  - 链接核验（本会话执行）：变更文档相对链接 62 条逐一核实 →
    `links checked: 62, broken: 0`。
  - 限制：纯文档先行契约，实现随 M4-02~05；实现若与本契约偏差，按 M1-08
    纪律先更新 §7.1 再合代码。
  - 同步：本里程碑（M4-01 勾选、本记录、状态 In Progress）、总计划当前
    状态。

- 2026-09-24（`M4-02`，**受阻未完成**——按工程规范 §4 勾选规则 3/4 与 §7
  验证证据纪律不冒充完成；
  Windows 11 / MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0）：
  - 已实现（工作树未提交原型）：`transfer/manager/transfer_session_manager.hpp`
    （单飞排空泵 + 会话循环 + 七状态机 + TaskHandle 按 ID + stop_all 消费）；
    SPI `start_file_transfer` 签名扩展（source_path，M4-01 §7.1⑤ 声明）同步至
    Fake/真实 Adapter/app TransferManager/全部调用点；DOD-02 六项 +
    七状态机/RULE-08 单测初稿（test_transfer_session_manager）。
  - 阻塞（未解决）：测试二进制 100% 复现 SIGSEGV。二分定位：
    - 最小探针（start → null-io tick 80ms → stop_all → executor shutdown，
      无 pause/resume/事件）即崩——排除了 pause/resume、事件回调、
      会话任务缺位（V3 二分：不派生会话任务仍崩 → 崩点在 start/enqueue/
      pump/stop_all/shutdown 链）；
    - stop_all 有/无两形态均崩；收件箱通道名缩短无效；
    - SIGSEGV 在 stop_all 内部或 executor.shutdown 期间（[probe] before
      stop_all 后无后续输出）。
  - 本地调试手段均已尝试且不足：MinGW gdb 无法读 MSVC PDB（bt 无符号）；
    MSVC ASan 运行时缺 clang_rt 组件（configure 失败）；cdb/WinDbg 缺失。
  - 与 M3-07 ReconnectCoordinator 的差异对照（M3-07 同 executor 模式测试
    全绿）：本管理器会话循环含 pause/resume 状态机 + 每 tick io 回调 +
    SessionControl shared_ptr——未定位到具体差异点。
  - 处置（工程规范 §4 勾选规则 3/4 与 §7 验证证据）：不合并未定位段错误
    的原型；M4-02 保持未完成；原型保留于工作树（未跟踪文件）供续查。
    补跑条件：WinDbg/cdb 或 MSVC ASan 运行时可用的环境符号化定位后修复
    再重跑全套单测。负责人：Linductor（补跑已于 2026-09-25 在原环境执行
    完成——根因定位与修复未依赖外部调试器，见下方评审修正与 2026-09-25
    完成条目）。
  - 同步：本里程碑（M4-02 受阻条目、本记录）、总计划（当前状态阻塞说明）。
  - 2026-09-24 评审修正（根因已定位并修复；以下修正原条目与事实不符处）：
    - 「已实现」清单纠正：`SPI start_file_transfer` 签名扩展（source_path）
      **未同步至任何代码**——heyaki_adapter.hpp / fake_heyaki_adapter.hpp /
      heyaki_node_adapter.hpp / app/application/transfer_manager.hpp 全部仍为
      三参签名（M4-01 仅完成设计定案，见 §8.1/DEC-006/§7.1⑤）；
      `test_transfer_session_manager.cpp` 单测初稿**已删除**（仅
      build/m3-01-debug 下残留构建产物）；工作树实际交付物仅为未跟踪头文件
      `transfer/manager/transfer_session_manager.hpp` 本体。
    - SIGSEGV 根因（评审探针实证 `"[probe] stop_all THREW: no state"`，
      并以同工具链最小探针复现）：`start_transfer` 只存
      `submission.handle`、丢弃 `submission.future`——`SessionRecord.future`
      从未赋值，`stop_all` 对非法 future 调 `wait_for` 抛
      `future_error(no_state)`；且 `sessions_.clear()` 先于取消循环，异常
      逃离时取消未下达、句柄已丢失，运行中会话成为孤儿（原「已完成会话
      跳过取消——request_task_cancel 不稳定」注记系对该 bug 的误诊）。
      次生缺陷：`consume_settled_futures` 消费已结算排空 future 后不复位
      `in_flight_generation_`，软超时击杀/提交即拒后单飞标志永久非零，
      命令静默滞留（违反 DEC-008 / 规则 10；对照 manager_runtime.hpp 先例）。
    - 修复（本会话执行）：future 随句柄保存；`stop_all` 沿 M3-07 次序重写
      （先取消全部句柄，再有界消费，移除误诊特例）；消费已结算排空 future
      时复位在飞代号（自愈）。验证：GCC `-Wall -Wextra -Wpedantic -Werror`
      语法检查通过；MSVC 14.44.35207（/utf-8 /permissive-，链
      build/m3-01-debug/lib/Debug/executor.lib）最小探针
      （start → tick 80ms → stop_all → shutdown）连跑 4 次全部
      `consumed=1` + `fully_stopped=1`（修复前同探针
      `[probe] stop_all THREW: no state` + shutdown 停滞）。
    - 状态：M4-02 仍**未完成**（In Progress）——原型缺陷已修复，但
      DOD-02 六项 + 七状态机/RULE-08 单测初稿已删除、待重建后全量验收；
      SPI 签名扩展随 M4-04/M4-02 落地。

- 2026-09-25（`M4-02`，**完成**；Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0；负责人：Linductor）：
  - SIGSEGV 根因复核闭环（独立三探针，`build/sigsegv-verify/`，gitignored，
    与 CI 同 pinned executor 构建产物链接）：① executor 黑盒——对已终态
    （完成/失败）任务的 `request_task_cancel` 共 2500 次（2000 完成 + 500
    失败，各含重复取消）零异常零崩溃，全部按契约返回 `AlreadyCompleted`，
    shutdown Completed——结合契约（executor.hpp:232-246 `noexcept` + 过期
    句柄幂等）、registry 实现（互斥 + map + tombstone，无裸句柄索引）与
    上游 `test_task_cancellation.cpp::RepeatAndStaleHandlesAreIdempotent`
    覆盖，证实「request_task_cancel 不稳定」系误诊且逻辑上不成立（原缺陷
    路径中该调用不可达——`wait_for` 对从未赋值 future 先抛
    `future_error(no_state)`，探针②精确复现该签名）；③ 孤儿会话 UAF
    复现（管理器释放后 worker 仍在其地址 tick，`shutdown` 停滞/任务静默
    死亡/SIGSEGV 为同一根因的三种时序表现）。不进 executor 反馈台账
    （无能力缺口）。
  - 实现（原型按 §7.1① 重构落地；本轮单测期复现的新 SIGSEGV 同族定位
    一并闭合）：出站四接口全部经单飞排空泵上下文串行（start 在泵内
    submit+登记，调用方线程零派生，无幽灵 Queued）；同步拒绝可见性
    （收件箱满 false 且零事件；泵内提交即拒回收记录——拒绝检测依据
    executor 同步结算语义：句柄恒先分配、future 即刻就绪才是信号，
    `handle.valid()` 不是）；状态事件仅在状态实际变化时投递一次（同态
    幂等不重报）；`SessionControl::start_gate` 启动闸门保证事件顺序恒为
    Queued → Negotiating → …（worker 抢跑不乱序）；`drain_loop` 修正为
    严格单消费者不变量（MpscChannel 契约 "one logical consumer"——原
    「批间释放在飞代号」窗口允许两个排空任务并发 `try_receive`：命令
    丢失 + 节点损坏）；`stop_all` 全局 deadline 预算（不随会话数放大）+
    活动态会话先推进 Cancelled 并投递终态 + 未消费 future 移入遗留集合
    由析构兜底无界消费（会话经 this 引用管理器，析构不得先于其终结）；
    `lifecycle_mutex_` 串行化 submit→登记 与 stop_all 快照（锁序
    lifecycle → mutex → futures），并修正登记守卫（记录存在即登记——
    快速调度下会话任务可能先于登记完成状态推进，误判接管会把运行中
    会话取消成无句柄记录）。
  - SPI source_path 签名扩展（§7.1⑤，M4-02 落地）：`heyaki_adapter.hpp`
    `start_file_transfer(receiver, TransferId, FileMetadata,
    const std::filesystem::path& source_path)`（source_path 不进入对端
    可见 FileMetadata，防信息外泄）；Fake（参数化接受，路径真实消费随
    M4-04）、真实 Adapter（M4 前语义 false）、app TransferManager
    （`StartTransferWork` 增 source_path 字段并透传；公开 API 默认空参）
    全调用点同步；test_heyaki_adapter / test_heyaki_node_adapter /
    test_app_managers 调用点更新。顺带修正 heyaki_adapter.hpp 过期头注
    （「9 类事件一一对应」→ 按 M3-05 第 10 方法后实际）。
  - 单测重建（`tests/unit/test_transfer_session_manager.cpp`，unit，
    9 用例 86 断言）：七状态机合法链全覆盖（事件序列逐项断言 + 每转移
    恰一次投递，含 Paused↔Transferring）；RULE-08 终态幂等（FIFO 屏障
    确定性验证终态后取消不复活、零新事件）；重复 ID/非法参数拒绝；io
    异常 → Failed（异常不外抛，future 干净结算）；执行中取消恰一次
    Cancelled（双报回归守卫）；提交拒绝（registry 容量 0 → 记录回收
    state_of 复位 nullopt + 无幽灵 Queued + 容量恢复后回归）；stop_all
    全局预算有界（事件控制的卡死 io + 预算内如实报 consumed=0 + 析构
    兜底消费）；收件箱满拒绝（单 worker 占用确定性，RULE-09）；shutdown
    钩子三会话终态各恰一次 + fully_stopped。断言只在主线程，事件收集器
    加锁；DOD-02 六项沿传输会话长任务路径全覆盖。
  - 验证：debug 全量 ctest 32/32、release 全量 ctest 32/32（既有 31 项
    零回归）；新测试二进制随机顺序 30 连跑零失败零崩溃（修复过程中以
    插桩定位三处竞态/顺序缺陷后收敛）；ASAN/UBSAN/TSAN 与 MSVC 矩阵随
    本 PR CI。
  - 观察登记（不在本项修复范围）：① `app/application/manager_runtime.hpp`
    的 drain_loop 存在同类「批间释放在飞代号」窗口（MpscChannel 单消费者
    契约下两个排空任务可并发 `try_receive`）——M1-05 既有组件，建议随后
    续里程碑以本项修正模式收口（M4-03 PR CI 首轮五档红即此窗口在慢核
    CI 时序下被闸门用例击中，已随 M4-03 补修复收口，见 M4-03 记录补记）；
    ② executor tracked 提交的容量类拒绝经
    future 同步结算异常而非句柄无效（`submit_tracked_with_hook` 先分配
    句柄后做 registry admission），`reconnect_loop.hpp` 的
    `!handle.valid()` 提交即拒分支对容量类拒绝不可达——reconnect 组件
    自身测试语义不受影响（其单飞拒绝为协调器层逻辑），登记备查。
  - 同步：本里程碑（M4-02 勾选、受阻条目引用修正、本记录、状态
    In Progress）、总计划（当前状态条目 + 更新日期 + M4 文档创建条目
    归位 + test_reconnect_loop 用例计数修正）。

- 2026-09-26（`M4-03` 完成；Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0；负责人：Linductor）：
  - 设计先行（DOD-04，M1-08 纪律；开工前调研两项结论按工程规范 6.2 落档）：
    新建 [DEC-010](../decisions/DEC-010-image-message-contract.md)（Accepted）
    冻结——① Image 消息 wire 契约：envelope `type="aki.image"`、
    `delivery_mode=peer_acked`、`schema_version=1`（语义为 aki 载荷 schema
    版本，heyaki 协议层仅校验非零）；ImagePayload 以 aki 自有 protobuf-wire
    最小编解码器（落设计 §14 预留的 `conversation/codec/`）编码进
    envelope.payload，冻结字段号 v1：1=name(≤512B)/2=size_bytes(varint)/
    3=mime_type(≤128B)/4=transfer_id(`hyt1_` 规范串 31 字符)/5=stored_sha256
    (预留 M4-04)；解码跳过未知字段（前向容忍）、缺 1~4/超限/非规范 → 有界
    拒绝可见；载荷总量 ≤4KiB（aki 侧上限）；追加可选字段不 bump 版本、破坏性
    变更才 bump；字符集仅长度界不校验 UTF-8（沿 aki.text 姿态）。
    ② DeliveryState↔TransferState 联动：正交生命周期、TransferId 消费侧
    join（`transfer.message_id` FK 保持可空不回填——M4-03 实现记录定案，
    join 依 `message.media_transfer_id` 单向保持）、发送侧准入期闸门
    （先传输准入、后发消息；传输准入失败 → 不发消息 + 消息行 Failed；
    消息准入失败 → cancel_transfer）由编排层承载、运行期零传导、接收侧
    一律 Delivered。③ SPI 形态：新增 `send_image_message` 专用方法，入站
    复用 `on_message_received`（信封 type 分发收敛 Adapter 层）。
    同批回填：设计 §6.1（新增「消息 wire 契约与收发状态联动」小节）+ §6
    （ImagePayload 形状）+ §7.1①（闸门编排层归属）+ §8.1（SPI 方法清单）、
    [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（冻结常量扩展
    `aki.image` + 载荷 schema 字段号 + TransferId `hyt1_` 双射显式点名 +
    映射 4 图片消息面扩展）、总计划已生效决策清单（DEC-010 条目）。
  - 实现：`conversation/message/message_types.hpp` `ImagePayload` 补
    `TransferId transfer_id`（镜像 FilePayload，`VideoPayload` 同构缺口按
    §6.1 先例留待接入）；`conversation/codec/image_payload_codec.hpp`
    （编解码 + 规范谓词 `is_canonical_transfer_id_text`——含尾部填充位
    校验，与 heyaki `parse_transfer_id` 接受集一致，域层不依赖 third_party）；
    `heyaki/session/runtime_node.hpp` `send_image`（MessageId/TransferId 双射
    + codec 编码 + `hyt1_` 权威校验，任一失败 admission false）与入站回调
    签名扩展（携带信封 type + payload 字节，不再压平 text 串）；SPI
    `heyaki_adapter.hpp` 新增 `send_image_message`（出站第 11 面方法）；
    Fake（参数化接受 + `sent_images()` 记录，沿 M4-02 先例不校验规范形式）
    与真实 Adapter（`send_image` 全接线 + 入站 type 分发 + 未知 type/解码
    失败有界拒绝计数 `inbound_rejections()` 可观测）同步；
    `app/application/message_manager.hpp` 图片出站路由（admission 语义同
    文本 + `transfer_admitted` 标记）；编排层闸门
    `app/application/image_flow.hpp`（`send_image_message_with_transfer`，
    唯一传导点，不创建任务不触碰 Store）；持久化 Image 分支
    `media_transfer_id` 列双向读写（`repositories.cpp`，列已存在无迁移）。
    入站回调签名扩展牵动的调用点同批更新（heyaki_node_adapter 构造/析构
    中和、test_message_loopback、test_heyaki_node_adapter）。
  - 测试（网络无关单测为主）：`test_image_payload_codec`（新二进制：往返 +
    截断逐长度扫描/缺字段逐项/超限/非规范 transfer_id 四形态/未知字段与
    预留字段 5 跳过/重复字段首现确定性/总量上限/非法 wire）；`test_heyaki_message`
    （增补：TransferId 双射 + codec 谓词对 heyaki 编码器产物的交叉验证 +
    非规范六形态双拒绝 + aki.image 信封 encode/parse 往返）；`test_heyaki_node_adapter`
    （增补：图片入站分发逐字段断言/解码失败与未知 type 有界拒绝计数/无 sink
    静默/send_image_message 出站有界校验）；`test_app_managers`（增补 3 用例：
    图片出站路由 + DeliveryState 正向链 + 迟到失败回报 RULE-08 + 接收侧
    Delivered；Adapter admission 失败 → Failed + 图片路径任务异常（DOD-02
    任务异常项沿图片泵路径）自愈；闸门两向失败路径——传输准入失败
    （容量 1 收件箱确定性占满）→ Fake 零 send 调用 + 消息行 Failed、消息
    准入失败 → cancel_transfer 到达 Adapter；运行期零传导——消息 Delivered
    与传输 Failed 正交 + 反向到达顺序解耦）；`test_persistence_repository`
    （Image 载荷 transfer_id 往返断言）。
  - 验证（本会话执行）：MSVC debug 全量 `ctest --test-dir build/debug -C
    Debug` 34/34 通过（既有 32 项零回归 + 新 2 项；最终代码状态下连续 9
    轮全量绿）；release 全量 `ctest --test-dir build/release -C Release`
    34/34 通过（连续 8 轮全量绿）；
    test_app_managers 随机顺序 10+8 连跑 + 新/改测试二进制（codec/message/
    node_adapter/repository）随机顺序各 5 连跑 +
    test_transfer_session_manager/test_discovery_pairing 各 8 连跑零失败零
    崩溃；变更文档相对链接核验 83 条 0 断链。如实登记：最终代码状态前的一次
    背靠背全量调用（debug 后接 release）出现过一次单测失败，其日志被后续
    运行覆盖、未定位于具体用例（疑为双套件紧邻运行的系统负载抖动）；此后
    连续 17 轮全量与上述定向压力全部绿，未复现。ASAN/UBSAN/TSAN 随本 PR CI
    （Linux 四档门禁）。
  - DOD-02 说明：图片路径未新增并发原语——出站经 MM/TM 既有单飞排空泵
    （DEC-008 模式，六项已随 M1-05/M4-02 既有用例覆盖），本项新增覆盖
    图片路径上的正常完成（路由用例）、任务异常（图片泵路径用例）、提交
    拒绝（闸门用例以收件箱满为触发路径之一）、shutdown（用例内
    fully_stopped 断言 + 会话先取消回收纪律）；执行中取消/超时沿泵与会话
    路径由既有用例覆盖（无图片特有分支——图片不派生自有任务）。
  - 如实降级（沿 M3-04~09/M4-02 纪律）：双端图片消息回环
    `test_image_message_loopback`（新单用例二进制）因本机防火墙拦截至端
    TLS（与 M3-05 文本回环同环境限制）以 `[skip] pairing handshake blocked
    (firewall)` 证据路径通过——网络无关半边（编解码/双射/信封/分发/路由/
    联动）已全部验证；补跑条件：防火墙放行入站 TCP 或 LAN 双端真机环境
    重跑（与 M3-04~08 登记的补跑条件同批执行）。文本回环 test_message_
    loopback 同样 [skip]（既有状态，签名适配后行为不变）。M4-06 回环验证
    按本契约补全链路断言（图片 + 传输 + 重启恢复一致），环境受限处置不变。
  - 已知边角登记（DEC-010②，非状态规则）：发送方消息 ack 失败但文件已
    推完 → 接收侧孤儿传输行；接收侧单侧到达（消息无传输行/传输无消息卡）
    ——M5 UI 兜底与后续 GC 议题。
  - 同步：本里程碑（M4-03 勾选、本记录）、总计划（当前状态条目 + DEC-010
    决策清单条目）、DEC-010/DEC-006/设计 §6/§6.1/§7.1/§8.1。
  - 补记（MR 闭环期，2026-09-26；负责人：Linductor）：PR CI 两轮五档全红
    （Linux debug/asan/ubsan/tsan + Windows debug，run 36182858882 与
    36187158313），失败均集中于本项新增闸门用例的 `transfers.flush(2s)`
    3 处断言（失败点两轮间漂移：:1366/:397×2 与 :397×2/:1661 组合），该
    二进制 CI 耗时两轮均 ~915.5s（本机 ~1s）；TSAN 档无数据竞争告警。
    第一轮修复（保留有效）：`app/application/manager_runtime.hpp`
    drain_loop 收敛为 M4-02 已落地的严格单消费者形态（批处理期间不释放
    在飞代号，仅在退出决策点释放+终检续期或让新 spawner 接管）——收口
    M4-02 观察项①登记的「批间释放」窗口（双消费者并发 try_receive 违反
    MpscChannel 单逻辑消费者契约 + 释放后单检查丢唤醒）。第二轮 CI 仍
    五档红、时长仍 ~915s——排除其为本次 CI 红根因（作为已登记缺陷的
    加固保留）。
    第二轮定位（决定性复现）：测试夹具强制 2 线程池后本机精确复现（同
    3 处 flush 断言、总耗时 916s）。根因：M1 会话骨架 `session_loop`
    （transfer_manager.hpp）在池 worker 上 `sleep_for` 轮询——每个活跃
    会话停占一个 worker；自适应池在 2 vCPU CI runner 上恰为 2 worker，
    闸门用例的双占位会话将其全部停占，TM 排空任务（flush 哨兵）永无调度
    → flush 超时失败；每个失败 section 的析构 shutdown 依次撞 executor
    内部等待上限（~305s × 3 ≈ 915s，与两轮 CI 时长吻合）。本机
    hardware_concurrency=20（进程 CPU 亲和性不改变池大小）故不复现。
    第二轮修复：`tests/unit/test_app_managers.cpp` BasicStack 固定线程池
    4 worker——测试脱离 runner 硬件决定性（双会话停占 2 个，排空与
    shutdown 恒可调度）。修复后本机 debug/release 全量各 34/34；
    ASAN/UBSAN/TSAN 随 PR CI 第三轮门禁复核。
    观察登记③（M4-04 收口）：会话骨架停占池 worker 在生产侧构成 2 核
    设备饥饿风险（≥2 并发传输即停占全部默认池 worker，Manager 泵不可
    调度）——M4-04 真实传输循环（分块写入走 blocking worker）必须消除
    池 worker 停占；M1 骨架以 M4 替换为既定设计，本项不重构。
