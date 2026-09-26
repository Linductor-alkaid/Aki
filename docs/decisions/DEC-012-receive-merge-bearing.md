# DEC-012：接收侧合并承载与 wire 事件路由形态

> 状态：Accepted
> 日期：2026-09-26
> 负责人：Linductor
> 冻结里程碑：M4（`M4-05` 实现前冻结；本记录即冻结，调研依据见文末）
> 替代/被替代：无（对 [DEC-011](DEC-011-transfer-io-bearing.md) 的发送侧承载
> 作接收侧补充；不改变 DEC-004 作业组语义与 DEC-009 容量模型）

## 背景与问题

M4-05 交付接收侧与暂停/恢复/取消（`SCOPE-08`），开工前必须定案三件事：

1. **接收侧合并的承载**：发送侧归档走专用 `aki.transfer-io` worker
   （DEC-011），接收侧 `Committed` 后「heyaki 接收根文件 → DEC-004 存储布局」
   的合并（流式 SHA-256 + 就位 + `stored_*` 回写）由谁执行、经哪条通道。
2. **接收侧是否需要会话/闸门机制**：发送侧因 Aki 归档滞后 wire 而有终态
   闸门与 SendSession；接收侧是否有对应相位差。
3. **`stored_sha256` 对账策略**（DEC-010 字段 5）：合并作业持有发送方哈希
   与实算哈希时失配如何处置（DEC-011 已定 `stored_sha256` 为 wire+内存字段，
   消息行无持久面）。

## 决策

**① 接收侧合并不上 `aki.transfer-io`、不新增 worker**：扩展现有 M2-06
「Completed 终态作业组」（`FileStore::complete_transfer`）的**供源**——
`.part` 缺失时回退到 heyaki 接收根文件（`<接收根目录>/<logical_name 段>`，
段拼接前有界校验：拒绝绝对路径/`..`，镜像 `valid_transfer_id` 纪律），同一
作业内完成流式 SHA-256 + 就位到 `files/<transfer_id>/<净化名>` + TRANSFER
终态与 `stored_*` 回写；接收源就位后**在同作业内删除原件**（保持
CompleteTransfer(Completed) = 1 作业，删除不拆分）；同卷优先 rename、跨卷
拷贝+删原件（实际实现统一走「hash+copy 到 `.tmp` → 原子 rename → 删源」
形态，与既有 `.part` 路径同构）。整体仍经 DatabaseWorker 通道（DEC-011 既有
结论维持：终态作业组经 DB 通道）。

**② 接收侧无 Aki 侧归档相位、无终态闸门、无接收会话**：heyaki `committed`
为终态，事件到达时文件已是接收根下完整落盘文件（BLAKE3 verify + fsync +
rename 已完成，file.hpp verifying/committed 注释）；Aki 侧无「归档滞后 wire」
的相位差——TM 现有「无 SendSession 的终态直达 `deliver_terminal`」即为正确
形态。接收行建行由入站路由首个状态事件承担（`UpsertTransfer`：
`file.name`=wire `logical_name`、`size`=`bytes_total`、sender=peer、
receiver=local）；`UpdateTransferProgress` 不建行、`complete_transfer` 对
缺行明确失败（既有契约不变）。

**③ `.part` 幂等删除维持现状**：DatabaseWorker discard 作业（接收侧无
`.part` 时幂等 no-op）；发送侧 `aki.transfer-io` cancel 作业的即时清理不变。
heyaki 失败/取消自清其接收侧残留；接收根残留靠作业幂等重跑收敛（启动清扫
仍仅覆盖 `files/tmp`，接收根长期 GC 归 §6.1 已登记的 M5 兜底/GC 议题，
本决策不扩清扫面）。

**④ wire 事件路由形态（DEC-006 映射 7 的 M4-05 细化；2026-09-26 评审修正
——冻结前提与 pinned 源不符，按源纠正）**：`set_file_event_observer` →
Adapter 入站分发（EXEC-02 有界校验 + 投递）按 FileTransferPhase 八态映射
——`probing`/`offered` 为**发送端专属事件**（file_service.cpp 仅发送侧发出
:210/:252/:604）→ `on_transfer_started`（发送行状态推进——行由
StartTransferWork 先建、TM 以泵内已知行缓存合并；行缺失时补建
`sender=local` 行）；**接收端首个事件是 `transferring`**（:1147/:1368，
接收侧无 probing/offered）→ 未见 started 的 id 由首个 `transferring`/
`verifying` 承担**接收行建行**（`on_transfer_started`，Negotiating 行：
sender=peer/receiver=local，`file.name`=**剥根段** wire `logical_name`——
wire manifest `logical_name` = join(root, name)（:556），heyaki 落盘为剥根段
相对名（:1075）而事件携带含根前缀（:1147/:1368/:1441），行名须与供源推导
（`receive_source_path`）一致；Adapter 以有界记名簿去重、容量满计数可见）
+ `on_transfer_progress`（TM 侧首个进度事件同时把行推进到
Transferring——`Negotiating → Transferring` 经整行 upsert 承载，后续经
`UpdateTransferProgress`）；`paused` → sink 第 11 方法
`on_transfer_paused(TransferId)`（§8.1 冻结签名；UpsertTransfer(Paused)，
不新增 AppEvent 主路径类型，状态经 Store 快照可见——对端驱动（含断线自动
暂停）与本地暂停确认同此映射）；`committed`/`failed`/`cancelled` →
`on_transfer_completed(final_state)`（既有路径；终态事件不建行——行缺失由
owner 拒绝可见，已知边角）。判向**不经 `direction`**（pinned 源定论，风险④
成立）：接收行仅由入站首个 `transferring`/`verifying` 建行（恒
sender=peer/receiver=local），`probing`/`offered` 恒发送行；行数据由 TM 的
泵内已知行缓存承载（StartTransferWork 与入站 started 建档，progress/paused
更新，终态清理）。

**⑤ `stored_sha256` 对账策略（DEC-010 字段 5 / DEC-011 风险登记项定案）**：
合并作业**不做**收发哈希对账——发送方哈希仅存在于 wire 消息载荷与内存态
（DEC-011：消息表无对应列），DB 作业无可比对的持久面；对账由**持有消息载荷
的消费者**执行（M4-06 回环断言 `消息 media.stored_sha256 == 传输行
stored_sha256`、M5 UI 展示层校验），失配即断言失败/可见呈现，不静默、不在
作业内伪造通过。若后续需要持久对账，须先立 schema 变更（消息侧哈希列），
不在本决策内。

**⑥ 配对申请 scope 扩展（DEC-006 映射 3 的 M4-05 增补）**：文件推送需
独立 live scope `file.push:<root>`（heyaki `file_push_scope(root)`，api.md
scope 语法——`message.send` 前缀通配不覆盖跨分支）；Aki 配对申请自 M4-05
起为 `{message.send, file.push:<root>}`（消息面 M3 冻结项不变；接收方向无
额外 scope 需求——由发送方持有）。`NodeSession::pair_peer` 增补可选 scopes
参数（缺省保持 M3 形态）。

**DEC-009 复核（触发条款履行）**：单更新作业数上限 n=2 不变
（Completed=1——接收合并折叠进既有 complete 作业；Failed/Cancelled=2——
终态列 + discard），64×2=128≤256 维持，§11.1① 无需改动；关闭序断言不变
（无新 worker，`blocking_workers == 2` 继续成立）。drain 预算：大合并
（≤`max_file_bytes` 配额，默认 1GiB）在飞时 DB 排空可超 2s/3s 预算——
`drain_budget_exhausted` 如实可见 + 幂等重跑收敛（与已接受的发送侧终态组
同级且更受控，配额为旋钮）。

## 备选方案

（调研否决项摘要，全文见冻结调研记录）

- **合并作业上 `aki.transfer-io`**（新作业 kind + TM 接收侧终态闸门持有
  committed + DB 侧「带预计算哈希的纯回写」作业形态）——一个终态组裂成
  收/发两种形态（与 DEC-011② 收口方向相反）；预计算值须穿透 typed 更新
  载荷（§10.1/§11.1④ 波纹）；单飞设计服务于多分块流，单次合并用不上。
- **新增第三 blocking worker**——两个长 IO worker 一个闲置裕度；关闭断言
  无谓翻动；接收并发受配额（默认 2）约束无容量压力。
- **经既有 TransferIo 把接收文件拷成 `.part` 再走既有 complete 作业**——
  三遍 IO 对比供源参数化的单遍合并；复用发送形相位语义错位。
- **把 heyaki 接收根直接配成 Aki 最终布局目录**——不可行：heyaki 按发送方
  `logical_name`（可含多段）落位，Aki 布局是 `files/<transfer_id>/<净化名>`，
  且 DEC-004 明文远端原始文件名禁止拼入磁盘路径；多传输同名冲突。
- **合并拆成 DB 通道内两个作业**——为一个语义操作引入作业间顺序耦合，
  折叠进既有幂等组严格更简。
- **`.part` 删除迁到 transfer-io**——现状已闭合，迁移无收益。

## 影响与风险

- **供源回退次序**：`.part` → 接收根推导路径 → final 恢复分支 → 明确失败
  （实现与测试逐项覆盖）；作业侧对推导路径的有界校验为纵深防御（heyaki 侧
  `safe_logical_file_name` 已拦绝对/`..`）。
- **接收根配置**：组合根把接收根目录配置在数据根内（如
  `<data_root>/receive/<root>`，NodeSession Options 扩展
  `file_receive_roots`——现状 `{}` 必须扩展），使合并同卷可 rename；根逻辑名
  与 Adapter `push_root`（默认 `inbox`）对应。本决策按单接收根
  （`inbox`）参数化 complete 作业（多根不支持，事件 root 字段的通用路由
  留待出现第二根时扩展）。
- **崩溃窗口**：heyaki committed 与 complete 作业之间——行停留非终态 +
  接收根文件保留，重发 `CompleteTransfer(Completed)` 幂等收敛；孤儿活动行的
  重启再驱动未设计（发送侧同样存在），归 M4-06 重启一致性范围。
- **`direction` 取值已从 pinned 源定论**（2026-09-26 评审修正，原风险④
  成立）：接收侧 push 事件 `direction` 恒为 `push`（file_service.cpp
  :1147/:1368/:1441，`pull_initiated=false`）、接收侧 `cancelled` 恒为
  `pull`（:380）——判向不经 `direction`（见 ④ 修正）：接收行仅由入站首个
  `transferring`/`verifying` 建行（恒 sender=peer/receiver=local），
  `probing`/`offered` 恒发送行；TM 已知行的 started 处理以缓存行数据推进，
  Adapter 构造的 sender/receiver 不参与。
- **heyaki 断点续传簿记**：committed 为终态不应有 resume；从接收根移走已
  committed 文件不应影响簿记（heyaki 状态对最终路径的引用未在公开头核实——
  回环补跑时观察，异常即登记）。
- **pull_file 接收方向 M4 不接入**（接口预留），本决策仅覆盖 push 接收。
- 本调研为纯静态证据（未编译未运行）；动态验证归 M4-05 实现与 M4-06 回环。

## 验证方式

本记录依据 2026-09-26 冻结调研（负责人 Linductor；pinned heyaki
`file.hpp`/`node.hpp`/`api.md` 公开头与 aki 仓库 file_store/file_jobs/
transfer_manager/main.cpp 逐文件静态核实）。动态验证归 M4-05：网络无关单测
（八相位路由逐条、暂停/恢复语义、取消幂等删除、接收合并 + `stored_*` 回写
+ 重启一致、DOD-02 沿接收/控制路径）+ debug/release 全量 ctest 零回归 +
ASAN/UBSAN/TSAN 随 PR CI；回环沿 M3/M4-03/04 降级纪律。证据记录于
[M4 里程碑文档](../plans/m4-image-file-transfer.md) M4-05 验证记录。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)§7.1①②③④⑤（③ 接收侧承载/
  ④ 供源参数化修订落点）、§8.1（sink 第 11 方法）、§8.3（路由表扩展与
  接收路径形态）、§11.1①④（容量模型维持/作业组供源修订）
- [DEC-004](DEC-004-local-persistence-sqlite.md)（存储布局/终态作业组/
  discard）、[DEC-006](DEC-006-heyaki-api-contract.md)（映射 7 M4-05
  细化）、[DEC-008](DEC-008-manager-routing-and-executor-tasks.md)（路由
  与泵纪律）、[DEC-009](DEC-009-appstate-write-path.md)（容量复核触发条款
  ——n=2 维持）、[DEC-010](DEC-010-image-message-contract.md)（字段 5
  对账策略定案）、[DEC-011](DEC-011-transfer-io-bearing.md)（发送侧承载/
  stored_sha256 wire+内存口径——本决策为其接收侧对偶）
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-08`、
  `EXEC-02/04`、M4）
- [M4：图片消息与文件传输](../plans/m4-image-file-transfer.md)（`M4-05`
  实现、`M4-06` 回环与对账断言）
