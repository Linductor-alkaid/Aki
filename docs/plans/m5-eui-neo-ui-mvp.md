# M5：EUI-NEO UI 与 MVP 验收

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M2、M3、M4、`DEC-005`（2026-09-26 已冻结 `Accepted`）。M3/M4 工作
> 项（`M3-01`~`M3-09`、`M4-01`~`M4-07`）均已完成并经收口审计，两里程碑保持
> In Progress 待防火墙放行入站 TCP / LAN 双端真机环境补跑关闭（与 M3-09/
> M4-07 记录同批）——沿 M4 文档创建先例，环境补跑不阻塞本里程碑文档与
> 设计先行工作项（`M5-01`），UI 实现工作项开工时复核 M3/M4 状态（`M5-02`
> 开工前已复核：全部工作项完成、仅防火墙/LAN 双端补跑待关闭，不阻塞）；
> 真实 Adapter、NodeSession、发现/消息/图片/传输/presence/重连管道与恢复
> 语义均已就绪
> 建议发布点：v0.5.0（MVP）
> 更新日期：2026-10-01（Heyaki v1.1.1 接入与跨平台验收）

## M5-33~M5-35：Heyaki v1.1.1 接入

> 状态：In Progress；日期：2026-10-01；负责人：Linductor。
> 依据：DEC-003、DEC-022、DEC-023，HEY-20260929-001 与 HEY-20260930-001~004。

- [x] `M5-33` 独立依赖 MR 固定 Heyaki v1.1.1 `1ceb42c7b244e950ebeeb10edc84b6d83e423626`；
  更新 lock 和供应链审计，移除已修复回调竞争的精确 TSAN 抑制，适配
  有界密码授权 admission；验收 Debug/ASAN/UBSAN/TSAN、Windows 与打包 CI。
- [ ] `M5-34` Aki 组合根显式开启 basic_communication，Adapter 分离策略
  scope 与设备 grant；新身份无 grant 双向消息/图片/inbox 文件可用，单侧
  策略关闭时失败可见，控制权限不随基础通信开放。Linux/Windows 安装版
  复验文本、文件与交换重启顺序后完成。
- [ ] `M5-35` Adapter 接入无密码请求/允许/拒绝及稳定请求 ID，Manager
  经现有 Executor MpscChannel 推进 Application State，UI 消费入站请求；
  正反方向、拒绝、超时、重复/迟到、断连与 shutdown 均可观察。
  该项后续独立功能 MR，不将接收方签发 grant 误标为“对方已信任本机”。

2026-10-01 准入：用户告知上游更新。Heyaki #1~5 均 closed/completed，
公开 v1.1.1 release 提供密码 admission、已授权会话修复、免信任基础通信、
无密码批准与 vendor 回调同步补丁。上游关闭不代表 Aki 验收完成。继续
独立管理 Heyaki，仅消费发布版本与官方 fetch 脚本，不直接改上游源码。
原 Linux 目标机离线；现有 Windows 192.168.5.99 未安装 Aki、SSH 22 超时，
后续通过安装包下载测试。补跑负责人 Linductor/设备操作者；条件为两端
安装本次同版 CI 安装包并在同一 LAN 在线。协议能力在 Adapter 收口，
Runtime 使用现有 borrowed Executor，新增业务不创建线程或私有调度器。

2026-10-01 M5-33/34 本地验证：Debug 全量构建通过。首轮 ctest 43/44，
唯一失败是旧回环把“已完成后再次提交”视为应拒绝；v1.1.1 明确允许续期。
改为接纳续期、一次新成功结果且授权会话保持，目标重跑 53 assertions
通过（7.25s，非 skip）。新单 owner、fresh profile 基础通信回环 3 cases /
110 assertions 通过（41.03s），覆盖双向 text/image/file 信封、inbox 本体
哈希、零 grant、退出/重开后重连、错误口令不关闭授权会话、反向 grant、
单侧消息 30s TTL 失败及文件无落盘/取消 admission/shutdown。
ASAN 基础回环与 Adapter 单测 2/2 通过（46.94s，非 skip），无内存报告。
首轮旧回环中的 message、disconnect recovery、real adapter、full/send
transfer 仍 skip，不能把 43 个返回成功项全部称为网络链路验收。
本地 TSAN 编译通过，运行在测试开始前因 unexpected memory mapping
失败（0.02s），未执行回归、未记通过；补跑负责人 Linductor，条件为
CI TSAN runner 对最新 head 完成全量门禁。UBSAN/Windows/打包同由最新
head CI 验证，安装版仍待用户双端测试。

单侧文件策略拒绝缺有界终态另登记 HEY-20261001-001 / Heyaki #13，
保持 M5-34 此项未完成；新测试的取消 admission 与 shutdown 不替代
上游终态修复。两端新 Aki 都启用策略的通路已验证，尚未声称目标 Windows
安装或 GUI 通信验收。M5-35 不在本升级 MR 宣称实现。

2026-10-01 M5-33 CI 首轮 run 36857212819 / head dc09559：TSAN
43/44，失败位于旧发送回环 test_transfer_send_loopback。Aki 未先停止
LanDiscoveryPipeline 就 shutdown Node，定时回调 endpoints() 与 Node
释放竞争。这是 Aki 生命周期顺序缺陷，不归上游 #5、不新增抑制。
修复依据 EXEC-01/04、设计 8.3：先停发现生产者；同一互斥边界阻止
取消前已排队的 tick 再访问 Node，并按扫描代次拒绝 stop/start 前的旧 tick。
stop 只等待端点查询结束；已合成事件可能仍投递，调用方必须保留管道与
sink 至 Executor 排空。回环断言失败也须在观察者/Manager 存活时清理。
负责人 Linductor；定向 Debug/ASAN 与最新 head CI TSAN 通过后记录证据。
首轮其余六项 CI completed/success；本地 UBSAN 基础回环通过（47.54s）。



2026-10-01 修复后本地定向构建 Debug/ASAN 通过；Adapter 137 assertions /
12 cases 两档通过。旧发送回环两档均在握手前置门 skip（15.65s/16.05s），
未执行传输或停止顺序断言，不记为竞态修复验收。真实关闭链路及 TSAN
由修复后最新 head 的 CI 补跑；负责人 Linductor，条件为 CI runner 可用。

2026-10-01 同类路径审计：PeerSessionPipeline 在 stop 的互斥边界之外
读取 peer_session_views，取消残留也可与 Node shutdown 竞争。沿相同
EXEC-04 停止契约把查询和代次校验放进原有互斥边界，保持现有 Executor
timer 与事件投递；不新增调度器。LanNameBeacon 已在同一锁内检查关闭
socket，无 Node 查询。本修复覆盖 Host 的发现与连接状态两条生产者。


2026-10-01 连接状态停止修复后：Debug/ASAN 的 Adapter（137 assertions）、
PeerSession 纯映射（143 assertions）与真实连接状态回环（30 assertions）
三目标均通过（18.21s/19.59s，非 skip）。回环覆盖停止后零事件及随后
Node shutdown；ASAN 无内存报告。最新 head TSAN 仍须 CI 运行后确认。

2026-10-01 修复 head 9d0a7fe 的五项测试 CI 已成功，TSAN ctest 44/44。
旧回环 CI 使用 output-on-failure，成功项未输出 Catch 断言或 skip 内容，
不能只凭 Passed 宣称关闭分支实际执行。为补上该具体证据缺口，在新
无 skip 的基础回环中运行发现/连接状态观察、检查断开/重连事件，并在
保持管道与 sink 存活时 stop→Node shutdown→Executor 排空。此轮只追加
回归与证据，不改变安装包产品代码；最新 head CI 仍须完成后合并。


2026-10-01 产品代码 gate：head 9d0a7fe15d89bf697aa2f0de19ceb7a54a13873a，
CI run 36860880298 七项独立核实 completed/success（Linux Debug/ASAN/
UBSAN/TSAN、Windows Debug、deb、setup）。TSAN ctest 44/44；新基础
回环无 skip 出口，TSAN 66.19s。旧回环成功项输出不含断言/skip 细节，
不宣称它们全部执行了网络分支。补充观察管道无 skip 回归在本地 Debug
116 assertions / 3 cases（41.61s）、ASAN 115 assertions / 3 cases
（44.35s）通过；差一断言来自已登记单侧文件取消 admission 路径。
后续提交仅追加测试与证据，产品代码相同，须再经最新 head 七项门禁。
Linux CI 包 SHA256：58878a74fb2df636e37de7a78f5b50cf3e54624f12624741fe3002ed389d8b7c，
包内 aki SHA256：5291c24f87ae0a17c5732331ba0a427ead733f0db1d0a801390425425b4a7a89；
amd64/0.1.0、desktop-file-validate 通过。安装版跨平台验收仍待操作者。

## M5-36~M5-39：Linux / Windows 实测缺陷修复

> 状态：In Progress；日期：2026-10-01；负责人：Linductor。

- [ ] `M5-36` Ubuntu 24 中文输入恢复：定位窗口后端与输入法边界，兼容
  适配仅使用公开 GLFW API；验收中文切换/候选确认/退格/发送，Dock 分组
  保持，纯策略单测与 Linux 构建，真实桌面复验后完成。
- [ ] `M5-37` Windows 首次设备密码：核实实际数据根、profile 是否复用及
  初始化检查错误；新数据根必显示设置窗口，不设置不得启动 Node；既有
  有效密码不覆写，检查错误需可见；Windows 实测后完成。
- [ ] `M5-38` 图片默认预览：归档完成后通过 Application State 暴露本地
  文件记录，出站即时源与收发归档均可展示缩略图，点击放大；重启历史、
  消息/文件乱序、未完成/缺失文件都有明确表现。Linux/Windows 双向复验。
- [ ] `M5-39` 文件位置：先按传输文件保存位置处理，接收与发送归档路径
  显示在聊天/传输卡片，支持复制完整路径与打开所在目录；长路径、中文路径、未就位
  与错误反馈可用。若用户澄清为应用安装位置，再调整本项范围。

用户实测版本为 9d0a7fe（产品代码与最新门禁 fe8d071 相同）：Ubuntu 24
无法切换中文输入，Windows 未看到首次设置密码，双向接收无图片预览且
Ubuntu 出站也无预览，文件位置不可见。四项均记为未通过，不将协议回环
替代 GUI 验收。M5-34 保持未完成。输入法涉及 pinned EUI-NEO/GLFW 原生
Wayland 缺口，按 DEC-025 单一 Platform Adapter 兼容；媒体按 DEC-026
从既有归档记录派生，不把本地路径加入 wire 或 Core 消息。

M5-33 收口：最新 head fe8d071 的 run 36863633285 七项成功，PR #63
已 Squash 合入 master 4998b8239ae3265f00825ebec48ab0ec7879645c，工作
分支已清理，本机 master 已 fast-forward 同步。M5-35 保持后续独立阶段。

## M5-29~M5-30：历史会话恢复与基础通信权限

> 状态：Completed；负责人：Linductor；依据：DEC-008、
> [DEC-023](../decisions/DEC-023-basic-communication-policy.md)。

- [x] `M5-29` 启动恢复的 Conversation 参与连接/断开事件推进，保留原 ID、
  端点和历史；验收：恢复后重连/断开、重复 ensure、Archived 不复活单测，
  HostRuntime 实际播种接线，CI 与双设备聊天页状态复验。
- [x] `M5-30` 核对并向 Heyaki 提出免设备信任的基础消息/文件能力；
  验收：公开 API、通道与服务权限证据、反馈台账、上游 issue。
  状态 Completed（本项范围为能力核对与 issue）；实现接入待 Heyaki 独立提供策略 API 后另行验证，不以 Aki 放开 UI 代替。

2026-09-30：用户要求优先修复设备页在线而聊天页断开的状态不一致，
并将消息/文件通信与未来 shell 等设备控制的信任授权分开。已定位恢复
会话只播种 AppStateOwner、未播种 ConversationManager 的创建记录，导致
后续 connected/disconnected 被当成未建会话空操作。修复复用既有
ManagerPump，不新增线程、队列或调度器。实际绿点来自 presence，仍须
真实建链事件才推进 Active，不能把发现在线直接作为已连接。

本机 Debug 全量构建通过；`ctest --test-dir build/debug --verbose
--output-on-failure -j4` 43/43 无失败（37.18s）。其中断线恢复回环因
close_lan 后未收到 disconnect 事件而 skip，不能计为真实恢复验收；
GUI 双端状态复验由用户配合，负责人 Linductor，条件为两端新版启动、
实际路径建立后查看同一历史 Conversation。新增恢复回归 1 用例/77 断言
通过；最终 Manager 单测 19 用例/921 断言、宿主单测 4 用例/149 断言通过。
宿主真实 DB 夹具验证恢复记录参与连接/断开并保留原 ID。
上游基础通信需求已提交 Heyaki #4，反馈 HEY-20260930-003；不改 pinned
依赖。本机正常关闭/重启新版 Debug 后，原 1 条历史会话从 Disconnected 恢复
Active，原 6 条消息仍在；该证据不代替目标机 GUI 验收。
后续双端同版 GUI 复验已通过，见 M5-32 验收记录。
CI run 36671067469 / head 580d989：Debug、ASAN、UBSAN、Windows Debug 与
Ubuntu 20.04 deb 通过；TSAN 因上游回调重置竞争失败（42/43），记录为
HEY-20260930-004 / Heyaki #5。沿现有 vendor-only 纪律只增加精确符号
豁免后重跑，不把竞态宣称为已修复。首轮 deb 已下载并核对 archive digest，
包 SHA-256 为 e9436226a343840136e88091410a7152b124e2f74bc91f2732f0218cbea80c10，
目标机 SSH 密码窗口仍等待用户输入；尚未上传，未冒充交付/安装完成。

## M5-31：传输完成与暂停确认的竞态

> 状态：Completed；负责人：Linductor；依据：DEC-011/DEC-012、设计 §7.1。

- [x] `M5-31` wire committed 后完成本地归档并收敛 Completed，迟到暂停
  确认不能使完成闸门停摆；保留 Cancelled/Failed 终态不复活、未知行拒绝。
  验收：确定性暂停/恢复/迟到暂停/committed 回归、归档闸门与取消回归，
  Debug/ASAN 本地目标测试、CI 七项全绿及新版包交付。

2026-09-30：run 36672794896 / head c7d2f1c 仅 Linux ASAN 失败，其余六项
成功，TSAN 上游精确符号豁免生效。失败为 test_transfer_full_loopback:406
的 terminal_seen 断言（无 ASAN 内存报告）：对端 committed 且本体哈希通过，
本机 Paused、owner 拒绝 1 次、TM handler 拒绝 1 次。小文件的最后进度可能
早于暂停确认；resume 仅解除归档抑制，未必还有 wire progress 能推进状态。
明确 committed 是 wire 完成事实：归档不再等待网络 resume，持有终态时
迟到 paused 不阻止本地归档；归档完毕后经既有 Transferring 合法边结算。
不增加线程/调度器，不放宽 Cancelled/Failed 或未知行的状态校验。

确定性回归在旧代码上失败两处：held committed 后归档 advance=0、
归档完整但行仍 Paused；修复后覆盖 committed 早/晚于归档、迟到 pause
控制/确认、Negotiating 无最后进度、取消 held 完成及迟到 IO/wire 事件。
Debug 完整构建通过，全量 ctest 43/43 无失败（34.78s）；发送路径
11 用例/279 断言，Manager 19/921、接收路径 4/80、宿主 4/149。
ASAN 目标构建与四个测试目标无失败（19.86s），发送路径同样 11/279。
本机完整文件回环在 Debug/ASAN 均因握手限制 skip，Debug 断线恢复回环
也 skip，不计网络验收；本轮 CI 尚待新提交结果，负责人 Linductor。
后续七项 CI 与新版包交付已满足，见 M5-32 续验记录。

续验：CI run 36675887002 / head 78684a9ece0ee141d56e4adda412622a91fd7520
七项均 completed/success：Linux Debug、ASAN、UBSAN、TSAN、Windows
Debug、Ubuntu 20.04 deb、Windows setup。TSAN 仍包含前述上游符号豁免，
不表示 Heyaki #5 已解决。目标机新版包交付与聊天页复验尚未完成，
负责人 Linductor 与设备操作者；条件为两端新版运行并检查同一历史会话。

## M5-32：重连快照中的历史关闭记录

> 状态：Completed；负责人：Linductor；依据：DEC-006/DEC-022、设计 §8.1。

- [x] `M5-32` 当前连接不被同设备的历史 closed 会话覆盖；验收：
  不同快照顺序、restricted/authenticated 与旧 closed 共存、完整退出/
  重开事件序列和双设备重启复验，相关单测与 CI。

2026-09-30：用户手动安装并启动新 deb 后报告：后启动一端识别对端，
先启动一端的历史聊天仍等待恢复；交换重启顺序则症状交换。目标机
安装包 SHA-256 为 21eafb49a39185d19c039321ec64ebfd49f5ee1f7d367a8065a9364a708ba4bc，
运行二进制 SHA-256 为 3d72bd938b00d3d47d2ddf414e70a806e86b6e91d4b0eaf3edc4859235f59d93，
`dpkg -V aki` 无差异；包构建时间为 14:13。本轮自动下载超时，未由
Agent 上传，交付/安装来源为用户手动操作。
本机启动后 DB 为 Active、6 条历史消息，目标机仍 Disconnected、4 条
历史消息；目标正常退出后本机 DB 转为 Disconnected，底层活动会话归零。
已核对 pinned Heyaki `publish_peer_sessions()` 将当前 attempts 放在
前面、finished closed 历史追加在后；Aki diff 用 device ID 无条件覆盖，
同设备的历史关闭行会覆盖新会话。该问题在 Aki Adapter 中归并修正，
不改 pinned 依赖、不新建并发路径。目标机重开后的现场已复现：本机底层当前 attempt 为 authenticated、
PeerSession active，同时保留一条 finished closed；本机 DB 仍
Disconnected，目标机 DB 恢复 Active。修复验收待补。

修复在 diff 两端排除 closed 记录，再按 authenticated/restricted/握手态
选择设备代表；不修改 Node 生命周期或自建监控。新确定性测试在旧代码
2 个用例/27 条断言中失败 5 条，修复后 Debug 与 ASAN Adapter 单测
均 14 用例/143 断言通过；覆盖历史顺序、冷启动、重排、路径变化、
重复断开及同设备多个端点。Debug 全量构建、verbose ctest 43/43
无失败（41.96s），文件完整回环实际通过；断线恢复回环因 close_lan
后未收到断开事件 skip，不计该回环验收。本机已正常关闭/重启修复版，
用户已配合目标机再次退出/重开并确认本机自动恢复连接。本机 DB
也恢复 Active；原 6 条消息保留并新增 1 条消息。目标机仍运行本轮补丁
之前的包，需要同版更新后反向重启复验；新版 CI 与 deb 交付待完成。

续验：run 36681973343 / head b31b567634b770e89aaeb927fea020b536bec907
七项 CI completed/success。Ubuntu 20.04 artifact 11082685644 的 archive
SHA-256 为 6dd5559814c24e22ac9a2de86dd4b7cc4022cf35ca3a1032c4713def95a4c5ec，
deb 为 9e155a81cae145acaea75668e800dd75f418aea5b28d353cc5b794ca404354a9，
已上传 `/home/ybt/aki-0.1.0-b31b567-amd64.deb`，远端哈希与格式检查
通过。首轮 scp 文件哈希异常未计交付，改为 SSH 流式传输到 staging，
核对后原子替换。M5-31 的 CI/新版包交付退出条件满足，状态 Completed。
用户安装并重开目标机后，运行进程与安装文件均为 CI 二进制 SHA-256
5a2be316455c6ea0f3c655e2881d6f35e98d897f4c60b9b610c18adebe59ac89；
目标历史 Conversation 为 Active，原 4 条消息保留。反向重连 GUI 复验
已由用户确认：本机正常退出后目标显示断开、重开后恢复；两端 DB 均
为 Active。但本机重开后运行 `/opt/aki/aki` 的 2026-09-29 旧安装版
（SHA-256 7ade718f0d49a4e05fcc89dd1af0075263a48e4a165c7ea8e935bfeb9611aeee），
此前本机修复验证运行 Debug。为确保日常启动也使用修复版，已将同一
CI deb 放到本机 `下载/aki-0.1.0-b31b567-amd64.deb`，待用户安装/重开
并核对运行二进制；负责人 Linductor 与设备操作者。
上一轮手动安装包 21eafb49... 已完整核对与 run 36675887002 / head
78684a9 的 CI deb 哈希一致，不仅以构建时间推断。

本机安装/重开后实际运行程序也匹配同一 CI 二进制 SHA-256
5a2be316455c6ea0f3c655e2881d6f35e98d897f4c60b9b610c18adebe59ac89；
目标机仍运行同版，两个历史 Conversation 均 Active，本机 9 条、目标
4 条消息。用户已分别确认目标机重启后本机自动恢复，以及本机退出后
目标机显示中断、重开恢复。M5-29/M5-32 双端会话恢复验收 Completed。
该结果仅证明连接/会话推进；基础消息/文件的免信任策略仍受 Heyaki #4
阻塞，密码授权及无密码批准的已登记问题未由本项解决。

## M5-25~M5-28：连接与信任语义复验

> 状态：In Progress；负责人：Linductor；设计依据：
> [DEC-022](../decisions/DEC-022-link-before-trust-and-grant-direction.md)、
> [Aki 设计](../design/aki_design.md)及 [UI 规范](../design/aki_ui_design.md)。

- [ ] `M5-25` 在线设备可先进入受限连接，连接/断开与授权分别可观测；
  验收：两端无 grant 时连接路径出现，授权转换不重复连接事件，断线可恢复。
- [ ] `M5-26` 信任方向显示按 grant 签发方解释；验收：A 输入 B 口令后
  A 显示「对方已信任本机」，B 显示「本机已信任对方」，反向操作得到互信；
  双端文本、图片、普通文件与失败反馈实际验证。
- [x] `M5-27` Settings 修改本机密码弹窗位于整个窗口中央；验收：
  不同窗口尺寸、设置页滚动位置和中英文界面下弹窗中心与窗口中心一致。
- [ ] `M5-28` 对端直接点击“允许”建立授权（双方无需输入密码）；
  阻塞：pinned Heyaki 无接收方批准 API，证据与最小能力见
  [HEY-20260929-001](../heyaki_feedback/ledger.md)。负责人 Linductor；
  上游固定 API 并授权更新 pinned 依赖后接入和双端验证。

2026-09-30 M5-27 验收 Completed：两台设备均运行本轮 CI deb，用户
确认修改本设备密码弹窗在中英文界面、不同窗口尺寸下始终位于整个
窗口中央；缩小窗口使设置页可滚动后，再滚动并打开弹窗仍居中。
本次只观察布局，未改动设备口令。

2026-09-29：用户要求按连接→信任→通信顺序修正并在另一台设备上配合
验证，同时将更新 deb 交付 `ybt@192.168.1.206:~/`。本机单测与编译先行；
双端验收、包安装与 GUI 定位由用户配合实测，未执行前保持未完成。
用户随后明确“对方确认”为对方点允许且无需输入密码；此项单列
`M5-28`，当前不得以密码配对替代验收。
审查重连时补充发现：Pending 设备可能已被 LAN 发现标为 Online，但仍未
建链；M5-25 纳入该状态的周期重连验证，终态不自动重新配对。

2026-09-29 本机验证（Ubuntu Linux x86_64，GCC Debug）：
`cmake --build build/debug -j4` 通过；针对性测试 12/12、11/11、
10/10 用例通过；最终全量 `ctest --test-dir build/debug
--output-on-failure -j4` 43/43 通过；Impeccable 机械扫描对变更的
UI 文件返回 `[]`。受限建链、授权方向与弹窗位置尚需双设备/GUI 复验，
负责人 Linductor 与设备操作者；条件为目标设备安装同版 deb、两端
Aki 在线并可互通 TCP，执行 M5-25~27 的验收动作。
同日补充：修正 Pending 已在线但未建链时的重连对账判定；
`cmake --build build/debug --target aki test_reconnect_loop test_host_runtime -j4`
通过，`test_reconnect_loop` 8/8、`test_host_runtime` 4/4 用例通过。
双机首轮验证：CI 产物 `aki-0.1.0-Linux.deb` 的 SHA-256 为
`a867dd6a8817363fd7bf5b418595b8143dc4483dadd19c2d912ed20ed6e16fcc`，
目标机 `ybt@192.168.1.206` 所安装包哈希一致，运行于 Ubuntu 22.04
amd64。目标机发现本机 Active、待确认，但点击「验证密码」后无可见反馈；
本机会话显示连接断开等待恢复。双向 TCP 监听端口可达；两端 Heyaki
TrustStore 中旧 grant 方向/撤销状态不一致。复验前修正断连时错误开放的
验证入口、未接纳请求的弹窗反馈，并给在线未建链设备提供连接入口。
连接、授权和消息实测仍未通过，M5-25~26 保持未完成；负责人 Linductor，
补跑条件为两端安装上述交互修正包并重新执行连接→口令→消息步骤。
交互修正的本机验证：`cmake --build build/debug --target aki test_ui_models
-j4` 通过，`test_ui_models` 12/12 用例、168/168 断言通过；双端复验
尚待新版安装。

2026-09-30 续验：`be3e4e8` 的七项 CI 均在 `test_device_trust` 失败；
测试仍使用旧的“Pending 即可验证”前置条件。补入真实连接事件后复验。
同时修正 UI 通道已接纳而 Adapter 拒绝时未发布失败状态的问题；
NodeSession 提交前核对受限会话快照，相关上游完成契约缺口登记为
[HEY-20260930-001](../heyaki_feedback/ledger.md)。回环测试统一单侧口令
提交，并验证发起方结果与双端 authenticated，避免交叉配对使结果失效。
单侧配对后的实跑暴露 `test_transfer_full_loopback` 旧前置缺陷：等待
未赋值的标志、未建立 Conversation、未连接 RouterSink。补齐测试装配，
改为消息哈希和 wire 终态实测，删除终态注入，并在失败时先停 worker。
断线恢复回环仍可能输出环境降级，必须按实际日志记录，不能由退出码
推断真实断线恢复已验收。
实跑同时发现发送归档会话未按网络进度推进 Transferring，导致后续
Completed 被拒。修正 TM 的发送会话路径，单测改用真实进度事件完成
状态链；全链路回环补数据库 worker，使完成后的归档、DB 哈希与文件
本体一致性能够实际执行。
本机最终 Debug 构建通过；`ctest --test-dir build/debug --verbose
--output-on-failure -j4` 43/43 无失败（24.24s），其中完整文件回环实际
通过 31 个断言，发送路径单测 11 用例/177 断言，设备信任单测
11 用例/81 断言，UI 模型 12 用例/168 断言。连接状态与断线恢复回环
（test_peer_sessions_loopback、test_disconnect_recovery_loopback）输出
握手失败 skip，不计为真实验收。上述文件回环只证明受控关闭后的传输
落库和消息快照保持，不宣称进程重启恢复。ASAN/UBSAN/TSAN、Windows
以及 Ubuntu 20.04 打包检查由本轮 CI 承载，尚待结果；双机补跑负责人
Linductor 和设备操作者，条件为两端使用新包、在线且 TCP 可互通。
用户明确本轮先验证连接与密码授权；Heyaki 独立管理，通过台账向上游
提出能力/缺陷 issue，不在 Aki 内直接修改依赖。上游
[Heyaki #2](https://github.com/Linductor-alkaid/heyaki/issues/2) 跟踪无密码
接收方允许，[Heyaki #1](https://github.com/Linductor-alkaid/heyaki/issues/1)
跟踪配对 strand 校验失败终态反馈，分别关联上述两个反馈编号。
本轮 CI 回填：run `36659102913`、head
`ef23c78c784b99ea73356e6aa138b62cb5872198`，Linux debug/asan/ubsan/tsan、
Windows MSVC debug、Ubuntu 20.04 deb 和 Windows setup 七项全绿。
目标机已由用户手动安装并打开新版；deb SHA-256 为
`05c6dfd24f1d9de7da89d902d1fbe94e288685ac2263a34b480a7a113c783fa8`，
安装二进制与包内容 SHA-256 同为
`34f169e485f220e4e88e369525cf96edbbeef7f8f447811256b84d88a93dab76`，
`dpkg -V aki` 无差异。本机命令行下载存储地址失败，未冒充上传成功；
用户手动下载的包已通过 LAN 取回核对。
双机复验再次失败：目标机截图显示在线、LAN、待确认；验证口令后弹窗
关闭且无结果。本机调试器取证确认已 authenticated 并收到了配对请求，
Heyaki 却不发配对结果，详见
[HEY-20260930-002](../heyaki_feedback/ledger.md#hey-20260930-002非对称授权状态下接收端静默忽略密码请求)
及 [Heyaki #3](https://github.com/Linductor-alkaid/heyaki/issues/3)。
M5-25~28 均保持未完成；非对称旧授权修复由 Heyaki 独立处理，不能用
首次配对测试或 CI 全绿替代双机验收。

## M5-12~M5-17：双端实测后的修复与设备体验

> 状态：In Progress；负责人：Linductor；设计依据：
> [DEC-019](../decisions/DEC-019-directional-trust-and-chat-scopes.md)、
> [DEC-020](../decisions/DEC-020-signed-lan-device-name.md)、
> [DEC-021](../decisions/DEC-021-bidirectional-trust-and-repair.md)、
> [Aki 设计](../design/aki_design.md)及 [UI 规范](../design/aki_ui_design.md)。

## M5-18~M5-24：第二轮双端实测修复（信任四态与恢复体验）

> 状态：In Progress；负责人：Linductor；设计依据：
> [DEC-021](../decisions/DEC-021-bidirectional-trust-and-repair.md)。

- [ ] `M5-18` Settings 本机名输入可清空重输、带用途说明且不与密码区重叠。
- [ ] `M5-19` 会话入口按连接/会话事实门控，Rejected/Revoked 已连接可通信。
- [ ] `M5-20` 终态设备有「重新配对」入口（Rejected/Revoked→Pending 显式边）。
- [ ] `M5-21` 四态信任显示（inbound_trust 校准/撤销归零/降级启发）。
- [ ] `M5-22` 掉线恢复：周期重连对账 re-arm，presence/会话状态随 connected 恢复。
- [ ] `M5-23` 图片/文件选取按存在性过滤（zenity stderr 诊断行不再致 stat 失败）。
- [x] `M5-24` 24.04 图标：desktop StartupWMClass 已落；Wayland app_id
  按 [DEC-024](../decisions/DEC-024-linux-desktop-window-identity.md)
  在 Aki 平台适配层补齐窗口创建参数；不改 pinned 依赖，上游 #77 继续
  独立跟踪。真实 Wayland/X11 标识检查及 Ubuntu 24.04 安装版 Dock 验收
  通过，状态 Completed。

2026-09-30 M5-24：用户在安装版再次复现 Dock 图标缺失。图标与 desktop
布局正确，pinned runner 缺窗口身份；按 DEC-024 在 Aki 平台层设置三个
GLFW 字符串 hint，非 Linux 无操作，不改 third_party/后端/并发设施。
Debug `aki` 构建成功，`desktop-file-validate` 和 `git diff --check` 通过。
最初本机 Debug 缓存仅编译 X11，原生 Wayland 探针初始化失败，未计成功；
将 Ubuntu `libxkbcommon-dev 1.6.0-1build1` 解包至 `/tmp`，通过
`PKG_CONFIG_PATH` 提供缺失开发头文件，重配置 GLFW 双后端后构建成功。
真实 GNOME Wayland 探针连续创建两个窗口，协议各发送
`xdg_toplevel.set_app_id("aki")`；同一公开 API 的 X11 探针两个窗口
`WM_CLASS=aki,aki`。GDB 对真实 Aki 在 `glfwCreateWindow` 前断点核实
两种后端的三个 hint 均为 `aki`（Wayland platform=393219，X11=393220），
证明配置首次查询落在 GLFW 初始化后、窗口创建前，后端未覆盖 hint。
不使用仅校验调用次数的 mock 测试替代真实桌面行为。
日志：`/tmp/aki-dock-identity-build.log`、
`/tmp/aki-dock-identity-wayland-build.log`、
`/tmp/aki-dock-wayland-protocol.log`、
`/tmp/aki-dock-identity-gdb.log`、
`/tmp/aki-dock-identity-wayland-gdb.log`；诊断源码和二进制仅在 `/tmp`，
不进入分发或仓库。CI、deb 交付及安装版 Dock 观察仍待执行；负责人
Linductor/设备操作者，补跑条件为 Ubuntu 24.04 正常退出旧版后安装新包。

同日运行版复验：用户正常退出旧安装版后启动上述双后端修复版，确认
“图标正常，归为同一个图标”。此结果证明 Ubuntu 24.04 GNOME Dock 的
实际关联恢复；当前运行的是 Debug 构建，尚不替代 CI deb 安装复验。
代码 head `5f8d71bd027998e6c78b668a2f9aba6f2890f842` / CI run
`36696751776`：17:52（Asia/Shanghai）核实 Windows Debug success，
其他六项仍 queued；Linux 打包 runner_id=0，无日志或失败 annotation。
本机未配置 Ubuntu 20.04 容器构建环境，兼容 deb 交付等待既有 CI runner。
本轮验收文档提交不改变应用代码，最新 head 的七项 CI 仍须独立核实；
不得用运行版验收或单项 Windows 成功宣称整批完成。

2026-10-01 M5-24 收尾：run `36699133720` 对应
`d2c39561f64b58232839ca00e412b1e59a31935c`，七项 job 全部
completed/success（Linux Debug/ASAN/UBSAN/TSAN、Windows Debug、Linux
deb、Windows setup）。artifact `11090715903` 的 archive SHA256
`c470136dc6dec9d72dc0a7267460f829ed0a79f6546c7036d4ea105ec0f7388f`
核对通过；deb SHA256
`a9f44db30a26966f18346f8ad69aa5062e09c531ab73aedd2a13c2cdc4030444`，
大小 13,600,374 bytes，amd64、libc6 >=2.30，桌面文件校验通过。
本机包 `/home/linductor/下载/aki-0.1.0-d2c3956-amd64.deb` 已由用户安装、
从桌面入口重开并确认“同一图标正常显示在 dock”；安装二进制与 CI 包
均 SHA256 `1e19ac04cb31d8ed09fa3792bd35aaab4b1c23e22f0f2a402aaa578f28a7811e`，
hicolor 图标与源资源哈希一致。M5-24 完成。
原目标 `ybt@192.168.1.206` 上传尝试返回 No route to host，用户确认设备
当前离线，未宣称上传成功；补传负责人 Linductor/设备操作者，条件为该
设备上线并确认地址。此交付待办不替代也不影响 Ubuntu 24.04 本机 Dock
验收。当前可用 Windows `192.168.5.99` 尚未安装 Aki，跨平台通信测试
将在 Heyaki 更新接入后的新包上进行，另项记录。

2026-09-29：用户第二轮双端实测发现上述七类问题，归入本维护批；
双端复验需分别测试撤销方向、重建配对与断线恢复时序。

- [ ] `M5-12` 入站消息采用真实接收时间并覆盖持久化后显示；验收：入站
  无时间戳仍得到非 epoch 时间，重启后时间不回退为 `08:00`。
- [ ] `M5-13` 图片和文件可从聊天窗口发出并展示发送失败；验收：双端
  真实图片/普通文件传输与旧 profile scope 迁移，失败可见。
- [ ] `M5-14` 输入法编辑时退格只由输入法处理；气泡深度和文本布局无
  裁切；验收：中文输入法组合串、长文本、不同窗口尺寸及双主题 GUI 复验。
- [ ] `M5-15` 连接与单向 Heyaki 信任分别建模和展示；验收：A 输入 B 口令
  后只有 A 侧信任推进，B 侧保持待确认，双侧消息/文件按授权范围工作。
- [ ] `M5-16` 首启与 Settings 可设/可改本机设备名，设备列表与详情显示对端设备名并支持本机
  备注；验收：双端交换名称、备注本地持久化、重启恢复和长名称布局。
- [ ] `M5-17` 首启与 Settings 可选语言，中文为默认且可切换英文；验收：
  设置持久化、关键路径两语言覆盖及中文字体和溢出 GUI 复验。

2026-09-29：用户同网双端实测发现上述七类问题，归入本维护批；现有
`M5-11` 的双端配对验收证据只证明消息互通，不证明文件、输入法或双向
信任语义。双端复验需两端同版、可入站 LAN/TCP、分别测试两种信任方向；
负责人 Linductor，未执行项保持未完成。

## 目标

交付 EUI-NEO 主窗口与基础主题（`SCOPE-12`）及四页导航（Conversations /
Devices / Transfers / Settings），覆盖设备列表（`SCOPE-04` 展示面）、
会话与聊天窗口（`SCOPE-05`/`SCOPE-06`/`SCOPE-07` UI 面）、文件卡片与
Transfers 页（`SCOPE-08` UI 面），并按设计第 15 节逐项验收 MVP
（`SCOPE-01`~`SCOPE-12` 全边界）。

UI 只消费 Application State（`RULE-02`/`EXEC-03`：DoubleBuffer 一致快照
主线程排空，`app::requestUpdate()` 跨线程唤醒）；并发全部经 pinned
executor（`DEC-005` 并发边界：不使用 EUI-NEO `app::async`/`core::network`/
`audio`；`ExecutorOwner` 关闭编入 `DslAppConfig::onShutdown`，保持
`EXEC-01` 唯一 owner）。本里程碑不改变 M3/M4 已交付的网络、持久化与传输
语义，只接 UI 面；新增的 `M5-11` 维护项按 DEC-018 单独修正配对契约。

## M5-11：本机口令与设备认证（维护项）

> 状态：In Progress；设计依据：[DEC-018](../decisions/DEC-018-user-pairing-password.md)。

- [ ] `M5-11` 首次启动设置本机口令、现有 profile 可更换口令、已发现设备可主动
  建链并输入对端口令完成认证。验收：单测覆盖 verifier 与状态路径，双端 Linux/
  Windows 真机完成正确/错误口令配对，GUI 截图及日志归档。

2026-09-28：用户在本机运行时发现首次设置和设备认证入口缺失，立项。负责人
Linductor；真机验收条件为 Windows 与本机处于可入站 LAN、两端运行本变更版本。

2026-09-28（`M5-11` 实施与阶段验证，负责人 Linductor）：

- 首启及旧固定口令 profile 在 Node 启动前要求设置本机口令；仅保存 Heyaki
  Argon2id verifier。Settings 可更换口令并保持设备身份。Unknown 行增加
  Connect，受限会话驱动 Pending；确认弹窗展示公钥指纹并收集对端口令，失败
  保持 Pending 供重试。被动连接也可建立 Pending 行。
- 网络无关单测覆盖首次创建/持久化/旧 profile 迁移、更换口令、错误口令
  后重试、Unknown→Pending→Trusted、被动连接无扫描、受限会话边沿事件。
  `cmake --build --preset debug -j4` 与 `cmake --build --preset release -j4`
  成功；两档 `ctest --test-dir build/<debug|release> --output-on-failure -j4`
  均 43/43 通过、0 失败。`git diff --check` 通过；UI 文件执行
  `impeccable detect --json` 返回 `[]`。CI 门禁及 PR 证据待回填。
- PR #56 首轮 CI 六档通过，但 Windows Debug 的 `ctest` 步骤超过半小时仍
  无结论，GitHub 未提供运行中用例日志；同分支新增 Windows 逐测试 180 秒
  超时以使卡住的用例可定位，重跑 CI 后按实际结果继续修复。该档未通过前
  不合并，不计入已完成验证。
- 2026-09-29 回填（负责人 Linductor）：重跑 CI run 36446106732 七档全绿
  ——Windows / debug (MSVC) 12m58s，`skeleton.app_runs` 1.07s 通过，
  43/43 0 失败；首轮 run 36440407557 经查为 Windows Debug 间歇性挂起后
  由 concurrency cancel-in-progress 随新 push 取消（annotation
  "higher priority waiting request"），非新增失败。门禁解除，合并闭环
  按标准流程执行；挂起根因未定位，再次出现时以 `--timeout 180` 的
  `***Timeout` 输出定位。
- **待验收**：本次未对用户正在运行的旧版 Aki 数据目录启动另一实例，避免
  与活动节点争用端口和 profile；未执行新增界面的 GUI 实拍。也未在本机
  与 Windows 双端安装同一新版本并验证正确/错误口令的真实 LAN 配对；本机
  loopback 用例受入站 TCP 防火墙影响可走 `[skip]` 分支，不能代替该验收。
  负责人 Linductor；补跑条件为两端安装此变更构建、关闭旧实例、放行入站
  LAN/TCP，在双端执行首次设置、主动及被动配对、错误口令重试并保存 GUI
  截图和运行日志。完成前本工作项保持 `[ ]` / In Progress。

2026-09-29（`M5-11` 真机缺陷修复：Connect 无效与扫描残留，负责人 Linductor）：

用户双端真机测试暴露两项缺陷，本轮回修复（设计 §8.1/§11.1 同步修订）：

- **Connect 按钮完全不生效**（根因：宿主级 `PeerSessionPipeline` 在
  `ensure_assembled` 只构造、从未 start，适配器内部管线又
  `peer_observation=false`——`connect_lan` 成功后 `pairing_restricted`
  会话无人观察，`on_pairing_ready` 永不触发，设备恒为 Unknown）。修复：
  装配即常驻启动 peer 管道（200ms diff；`HostAssemblyReport.
  peer_observation_started` + `HostRuntime::peer_observation_running()`
  可观测；启动失败计入装配失败），主动/被动配对、presence/路径、断线
  重连事件源随之接通。
- **扫描出现大量设备**（根因：每次发现的 Unknown 设备整行持久化，重启
  全量恢复进 DeviceStore 无存活过滤——列表为历史所有见过设备的并集）。
  修复：启动播种过滤（`seeded_app_state`，Unknown 恢复行不入会话
  DeviceStore，DB 行不删）；发现观察管道重扫重置 seen/live、合成事件
  presence 取 Online、广播消失经新增第 13 sink 方法 `on_device_presence`
  → DM `SetPresence(Offline)` 回落；无 32 字节身份公钥的目录条目不合成。
- UI：Connect 按钮仅对 presence Online 的非本机行渲染（离线行
  `connect_lan` 必然被拒，不渲染无效按钮）。
- 测试与证据（2026-09-29，委派 Independent-Verification-Agent 执行并回报
  证据，负责人 Linductor）：新增/更新用例——`test_peer_sessions_pipeline`
  增 `diff_lan_discovery` 七用例（在线合成/trusted 跳过/非 32 字节公钥跳过/
  seen 去重/消失回落/重现不再合成/**trusted 毕业不误报离线**）；
  `test_host_runtime` 增 `seeded_app_state` 两用例 + 装配/关闭
  peer_observation 断言；`test_app_managers` 增 presence 路由用例（含未知
  id owner 拒绝、空 id handler 拒绝可见）；双端 sink 适配与回环测试
  presence 契约断言。验证期间发现并修复一处边界缺陷：设备由未信任毕业为
  trusted 的 tick 会误报 `went_offline`（向刚信任设备发 Offline）——
  trusted 分支改为从 live 集移除且不合成离线事件，新增用例锁定。三次
  变异注入（Online→Offline、去播种过滤、去毕业 erase）均被对应用例捕获。
  `cmake --build --preset debug|release -j` 全绿；两档
  `ctest --output-on-failure` 均 **43/43 通过、0 失败**（8 个 LAN 回环
  沿登记的防火墙 `[skip]` 降级通过，非静默）；`git diff --check` 通过。
  本机 tsan/asan 档因缺 wayland/xkbcommon dev 包无法配置（sudo 需密码，
  环境限制），由 CI 门禁 debug/asan/ubsan/tsan 四 Linux 档覆盖。CI 证据
  （2026-09-29 回填）：PR #57 run 36460711932 七档全绿——Linux debug
  9m37s / asan 11m18s / ubsan 13m20s / tsan 13m08s、Windows debug (MSVC)
  13m11s、Windows setup.exe 19m29s、Ubuntu 20.04 deb 15m03s。双端真机
  配对验收待回填（条件沿用本记录上方待验收条款）。

2026-09-29（`M5-11` 双认证方式：一次输入、两侧就绪，负责人 Linductor）：

用户对认证交互提出两种方式需求：输入对方设备口令，或由对方确认本机发出的
连接请求。协议事实（pinned heyaki 源）：口令由持有方输入、对端以其
verifier 校验并签发 grant；`handle_pairing_request` 响应侧即升级会话授权、
发起侧收到结果同此升级——**一次输入即双向可用**；pairing observer 仅在
发起方触发，此前响应侧行只能靠反向重复输口令推进，导致两侧都要输入。

- 修复：DM `DeviceConnectedWork` 对 Pending 行自动推进 Trusted
  （`advance_pending_trust`，只走合法边；Unknown 归 PairingReadyWork、
  终态由 owner 拒绝；错误口令会话 `pairing_denied` 不会进入授权态）。
  两种方式就此等价：本机输对端口令（方式一）或对方点确认输本机口令
  （方式二），任一侧一次输入、两侧设备行就绪。确认弹窗对两种角色通用，
  UI 无结构变更。
- 已知边界：grant 单侧持有时重连由持有方发起（双方重连循环竞争，持有方
  建链成功后对端会话随之授权）——登记于设计 §8.1。
- 测试：委派 Independent-Verification-Agent 执行（DM 自动推进单测：
  Pending 推进/Unknown 不动/Trusted 幂等/快照滞后无害；回环用例补单向
  `pair_peer` 两侧会话授权断言），证据随 MR 回填。
- 附带发现（登记备查）：pinned heyaki `Node::pair_peer` 为 strand 异步
  投递，返回值只反映受理（恒 true）——已授权会话上的重复提交在 strand
  侧被拒后结果被丢弃，公开面无可观测返回值或事件（RULE-09 在 heyaki
  边界的可见性缺口，Aki 不可观测；若需重复提交反馈应向 heyaki 上游
  反馈，不在本仓库修改 third_party）。旧「双端各自 pair_peer」流程实为
  协议性交叉配对（每侧先以 responder 升级、随后两侧
  `handle_pairing_result` 均报 `pairing_result_outside_restricted`）——
  单侧提交才是正确用法，回环测试已改为单侧流程后在本机首次真实跑通
  配对→信任→撤销→错误口令全链（不再 `[skip]` 降级）。
- CI 证据（2026-09-29 回填，PR #58 三轮）：首轮 run 36475814621 七档全绿
  ——Linux debug 9m46s / asan 10m19s / ubsan 15m03s / tsan 13m03s、
  Windows debug (MSVC) 13m05s、Windows setup.exe 18m58s、Ubuntu 20.04 deb
  11m20s（回环配对链在 CI LAN 沙箱真实跑通，无降级退出）。证据回填重跑
  （run 36478108803）tsan 档暴露 `BUG-20260929-tsan-sink`：回环测试发现
  sink 在 executor timer 线程执行 Catch2 断言，与主线程断言构成
  `RunContext::assertionPassedFastPath` 数据竞争（既有模式潜在竞争，非
  本轮产品代码缺陷）——修复为原子记录 + 主线程断言后，合并门禁 run
  36480925888 七档全绿（Linux debug 7m43s / asan 11m22s / ubsan 10m50s /
  tsan 13m07s、Windows debug 12m47s、setup.exe 17m46s、deb 12m35s）。
  记录登记重跑（run 36483070370）：Windows debug 首跑 12 测试同因
  `transport: lan_no_ready_interface` 失败（runner LAN 接口未就绪的环境
  抖动，与 #56 时代 Windows Debug 间歇性挂起条款同型登记）——失败档重跑
  12m50s 通过，其余六档未变动。双端真机配对验收待回填（条件沿用本记录
  上方待验收条款）。

## 范围与非目标

### 范围

- `M5-01` 设计先行与运行复核：[Aki UI 设计规范](../design/aki_ui_design.md)
  第 5 节要求的「与 pinned EUI-NEO 实际版本一致性复审」+ EUI-NEO 最小运行
  探针（窗口/compose 循环/主题逐项覆写实测——`RISK-2026-002` 运行复核
  收口：virtuallist 固定行高模型 vs 变高气泡列、dialog/toast open 状态
  持有、文件对话框只读能力）；UI 装配契约固化（设计第 9 节细化：AppState
  →视图模型派生、快照消费与唤醒、`onShutdown` 关闭序、主题档位覆写清单、
  渲染层验证策略）；偏差先更新设计/决策再合代码（M1-08 纪律）。
- `M5-02` EUI-NEO 接入与主窗口骨架：submodule 拉取登记（`DEC-005` pin
  `b9032a8a`）+ 锁文件 + configure 三方校验（沿 M3-01 fetch 纪律）；单一
  构建图 `add_subdirectory(third_party/EUI-NEO)` + `eui_neo_configure_app()`
  （构建开关 CACHE FORCE 按 `DEC-005` 冻结值：`EUI_DEPS_MODE=bundled`、
  apps/test-fixtures/install/modules OFF、glfw+opengl）；`aki_ui` 实体化
  （`eui::neo` 仅由 `aki_ui` 链接，`RULE-01`/`RULE-10`）；三栏布局壳 +
  四页导航路由 + `ui/theme` light()/dark() 逐项覆写；`ExecutorOwner` 关闭
  编入 `onShutdown` 钩子（`EXEC-01`）。
- `M5-03` 状态消费面与视图模型：DoubleBuffer 快照主线程排空 +
  `app::requestUpdate()` 跨线程唤醒（`EXEC-03`/`RULE-02`）；`ui/models`
  视图模型从 AppState 派生（设备/会话/消息/传输四域）；UI 只读不直写，
  操作经 Application 出站面（Manager 出站接口）下达。
- `M5-04` Devices 页（`SCOPE-04` + `SCOPE-02`/`SCOPE-03`/`SCOPE-10` 展示
  面）：设备列表（名称/类型/OS/连接方式/在线状态）、发现→信任操作面
  （Pending 确认/拒绝/Revoked 撤销）、Presence 与连接路径徽标
  （`Lan`/`P2p`/`Relay`/`Unknown` 视觉语义按 aki_ui_design 第 3 节）。
- `M5-05` Conversations 页与聊天窗口（`SCOPE-05`/`SCOPE-06`/`SCOPE-07`
  UI 面 + `SCOPE-08` 会话内文件卡片）：会话列表、消息历史
  （`virtuallist`，行高模型按 `M5-01` 复核结论）、文本发送、图片发送
  （文件对话框只读选取→hash-first 发起链路，`DEC-010`/`DEC-011`）、消息
  气泡/图片预览（`dialog`）、输入区、会话内文件卡片（进度组件与 Transfers
  页复用，`aki_ui_design` 第 4 节）。
- `M5-06` Transfers 页（`SCOPE-08` Transfers 页面）：传输任务集中列表与
  操作面（暂停/恢复/取消；含无会话行 `cancel_transfer` 直接终态入口
  （`DEC-013`⑥）与孤儿接收行 re-push 触发面登记）；接收根残留 GC 议题
  处置（`DEC-012`③ 登记的 M5 GC 议题：实现或显式延后并登记触发条件）。
- `M5-07` Settings 页与主题（`SCOPE-12`）：主题三选（跟随系统/浅/深——
  无系统主题检测 API 缺口按 `DEC-005` 处置：Aki 平台层查询或先交付浅/深
  两档并如实登记）；最小设置项（数据目录展示等）。
- `M5-08` MVP 全链路验收（设计第 15 节 / `SCOPE-01`~`SCOPE-12` 逐项）：
  双端真机链路（与 M3/M4 退出补跑同批环境）；文本/图片/文件 + 进度 +
  暂停/恢复/取消 + 历史/传输行重启恢复 + 断线恢复原会话继续可用；MVP
  验收清单逐项归档；M3 登记的补做条件复核（占位口令 verifier/secret
  backend/DeviceIdentity 元数据——真实口令流程随 M5）。
- `M5-09` 收口审计与退出证据归集（沿用 M1-08/M2-08/M3-09/M4-07 纪律）。
- `M5-10` 导航与视觉修整：按 `aki_ui_design` §2.4/§2.6 将已登记的四个
  Font Awesome 图标用于左栏；在本设备 2× 缩放默认窗口与较宽窗口实拍，
  修复控件越界、说明叠印和深色主题 primary 按钮不可读。仅改 UI 装配与
  用户文案，不改变网络、状态机或传输语义。

### 非目标

- 语音/视频通话、远程终端、屏幕控制、设备命令、Agent（`SCOPE-13`~
  `SCOPE-17`、`POST-01`~`POST-03`）。
- 群组/多设备会话（`POST-04`）。
- Android/移动端（`POST-05`）。
- CI 内 UI 渲染自动化（GLFW+OpenGL 渲染不进 CI）：UI 逻辑（视图模型/
  状态映射/指令出站）以网络无关单测承载，compose/渲染层本机手工验证 +
  证据归档，如实登记限制（`RULE-11`）。
- 内容寻址去重、断点续传语义扩展（既有延后项不变）。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 9 节（GUI 三栏与四页导航）、
  第 10 节（状态管理与线程边界）、第 14 节（`ui/` 目录与依赖方向）、
  第 15 节（第一阶段范围与 MVP 清单）。
- [Aki UI 设计规范](../design/aki_ui_design.md)：第 2 节（ZCode → EUI-NEO
  绑定映射：排印/间距圆角/语义色板/组件映射）、第 3 节（领域状态视觉
  语义）、第 4 节（页面与组件映射表）、第 5 节（落地与验证、启动前复审
  要求）；[ZCode Design System 原文](../design/zcode-design-system.md)。
- [DEC-005](../decisions/DEC-005-eui-neo-integration.md)（集成方式/构建
  开关/并发边界冻结/静态盘点与已知缺口）、[DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)
  （分层与状态边界）、[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
  （Manager 模式——UI 经 Application 出站面，不直触 Manager 内部）、
  [DEC-010](../decisions/DEC-010-image-message-contract.md)（图片消息面
  展示与发送链路）、[DEC-012](../decisions/DEC-012-receive-merge-bearing.md)/
  [DEC-013](../decisions/DEC-013-orphan-row-recovery.md)（传输终态语义/
  孤儿降级——Transfers 页操作语义依据）。
- M3 移交：发现来源分期（Relay/邀请链接/手动输入展示面）、占位口令
  verifier + secret backend + DeviceIdentity 元数据补做条件（M3 里程碑
  记录归档）；M4 移交：接收根残留 GC（`DEC-012`③）、孤儿接收行 re-push
  触发面、孤儿传输行/单侧到达 UI 兜底（M4-03/05/06 边角登记）。
- AGENTS.md Executor 强制规则与工程规范第 9 节；`EXEC-01`（onShutdown
  关闭序）/`EXEC-02`（第三方回调隔离——EUI-NEO 回调若有，同样只投递）/
  `EXEC-03`（DoubleBuffer 快照消费）/`EXEC-06`（可观测）/`EXEC-07`（句柄
  所有权）。

## 工作项

（条目的完整内容与边界见「范围与非目标」各 `M5-NN` 条；此处为勾选跟踪与
可验收结果锚点。）

- [x] `M5-01` 设计先行与运行复核（可验收：aki_ui_design 第 5 节一致性复审
  记录落档；EUI-NEO 最小运行探针实测结论——`RISK-2026-002` 运行复核收口；
  UI 装配契约固化进设计第 9 节；偏差先更新设计/决策再合代码）。
  （2026-09-26 完成：§5 一致性复审逐项落档（16 组件存在一致；§2.1 排印/
  §2.2 圆角尺寸默认档偏差确认→覆写清单；§2.4 深度锚点修正
  fieldVisuals().popupShadow；间距一致零覆写）；运行探针三项复核收口——
  virtuallist 固定行高模型实测确认（rowHeight 统一值锚点 + 渲染通过，
  变高气泡按组合策略承接）、dialog/toast 页面持有 + requestUpdate 唤醒
  重组实测（waker 原型 11 次开合转换全部拾取——EXEC-03 契约原型）、文件
  对话框只读满足发送选取（接收侧按接收根无需求）；**组合模型发现**：compose
  为保留模式事件触发（静态 UI 不重组，探针 12s 仅 2 次 compose）——装配
  契约关键输入；设计 §9.1「UI 装配契约」固化（视图模型派生/快照消费与
  唤醒/onShutdown 关闭序/主题覆写清单/渲染层验证策略）。详见验证记录。见
  下方 2026-09-26（M5-01）验证记录。）
- [x] `M5-02` EUI-NEO 接入与主窗口骨架（可验收：configure 三方校验通过、
  `aki_ui` 实体化构建图生效、三栏壳 + 四页导航可运行、`ui/theme` 逐项覆写
  留对照、`onShutdown` 关闭序经关闭路径验证）。
  （2026-09-27 完成：单一构建图接入（八项开关 CACHE FORCE，configure 通过，
  bundled 八件套零联网）；`aki_ui` 实体化（eui::neo 仅由 aki_ui 链接，测试
  exe 不链 eui 经 dumpbin 核查 0 命中）；宿主入口迁移 dslAppConfig()+compose()
  （/SUBSYSTEM:WINDOWS）——组合根抽离为 EUI-NEO 无关的 HostRuntime
  （app/lifecycle/host_runtime，设计 §9.1 首帧装配例外条款先行落档），
  首次 compose 惰性装配 + onShutdown 薄委托受控关闭；三栏壳 + 四页导航
  本机可运行（截图+日志证据归档）；ui/theme akiTheme() 逐项覆写 + 双份
  回归对照（test_ui_theme_values 独立抄录期望值 + GUI 运行日志对拍）；
  onShutdown 关闭序经 test_host_runtime 关闭路径验证（DOD-02 六项 +
  §8.3 钩子原序断言）+ GUI 会话日志（8 步钩子序 + fully_stopped）；debug/
  release 全量 ctest 40/40 零回归；DEC-014 落档（tsan 全图插桩 + CI 依赖
  集，覆盖声明五条）；DEC-005「影响与风险」验证项逐项回填。见下方
  2026-09-27（M5-02）验证记录。）
- [x] `M5-03` 状态消费面与视图模型（可验收：快照排空 + 跨线程唤醒路径经
  单测——机制契约面：注入回调于 executor 任务内的 owner drain 中触发，
  现行管线 drain 仅在主线程；四域视图模型派生有断言，UI 操作全经
  Application 出站面——边界由退出-3 grep 与单测共同锁定）。
  （2026-09-27 完成：`ui/models` 拆分为 EUI-NEO 无关独立目标 `aki_ui_models`
  （四域视图模型纯函数派生 + `consume_ui_state` 水位消费面 + `UiActions`
  注入出站面，测试 exe 直链——DEC-005「测试 exe 不链 eui」落地面）；发布点
  唤醒回调 `AppStateOwnerOptions::on_publish`（owner 上下文、publish 后同步
  调用、异常全捕获计数）经 `HostRuntime::ensure_assembled` 装配参数注入，
  GUI 宿主传 `app::requestUpdate()`（本机日志留首触发证据）；main_window
  占位页最小接线（四域计数消费展示 + Devices 页发现启停出站示范）。新单测
  test_ui_models（派生/水位去重/路径 mailbox 独立推进/发布→唤醒调用序/钩子
  异常收口/executor 任务内驱动 drain 的回调契约——机制契约面：注入回调于
  executor 线程触发，现行管线 drain 仅在主线程，78 断言）+
  test_ui_actions（页面→出站接口→Manager 泵→Fake Adapter SPI 通道，
  51 断言）；debug/release 全量 ctest 42/42 零回归；退出-3 grep 扩面全 0。
  见下方 2026-09-27（M5-03）验证记录。）
- [x] `M5-04` Devices 页（可验收：`SCOPE-04`/`SCOPE-02`/`SCOPE-03`/
  `SCOPE-10` 展示面逐项可演示——列表、信任操作面、presence/连接路径徽标）。
  （2026-09-27 完成：SPI 信任操作扩展（出站 `confirm_pairing`/`revoke_trust`
  + 入站第 12 方法 `on_pairing_completed`，DEC-006 映射 3 落地，真实/Fake
  对齐）；DeviceManager 信任三操作 + 配对结果路由（§4 固定转移边校验，
  非法转移拒绝可见）；[DEC-015](../decisions/DEC-015-per-device-connection-path.md)
  逐设备路径落地（退役全局摘要；初连路径补发、断连置 Unknown）；
  UiActions 信任三操作；Devices 页实体化（列表行/信任操作面/确认弹窗 mono
  指纹/presence·路径·信任徽标语义色/发现来源分期披露）；[DEC-016]
  (../decisions/DEC-016-pairing-password-verifier.md) 口令常量 + 真实
  verifier（取代 M3-03 占位假编码串，存量 profile 处置登记）；
  test_device_trust 76 断言 + 口令往返用例；debug/release 全量 ctest 43/43
  零回归；Devices 页本机截图归档。见下方 2026-09-27（M5-04）验证记录。）
- [x] `M5-05` Conversations 页与聊天窗口（可验收：`SCOPE-05`/`SCOPE-06`/
  `SCOPE-07` UI 面 + 会话内文件卡片逐项可演示——含文本/图片发送与消息
  历史滚动）。
  （2026-09-27 完成：Conversations 页实体化——会话列表（复用 M5-03
  ConversationView 派生 + 最后消息摘要/方向/投递徽标扩展）+ 聊天窗口
  （头部信任/路径徽标、§3 断连横幅、变高气泡列按「卡片自绘 +
  scrollview」承接（M5-01 复核结论实测成立）、文本/图片消息气泡与
  文件卡片（进度语义色，M5-06 Transfers 页复用形态）、输入区、图片
  预览弹窗、New chat（Trusted 设备选择）弹窗）；发送面页面形改造——
  wire id 生成收敛 `heyaki/adapter/wire_ids.hpp`（`NodeSession::
  new_transfer_id` 委托同入口，§6.1 同步）+ UiActions 增 id 生成器注入/
  send_image 绑定 hash-first 编排（DEC-010/DEC-011）；视图模型扩展
  MessageView（方向/投递态/媒体载荷 + TransferStore 消费侧 join，无传输
  行单侧到达兜底态）；唤醒缺口修复（本机 GUI 实测）——owner 增
  `on_update_submitted` 受理点唤醒钩子（§9.1 同步），发送结果不再滞留至
  下一次输入事件；文本发送 GUI 实测（real Adapter 无会话拒绝 → Failed
  徽标可见 + 列表摘要实时更新）；新断言 test_ui_models 116 + test_ui_
  actions 65；debug/release 全量 ctest 43/43 零回归；本机 GUI 截图 ×5 +
  运行日志归档（RULE-11）。见下方 2026-09-27（M5-05）验证记录。）
- [x] `M5-06` Transfers 页（可验收：传输集中列表与暂停/恢复/取消操作面
  可演示；接收根残留 GC 议题实现或显式延后并登记触发条件）。
  （2026-09-27 完成：Transfers 页实体化（`ui/pages/transfers_page`）——
  传输行=文件卡片共享形态（M5-05 ③ 抽出为 `ui/components/transfer_card`，
  §4 复用契约兑现；形状与语义色不变）+ 操作面 Pause/Resume/Cancel 按
  TransferView 状态门控派生（§7 固定边）经 UiActions 既有传输三接口；
  Paused 行 Cancel = DEC-013⑥ 无会话行直接终态入口 UI 触达（GUI 实测
  Paused 行经 Cancel 即转 Cancelled 中性态；admission 反馈可见性原证据
  存在页脚叠印缺陷，修正后 GUI 复验——见验证记录⑩）；孤儿
  接收行 re-push 触发面=页脚登记披露；接收根残留 GC 议题处置=显式延后
  （aki_design §11.1 同步，触发条件三则登记于本记录）；TransferView 增
  mime_type 透传；新断言 test_ui_models 135 + test_ui_actions 67；
  debug/release 全量 ctest 43/43 零回归；GUI 截图 ×2 + 运行日志归档
  （RULE-11）。见下方 2026-09-27（M5-06）验证记录。）
- [x] `M5-07` Settings 页与主题（可验收：主题三选按 `DEC-005` 缺口处置
  落地并如实登记，最小设置项可演示）。
  （2026-09-27 完成：Settings 页实体化——主题三选（跟随系统/浅/深）
  segmented（§4 映射）页面持有 UI 态 + 经 akiTheme()/akiSemanticColors()
  装配面生效（覆写清单/回归对照不变）；DEC-005 缺口走平台层查询路径——
  `app/lifecycle/system_theme` 平台条件编译单元（Windows
  AppsUseLightTheme；公开面仅 std 枚举 RULE-10；其余平台 Unknown 回落
  Light 页内披露）；主题选择会话级（schema v1 无设置表，不私自扩表，
  持久化登记为后续项）；最小设置项=数据目录（HostRuntime 装配面）+
  本地设备 id（快照）mono 只读展示；resolve 状态机抽 EUI-NEO 无关
  ui/theme/theme_mode.hpp（网络无关单测，test_ui_theme_values 62 断言）；
  debug/release 全量 ctest 43/43 零回归；深浅两档渲染对照截图 ×3 +
  运行日志归档（RULE-11）。见下方 2026-09-27（M5-07）验证记录。）
- [x] `M5-08` MVP 全链路验收（可验收：设计第 15 节清单 + `SCOPE-01`~
  `SCOPE-12` 逐项归档；M3 登记补做条件复核闭环；环境受限沿降级纪律）。
  （2026-09-27 完成（验收归集类，无产品代码变更）：SCOPE-01~12 逐项
  归档两态标注（本机已验证/双端待补跑）——证据锚点映射 M1~M4 里程碑
  验证记录与 M5-01~07 GUI 实测；验收基线复跑 debug/release 全量 ctest
  各 43/43（干净树）；本机补证：发现启停 GUI 实拍（start_discovery
  admitted）+ 文本发送/传输终态/主题设置引用既有归档；**发现一项 GUI
  缺陷**（Devices 页 Pending 行 Confirm 点击无响应，复现 2 新会话 ×3
  点击，相邻按钮正常——登记独立修复工作项，SCOPE-03 信任确认弹窗 GUI
  验收被该项阻塞，网络无关半边由 test_device_trust 承载）；M3 登记
  复核闭环：口令 verifier 闭合（DEC-016）/secret backend 再登记/
  DeviceIdentity 元数据再登记/kAkiPairingPassword 维持（DEC-016 移除
  条件未触发）；双端项显式降级（原因/负责人/补跑条件，退出-1 保持
  未勾选）。见下方 2026-09-27（M5-08）验证记录。）
- [x] `M5-09` 收口审计与退出证据归集（可验收：审计记录 + 退出-1~5 证据
  齐备；工作项全部完成不自动关闭里程碑——关闭以收口审计为准）。
  （2026-09-27 完成（纯审计与文档，无产品代码变更）：设计-实现审计矩阵
  逐项一致（aki_design §6.1/§9/§9.1/§10/§10.1/§11.1/§14 对 ui/ 四子目录 +
  app/lifecycle + main.cpp；aki_ui_design §2~5；DEC-005/014/015/016 逐一）
  ——未记录偏差数 0；六项已知偏差锚点逐一核实（登记全部维持）；退出-2~4
  证据本会话复验（DOD-02 映射 + 边界 grep 全 0 + debug/release 全量 ctest
  各 43/43 + gh 逐 PR 核实 #39~#49 五档全绿）；退出-5 链接核验
  `links checked: 145, broken: 0`；审计附带处置：M4 退出-2~5 勾选沿 M3-09
  先例对齐、两处更新日期订正、许可证检查维持发行前登记；**审计发现并
  登记 BUG-20260927-002**（master push tsan 间歇红档——CI 专有测试代码
  竞争触发 vendored Catch2 内部状态，非产品竞争，独立 MR 修复）；退出-1
  双端全链路按降级纪律保持未勾选，M5 保持 In Progress。见下方
  2026-09-27（M5-09）验证记录。）
- [x] `M5-10` 导航与视觉修整（可验收：四项图标/标签在深浅两档完整可见；
  1080×720 与 1600×900 本机窗口的四页无截断或叠印；深色 primary
  操作文字和发送图标可读；Debug 构建及全量测试通过，GUI 截图与设计
  映射同步）。（2026-09-28 本机完成；验证记录见文末。M5 退出-1
  双端补跑状态不变。）

## 风险与阻塞

- **`RISK-2026-002` 运行复核（Mitigated → 收口）**：静态盘点 16 组件全部
  存在，运行复核挂 `M5-01`——virtuallist 固定行高模型对变高气泡列的适配、
  dialog/toast 页面持有 open 状态与单向数据流的配合、文件对话框只读能力
  对发送链路的满足度，以实测为准；缺口处置优先原语组合，确需上游能力或
  贡献时按工程规范以决策记录确认，不得修改 pinned 依赖。
- **防火墙/LAN 双端环境限制**：M3/M4 退出-1/3 补跑与 `M5-08` MVP 双端验收
  同批约束——沿 M3-09/M4-07 降级纪律（网络无关断言拆分、[skip] 显式 +
  补跑条件），不冒充已验证；补跑由负责人执行。
- **无系统主题检测 API**（`DEC-005` 已知缺口）：Settings「跟随系统」需
  Aki 平台层查询或先交付浅/深两档；如走平台层查询需平台条件编译单元
  （`RULE-10` 公开面仅 std 类型）。（M5-07 处置收口：走平台层查询路径——
  `app/lifecycle/system_theme`（Windows `AppsUseLightTheme`；公开面仅
  std 枚举）；非 Windows 平台/查询失败 Unknown 回落 Light 并页内披露，
  跨平台系统主题扩展（如 XDG portal，需 dbus 依赖）出现需求时再立决策。）
- **CI 无显示环境**：GLFW+OpenGL 渲染不进 CI——UI 逻辑层网络无关单测 +
  渲染层本机手工验证证据归档；不在 CI 宣称的检查不得写入 CI 断言
  （`RULE-11`）。
- **默认主题档位 ≠ aki_ui_design 第 2.1 节表**（`DEC-005` 静态盘点结论）：
  `ui/theme` 装配须逐项覆写并留回归对照，不得依赖上游默认档。
- **主线程 compose 与 executor 关闭顺序**：`onShutdown` 编入 `EXEC-01`
  后，窗口/GPU 设备销毁与 worker 回收次序需测试覆盖（关闭路径 DOD-02）。
- **UpsertDevice 整行替换对易失字段的覆写隐患**（`DEC-015` 关联发现的
  登记兑现，2026-09-27 收口批）：`apply_trust_transition`
  （app/application/device_manager.hpp:230-252）从最近已发布快照读-改-写
  整行 upsert——快照滞后于 owner 在途更新时可能覆写并发 presence 变化
  （连接中被重新发现可能 Online→Offline）；wire 节奏的
  PairingCompletedWork 结果被拒时仅计 handler_rejections 即丢弃（无延迟
  重排）。M5-04 按「用户操作节奏吸收」接受（记录②），后续工作项：结果
  丢弃时延迟重排或 drain 后重读，与该覆写隐患同批处置。
- pinned EUI-NEO 升级不属本里程碑；许可证检查（字体/图标/shader/assets）
  在正式发行前完成（设计第 9 节），登记于 `M5-09` 复核项。

## 测试与退出条件

- [ ] 退出-1：MVP 全链路双端验收——设计第 15 节清单逐项 +
  `SCOPE-01`~`SCOPE-12` 全边界（发现→信任→会话→文本/图片/文件→进度→
  暂停/恢复/取消→重启恢复→断线恢复）；环境受限时按 M3-09/M4-07 先例
  「部分验证 + 如实降级声明」处置并登记补跑条件。（`M5-09` 处置：双端
  全链路属环境受限——本机防火墙拦截至端 TCP、无第二台同网段 Aki 设备
  （M3-09 起同批约束）；网络无关半边已全部验证（M5-08 ② 两态标注归档）；
  原因/负责人 Linductor/补跑条件（防火墙放行入站 TCP + LAN 双端真机）
  已登记于 M5-08 记录 ⑤。沿 M3-09/M4-07 先例**保持未勾选**，M5 关闭待
  补跑后复核；如需缩小退出口径须先经决策记录重新划界（§14 纪律）。）
- [x] 退出-2：DOD-02 六项沿 UI 新增并发路径通过——正常完成、任务异常、
  提交拒绝、执行中取消、超时、shutdown（含 `onShutdown` 关闭序与跨线程
  唤醒路径）；UI 逻辑层网络无关单测通过。（`M5-09` 复验勾选：映射见
  M5-09 验证记录 ③——六项沿宿主生命周期路径（test_host_runtime 67 断言
  直跑复核）+ 跨线程唤醒/钩子异常（test_ui_models 135 断言）+ 出站通道
  （test_ui_actions 67 断言）；UI 新增路径自 M5-03 起无池上线程/无新增
  并发面（M5-06/07 记录），六项由宿主路径承载。）
- [x] 退出-3：边界锁定——`RULE-01`/`RULE-02`（页面代码不持有 transport
  对象、UI 只消费状态变化）、`RULE-10`（公开面无 EUI-NEO/平台类型）、
  `DEC-005` 并发边界 grep（无 `app::async`/`core::network`/`audio` 引用、
  无自建线程）通过。（`M5-09` 终态代码复验勾选：七项 grep 全 0 + 测试 exe
  不链 eui 经 cmake --graphviz 链接图核验——原 dumpbin 符号表复核经复跑
  证实无区分力已更正，命令与输出见 M5-09 验证记录 ④。）
- [x] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN/TSAN 随 CI
  门禁；渲染层本机验证证据归档（截图/运行日志），不适用工具链记录限制
  与补跑条件。（`M5-09` 复验勾选：debug/release 全量 ctest 各 43/43
  （本会话，release 增量重建后）；`gh pr checks` 逐 PR 核实 #39~#49
  五档（Linux debug/asan/ubsan/tsan + Windows MSVC）全绿，run 号留档
  M5-09 记录 ⑤；渲染层截图 ×29 与运行日志归档 `build/scratch/`
  （M5-02~08 + BUG-001，RULE-11）。**如实披露**：master push 档存在
  间歇 tsan 红档（当前 HEAD run 36328366422，test_peer_sessions_loopback
  的 Catch2 断言线程竞争——第一方测试代码诱因，非产品竞争；合并门禁
  PR 档全绿），已登记 BUG-20260927-002 待独立修复（M5-09 记录 ⑧）。）
- [x] 退出-5：设计（第 9 节细化/第 14 节）、决策（UI 相关缺口如立新决策）、
  aki_ui_design 复审记录、总计划与里程碑状态同步；验证记录含可复现命令；
  环境受限项降级声明完整。（`M5-09` 复验勾选：设计 §6.1/§9.1/§10.1/§11.1
  与 DEC-005/014/015/016 及 aki_ui_design §5 记录逐项核对一致；本记录
  命令可复现；退出-1 降级声明完整；M5 系列变更文档相对链接
  `links checked: 145, broken: 0`。）

（自 `M5-01` 起按工程规范 6.1/6.3 追加。）

## 验证记录

- 2026-09-26（`M5-01` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor；纯文档 + scratch
  探针，无产品代码——探针位于 `build/scratch/m5-probe/`（gitignored），
  standalone CMake 不入产品构建图，只读消费 pinned `third_party/EUI-NEO`
  @ `b9032a8a`）：
  - **① aki_ui_design §5 一致性复审**（逐项结论已回填
    [aki_ui_design](../design/aki_ui_design.md) §4/§5）：
    - 绑定映射 16 组件在 pinned v0.6.0 全部存在（components/components.h
      伞头 + components/ 目录 37 头文件逐一对照）——一致。
    - §2.1 排印偏差确认：`TypographyTokens` 默认档（theme.h:11-23：micro
      11/caption 12/hint 13/label 14/body 16/subtitle 20/title 22）≠ 本表
      （ui-2xs 9/ui-xs 10/ui-sm 12/ui-caption 13/ui-base 14/ui-lg 16/
      ui-xl 18）→ `ui/theme` 逐项覆写（清单入设计 §9.1）。
    - §2.2 间距一致（SpacingTokens tiny 4/compact 8/content 12/section 16/
      large 20/panel 24，theme.h:33-39——探针对拍零覆写）；圆角/尺寸部分
      偏差（radius.small 6→4、control.field 35→36、menuItem 34→28；
      theme.h:47-59）→ 覆写。
    - §2.4 深度锚点复核（2026-09-26 评审纠正）：v0.6.0 **存在**
      `theme::panelShadow`/`popupShadow` 独立函数（theme.h:266/:270，
      自上游 d28609fb 2026-04-28 即在）——本记录先前「无独立函数」断言
      与 pinned 源不符、已撤回；`fieldVisuals().popupShadow*`
      （theme.h:126-128/:211-215）为其合成字段、锚点属实。aki_ui_design
      §2.4 落点已恢复为 panelShadow/popupShadow（弹层与 Toast）。
  - **② 运行探针实测**（standalone CMake + `eui_neo_configure_app`，
    DEC-005 冻结开关 CACHE FORCE；configure 通过 42.9s、Debug 构建通过；
    日志证据 `m5-probe.log` 落盘 + flush，探针由外部超时终止——无编程式
    关窗 API，onShutdown 路径归 M5-02 关闭路径测试，如实声明）：
    - 窗口启动 + compose 循环：GLFW+OpenGL 窗口打开、首帧 compose、
      画面尺寸 720x520 落盘。
    - 主题逐项覆写实测（对拍落盘）：`typography.title 22→18 subtitle
      20→16 body 16→14 caption 12→13 hint 13→12 micro 11→10 | radius.small
      6→4 | control.field 35→36 menuItem 34→28`；间距六档与默认一致零
      覆写——与静态盘点结论吻合。
    - **RISK-2026-002 三项复核收口**：
      ① virtuallist 固定行高模型**实测确认**——`rowHeight(float)` 统一
      行高（virtuallist.h:35）、行定位 `index * rowHeight`（:148），变高
      内容被固定行高裁切；200 行探针数据渲染通过。M5-05 布局策略：消息
      气泡列按「卡片自绘 + scrollview」或行内分段组合承接（原语组合纪律），
      不修改 pinned 依赖。
      ② dialog/toast 页面持有状态**实测确认**——`dialog.open(bool)`
      （dialog.h:54）、`toast.visible(bool)+bindVisible(Signal)`
      （toast.h:43-47）；探针以页面持有原子状态 + 边沿检测驱动开合，
      requestUpdate 唤醒下 13s 内 11 次开合转换全部被重组拾取（日志
      transition 序列完整）——与单向数据流配合成立（页面模型持状态，
      compose 只读派生）。
      ③ 文件对话框只读能力**满足发送选取链路**——
      `eui::platform::openFileDialog`（platform.h:13；平台能力文档：支持
      多选/扩展名过滤、不支持目录/保存 :146）：图片发送 open 选取 →
      hash-first 发起链路成立；接收侧按接收根落盘无对话框需求。
    - **组合模型发现（装配契约关键输入）**：compose 为**保留模式、事件
      触发**——静态 UI 不重复重组（对照实验：无状态变化时 12s 仅 2 次
      compose）；跨线程 `app::requestUpdate()` 唤醒 → 主线程重组拾取页面
      持有状态新值（waker 原型：非主线程线程翻转原子状态 + requestUpdate，
      每次唤醒均触发重组并拾取）。此结论固化进设计 §9.1（快照消费与唤醒
      条款）。
  - **③ UI 装配契约固化**：设计 §9.1「UI 装配契约（M5 契约，M5-01）」
    五条——视图模型派生（四域纯函数派生 + 操作经出站面）、快照消费与唤醒
    （DoubleBuffer 主线程排空 + requestUpdate 跨线程唤醒 + compose 三不
    纪律）、onShutdown 关闭序（EXEC-01 序编入钩子 + 关闭路径测试归
    M5-02）、主题档位覆写清单（逐项数值 + akiTheme() 装配归 M5-02）、
    渲染层验证策略（RULE-11：渲染不进 CI + 本机证据归档口径）。实现偏差
    先更新本节再合代码（M1-08）。
  - 验证命令（可复现）：探针 configure
    `cmake -S build/scratch/m5-probe -B build/scratch/m5-probe/build -G
    "Visual Studio 17 2022"`；构建 `cmake --build
    build/scratch/m5-probe/build --target m5_probe --config Debug`；运行
    `cd build/scratch/m5-probe/build/Debug && ./m5_probe.exe`（日志
    `m5-probe.log` 随帧落盘；探针无自退出，验证时以超时终止收集）。
  - 限制与补跑条件：探针为 Debug 单配置、kill 终止（onShutdown 未在本
    探针触发——关闭序归 M5-02 关闭路径测试）；virtuallist 裁切行为与
    dialog/toast 视觉效果为日志+代码锚点证据，像素级视觉复核归 M5-05/07
    本机手工验证（截图归档口径，退出-4）。无产品代码变更：`git status`
    复核仅 docs/design/aki_ui_design.md、docs/design/aki_design.md、
    docs/plans/{m5,aki-implementation-plan}.md 与 scratch（gitignored）。
  - 同步：本里程碑（M5-01 勾选、状态 In Progress、本记录）、
    [aki_ui_design](../design/aki_ui_design.md)（§2.4 修正 + §4 注 + §5
    复审记录）、[aki_design](../design/aki_design.md) §9.1（装配契约）、
    总计划（当前状态条目 + M5 索引 In Progress）。

- 2026-09-27（`M5-02` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① 依赖接入（DEC-005/DEC-003）**：submodule `third_party/EUI-NEO` @
    `b9032a8a848f8d8cf096bb7711c626f9c051e0ce`（v0.6.0，`git submodule
    status` 实测）+ 锁文件（M0 起已登记，本项无变更）；configure 三方校验
    属锁文件纪律的既有校验（Aki lock ↔ checkout，`cmake/Dependencies.cmake`
    pinned 分支）——debug preset configure 通过，日志含
    `Dependency 'EUI-NEO' pinned at b9032a8a…` 与
    `Pinned dependency verification passed`；bundled 八件套
    （glfw/glad/tray/freetype/zlib/libpng/md4c/miniaudio）全部
    `Using bundled … source` 零联网（DEC-006「不静默联网」先例口径，
    EUI_DEPS_MODE=bundled 冻结）。无独立 fetch 脚本需求：submodule 经
    `git submodule update --init`（CI checkout `submodules: recursive`），
    其 bundled 第三方随 pinned 源码树分发，无网络拉取环节。
  - **② 单一构建图**：根 CMakeLists.txt `add_subdirectory(third_party/
    EUI-NEO)` + 八项开关 CACHE FORCE（`EUI_DEPS_MODE=bundled`、
    `EUI_BUILD_APPS/EUI_BUILD_USER_APPS/EUI_BUILD_TEST_FIXTURES`、
    `EUI_ENABLE_INSTALL/EUI_ENABLE_MODULES` 全 OFF、`EUI_WINDOW_BACKEND=
    glfw`、`EUI_RENDER_BACKEND=opengl`；EUI_ENABLE_TRAY 保持上游默认 ON，
    DEC-014）+ `eui_neo_configure_app(aki)`（框架 main 注入，
    /SUBSYSTEM:WINDOWS 实证——aki.exe 无控制台输出、GUI 会话正常开窗）。
    eui_neo 头文件 SYSTEM 标注（IDE 生成器限制同 executor 先例）+ 框架
    注入源 glfw_app_main.cpp 源级 /W0（上游告警不进第一方 /WX 门禁）。
  - **③ aki_ui 实体化**：ui/CMakeLists.txt 静态库（theme/aki_theme.cpp +
    pages/main_window.cpp），`eui::neo` 仅由 aki_ui 链接（RULE-01/RULE-10；
    eui include 全仓仅存在于 ui/ 四文件与根 main.cpp，grep 实证）；测试
    exe 不链 eui（tests/CMakeLists.txt 零 aki_ui 链接；aki_host_smoke.exe
    `dumpbin /SYMBOLS` 对 eui::/components::/glfw 符号 0 命中，console
    约定保持，DEC-005）。宿主组合根抽离为 `aki_host` 静态库
    （app/lifecycle/host_runtime.{hpp,cpp}，EUI-NEO 无关——GUI 钩子、
    console 驱动 aki_host_smoke、关闭路径测试三方共享）。
  - **④ 宿主入口迁移 + 启动/关闭配对**：根 main.cpp 重构为
    dslAppConfig()（纯配置，const 查询语义）+ compose() 钩子；启动触发点 =
    首次 compose 主线程惰性 `HostRuntime::ensure_assembled()`（设计 §9.1
    增补「首帧装配例外」与「启动↔关闭配对」两条款先行落档——M1-08 纪律，
    compose 三不纪律的唯一显式例外）；onShutdown 薄委托
    `shutdown_with_report()`（§8.3 钩子原序 + EXEC-01 步骤 2~5）。
  - **⑤ 三栏壳 + 四页导航**：ui/pages/main_window.{hpp,cpp}——导航栏
    (fixed 64) + 列表栏 (fixed 264) + 内容栏 (fill)，Conversations/Devices/
    Transfers/Settings 路由占位（页面模型持 UI 态，compose 只读派生）。
    本机可运行证据：aki.exe（Release）GUI 会话——`aki-run.log`
    （build/release/Release/，随帧落盘）：装配 ok 32/54/60ms（含恢复路径
    identity=loaded devices=1）、主题对拍落盘、screen 1080x720；
    截图 `build/scratch/aki-m5-02-window.png`（三栏壳 + 四页导航 + light
    主题渲染；右下 Windows 防火墙弹窗为系统级提示——aki_host_smoke ctest
    运行触发，非应用内容，属 M3/M4 已登记防火墙环境约束）；taskkill
    WM_CLOSE 优雅关窗 → onShutdown 完整执行。复现：`cmake --build
    --preset release --config Release && cd build/release/Release &&
    ./aki.exe`（关闭：正常点窗叉或 `taskkill /IM aki.exe` 优雅 WM_CLOSE）。
  - **⑥ ui/theme 逐项覆写 + 回归对照**：数值权威表
    ui/theme/aki_theme_values.hpp（EUI-NEO 无关）+ akiTheme()/akiSemantic
    Colors() 装配（Typography title 22→18 subtitle 20→16 body 16→14
    caption 12→13 hint 13→12 micro 11→10（label 14 保持）；Radius small
    6→4（card/overlay/control 锚定）；ControlSize field 35→36 menuItem
    34→28；间距六档零覆写锚定；§2.3 语义色板深浅两套 + 扩展语义色）。
    回归对照双份：tests/unit/test_ui_theme_values.cpp（设计文档独立抄录
    期望值 + 预混合公式复核，不链 eui）+ GUI 运行日志「上游默认 → 覆写值」
    对拍行（M5-01 探针同款）。
  - **⑦ onShutdown 关闭路径测试（DOD-02）**：tests/unit/
    test_host_runtime.cpp（console exe，链 aki_host 不链 eui）——六项沿
    宿主生命周期路径：正常完成（submit_auto 返回值 + Manager 泵 + 真实发现
    启停 + 快照本地身份行）、任务异常（future 上浮 + task_exception_count）、
    执行中取消（submit_cancellable + request_task_cancel 协作退出 +
    CancellationStatus）、提交拒绝（set_max_in_flight_tasks(1) 耗尽 →
    CapacityExhaustedException 即时就绪 + capacity_exhausted_count）、
    超时（completion_wait 预算耗尽如实记录——超时非干净关闭
    fully_stopped=false 但 EXEC-01 步骤 5 仍收敛 Stopped；config 级排队软
    超时已由 test_app_managers 沿泵路径覆盖）、shutdown（§8.3 钩子原序
    hook_sequence 8 步逐项断言 + 两 blocking worker 2/2 回收 + 写路径
    admit==completed 零丢失 + 幂等 + 关闭后提交显式拒绝）。窗口/GPU 销毁与
    worker 回收次序：worker 回收在 onShutdown 钩子内（EXEC-01 步骤 2/3）
    完成，GPU 设备销毁（renderBackend.reset()）在钩子返回后（框架
    glfw_app_main.cpp:594-604）——次序由 test_host_runtime（钩子内回收
    断言）+ GUI 日志（shutdown 返回行后框架收尾）双重承载。
  - **⑧ 单图符号冲突核对（DEC-006 sqlite 先例）**：dumpbin /SYMBOLS——
    eui_neo.lib 含 heyaki 栈符号（sqlite3_/usrsctp/rtc::）0 命中；
    heyaki 侧 lib 含 eui 栈符号（glfw/freetype/FT_Init/md4c_/stbi__）
    0 命中；Release 全量链接日志 LNK4006/重复符号 0；aki.exe
    /DEPENDENTS = 系统 DLL + libssl/libcrypto（OpenSSL 随宿主部署），
    全静态单 exe。
  - **⑨ assets 就位**：build/release/Release/assets/（icon/fonts/shaders
    等随 eui_neo_configure_app POST_BUILD copy 生成，GUI 会话消费实证——
    窗口图标与文本渲染正常）。
  - **⑩ CI 扩面（DEC-014）**：ci.yml Linux 四档安装 pinned 集成指南完整
    依赖集（libssl/libcurl4-openssl/libgl1-mesa/libegl1-mesa/libx11/
    libxext/libxrandr/libxinerama/libxcursor/libxi/libwayland/
    wayland-protocols/libxkbcommon/libglib2.0-dev；tray/Wayland 不降档）；
    `aki_apply_warnings` GNU+tsan `-Wno-error=tsan` 豁免（heyaki 先例同款，
    限定编译语言）；tsan 对 eui 栈全图插桩零豁免（DEC-014 机制探针 +
    覆盖声明五条）。**CI 门禁全绿证据未在本会话产生**（不建分支/不推送
    纪律）——随 M5-02 PR 首跑核实，如实登记。
  - **⑪ MinGW 受限（纪律记录）**：Aki CMakePresets 无 MinGW 档（M3-01 起
    w64devkit 受限，configure-only 未达成先例）；EUI-NEO 硬性要求
    GCC≥12 且 static runtime 探测失败即 FATAL（其 CMakeLists.txt:98-103）
    ——MinGW 被 EUI-NEO 阻断属 DEC-005 预期，本项不建立 MinGW preset、
    不冒充验证。
  - **⑫ 全量测试零回归**：debug 与 release 全量 ctest 各 40/40 通过
    （原 38 项语义零损失迁移——skeleton.app_runs/smoke.device_lifecycle/
    recovery.corrupt_db_clean_failure 改由 aki_host_smoke 驱动（同 argv
    契约、同输出标记），+test_host_runtime +test_ui_theme_values 两新项）。
    命令：`ctest --preset debug`（151.5s，100% passed 40/40）、
    `ctest --preset release`（156.3s，100% passed 40/40）。
  - **⑬ 边界 grep（退出-3）**：第一方线程创建（std::thread/jthread/async/
    CreateThread/pthread_create）0 命中（唯一命中为 test_app_state 注释
    行）；ui/+main.cpp 的 app::async/core::network/eui::network 0 命中；
    EUI-NEO include/类型越过 ui/（app/device/conversation/transfer/
    persistence/heyaki）0 命中。
  - **⑭ 设计与决策同步**：设计 §9.1 增补「首帧装配例外」「启动↔关闭配对」
    两条款（M1-08 先行）；[DEC-014](../decisions/DEC-014-eui-tsan-coverage.md)
    落档（Accepted）；DEC-005「验证方式」逐项回填。
  - 限制与补跑条件：CI Linux 四档门禁与 tsan 首跑证据随 M5-02 PR（本会话
    未推送）；Linux 真实 GCC tsan 编译/运行与 UI 运行期 tsan 证据按
    DEC-014 口径归后续会话（本机无 Linux 工具链）；非空库 GUI 装配耗时
    随 M5-08 双端验收复测；GUI 数据根为真实用户目录（%APPDATA%\aki，
    本机 GUI 会话已建立 identity/devices=1——应用正常形态，测试注入经
    HostRuntime 参数/aki_host_smoke argv 承载）。
  - 同步：本里程碑（M5-02 勾选、本记录）、
    [aki_design](../design/aki_design.md) §9.1（两增补条款）、
    [DEC-014](../decisions/DEC-014-eui-tsan-coverage.md)（新建）、
    [DEC-005](../decisions/DEC-005-eui-neo-integration.md)（验证方式回填）、
    总计划（当前状态条目 + 决策表 DEC-014）。

- 2026-09-27（`M5-03` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① 视图模型派生（设计 §9.1）**：`ui/models/view_models.{hpp,cpp}`
    纯函数四域派生——设备列表（DeviceStore × presence + 连接路径摘要入参；
    Store 无逐设备路径字段，摘要取 owner LatestMailbox 最新单值，逐设备
    路径如需扩展先立 DEC——头文件内登记）、会话列表（ConversationStore ×
    最后消息摘要：端点归属过滤 + Store 序倒序取最新，媒体预览带
    "[image] name" 标注）、消息流（MessageStore 按会话端点过滤 + 本地身份
    端点守卫，可见空态）、传输列表（进度 fraction（total==0 → 0 不除零）/
    方向/终态标志）。`aki_ui_models` 为 EUI-NEO 无关独立静态目标（纯
    std/aki 类型；ui/CMakeLists.txt；依赖方向 ui/models → Application，
    RULE-01），测试 exe 直链（DEC-005「测试 exe 不链 eui」落地面）。
  - **② 快照消费面（EXEC-03）**：`ui/models/ui_state_consumer.{hpp,cpp}` —
    `consume_ui_state(owner, watermark, view)` 单次有界消费：快照
    `load_snapshot_newer_than` 水位去重 + 连接路径
    `try_load_connection_path_newer_than` 独立水位（SetConnectionPath 不
    触发 Store 发布，路径推进不依赖快照水位——回填设备视图摘要）；新快照
    为提交点（先落水位再派生，无半更新视图）；槽位忙/无新数据返回 false
    留待下帧（覆盖式语义丢帧不丢状态）。水位与派生结果
    （UiConsumerWatermark/UiStateSnapshot）存页面模型（§9.1「页面持有
    UI 态」）；宿主在 compose 前调用 `HostRuntime::pump_state()`（主线程
    = owner 上下文的有界 drain，无等待无 IO）后再 consume——compose 内
    不等待、不轮询、不做 IO 保持。
  - **③ 跨线程唤醒接线（设计 §9.1）**：`AppStateOwnerOptions::on_publish`
    （`std::function<void()>`，EUI-NEO 无关，RULE-10）——发布点为
    `publish_if_dirty` 的 `snapshot_.publish()` 之后、owner 单写者上下文
    同步调用；异常全捕获计数 `publish_hook_failures`（不中断 drain，
    `publish_hook_calls` 成功计数——RULE-09/EXEC-06 可观测）；经
    `HostRuntime::ensure_assembled(data_root, wake)` 装配参数转发注入；
    GUI 宿主 main.cpp 传包装回调（首次触发留日志 +
    `app::requestUpdate()`），console/测试宿主传计数器。设计 §9.1 消费面
    装配细节先行增补（M1-08：aki_ui_models 目标/consume 水位/on_publish
    注入/UiActions 四点）。
  - **④ UI 出站面（DEC-008）**：`ui/models/ui_actions.{hpp,cpp}` —
    `UiActions` 注入接口（send_text/send_image、传输四接口、发现启停、
    ensure_conversation 绑定面）+ `make_ui_actions` 组合根绑定（直呼
    Manager 公开出站方法，返回值即泵入队 admission，拒绝可见）；页面只持
    UiActions 不持有 Manager/transport 对象（RULE-01/RULE-02）。信任判定
    操作未预建（DeviceManager 现无对应方法——grep trust/reject/revoke
    零命中，归 M5-04 补建，设计 §9.1 登记）。
  - **⑤ main_window 最小接线**：列表栏计数消费展示（"N devices · N
    conversations · N transfers"，派生自最近消费快照）；Devices 页
    Start/Stop Discovery 出站示范按钮（admission 结果写页面模型反馈文案
    ——页面持有 UI 态形态）；内容栏提示 "state consumption wired
    (M5-03)"。
  - **⑥ 测试**：test_ui_models（8 用例 78 断言：四域派生空态/终态/易变
    字段/端点归属；consume 水位去重/重派生/路径独立推进/重试语义；
    on_publish 发布→唤醒调用序——钩子内观察新序列号==发布后序列号、
    无变更 drain 不触发；钩子异常收口；executor 任务内驱动 owner.drain
    调用注入回调——跨线程为机制契约面：owner 上下文可落在 executor 任务
    上，回调于 executor 线程触发（现行管线 drain 仅在主线程
    host_runtime pump/quiesce）——DEC-014 覆盖声明第 2 条：CI ctest
    不跑渲染）+ test_ui_actions
    （1 用例 51 断言：会话/消息/发现/传输四域经 UiActions → Manager 泵 →
    Fake Adapter SPI 全通道 + 入快照断言 + 独立 owner 受控关闭）+
    test_host_runtime 扩展（HostRuntime 装配参数 wake 计数器：quiesce
    发布后触发断言）。
  - **⑦ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 42/40+2
    （151s 量级）；release `cmake --build --preset release --config
    Release` 0 error 0 warning + `ctest --preset release` → 100% passed
    42/42；新二进制直跑 `test_ui_models.exe`（73 断言全过）、
    `test_ui_actions.exe`（51 断言全过）、`test_host_runtime.exe`（67
    断言全过）。GUI 本机会话（Release aki.exe）：aki-run.log 装配 ok
    65ms + `wake: on_publish fired -> app::requestUpdate()` 首触发证据 +
    onShutdown 关闭序完好（8 步 + fully_stopped）；截图
    `build/scratch/aki-m5-03-window.png`（列表栏消费计数 "1 devices ·
    0 conversations · 0 transfers" 可见——消费面装配展示可运行）。
  - 评审修正（2026-09-27）：⑥ 原「executor 任务内调用注入回调」用例仅把
    自构计数 lambda 提交到 executor，从未触发注入的 on_publish（未涉及
    AppStateOwner），证据失实——用例改为 executor 任务内真实驱动
    `owner.drain()`（该任务即本次 drain 的 owner 上下文，主线程不并发
    drain），断言回调于 executor 线程触发且新序列号已可见（73→78 断言）；
    「跨线程」如实限定为机制契约面，非现行调用路径。修正后复验：debug
    全量 `ctest --preset debug` 42/42 + 直跑 `test_ui_models.exe`
    78 断言全过（连续 20 次运行稳定）；上方 ⑦ 的 release 档与 GUI 会话
    证据对应修正前树，未复跑（改动仅单个 console 测试文件）。
  - **⑧ 退出-3 grep（本项扩面）**：自建线程 0；ui/+main.cpp 的
    app::async/core::network/eui::network 0；EUI include/类型越过 ui/ 0；
    ui/ 持 transport 类型（HeyakiAdapter/NodeSession 等）0；ui/ 直写
    Store（submit_update/post_event/drain）0；eui include 仅 main.cpp +
    ui/ 渲染面（models 无 eui——DEC-005 落地实证）。
  - 限制与补跑条件：CI 五档门禁随本工作项 PR 首跑（本会话未推送，如实
    登记）；TSAN 运行期 UI 证据按 DEC-014 口径归本机 Linux（渲染不进 CI，
    RULE-11）；UiActions 的 start_transfer 在无接收端环境仅断言入队
    admission 与 SPI 出站记录，wire 侧推进语义归 M4 既有回环/单测（不
    重复声明）。
  - 同步：本里程碑（M5-03 勾选、本记录）、
    [aki_design](../design/aki_design.md) §9.1（消费面装配细节增补）、
    总计划（当前状态条目）。无新决策记录（on_publish/UiActions 为设计
    §9.1 既有契约的具体化，未越契约边界）。

- 2026-09-27（`M5-04` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① SPI 信任操作扩展（DEC-006 映射 3，设计 §8.1 先行同步）**：出站
    `confirm_pairing(device)`（→ `NodeSession::pair_peer`，scope 冻结
    `{message.send, file.push:inbox}`；提交被拒=会话缺失/非
    pairing_restricted/重复 pending，admission false 可见）与
    `revoke_trust(device)`（→ `revoke_trust_grants`，无有效 grant 时
    false）；入站第 12 方法 `on_pairing_completed(device, success, detail)`
    （Node 上下文回调 → Adapter 有界校验 + sink 投递，EXEC-02；析构中和
    observer）。真实 Adapter（set_pairing_observer 构造登记）与
    FakeHeyakiAdapter（confirm 记录 + `inject_pairing_completed` +
    `queue_pairing_result`/`set_has_valid_grant` 测试配置面）对齐。口令
    处理不进 SPI 签名（DEC-016）。
  - **② DeviceManager 信任三操作 + 配对结果路由**：confirm → wire 面先行
    （pair_peer 提交 admission），信任状态推进经配对结果事件异步落地（不
    乐观改状态——DEC-006 映射 3 冻结顺序）；reject → 纯本地判定（Pending
    → Rejected 无 wire 面）；revoke → wire 撤销全部有效 grant + 本地
    Trusted → Revoked。行改写经快照读行 + 整行 UpsertDevice（M1-06 确认
    语义；快照滞后窗口由用户操作节奏吸收——操作面数据本就来自快照，登记）。
    PairingCompletedWork：success → Trusted、失败 → Rejected（不新增
    AppEvent 主路径类型，同 on_transfer_paused 先例）；未知设备/非法转移
    拒绝可见。
  - **③ DEC-015 逐设备路径落地**：DeviceStore 增
    `DeviceConnectionPathEntry` 向量 + `SetDeviceConnectionPath{device,
    path}`（未知 id 拒绝、同值幂等 no-op 不重复发布）；退役全局
    `SetConnectionPath`/`LatestMailbox`（owner 成员/访问器/统计同步移除，
    grep 残留 0）；初连路径补发（`PeerSessionEvents::on_connected` 增携带
    映射路径，宿主删除 M3-06 的 Lan 硬编码）；断连置 Unknown（DM 断连
    处理同批提交）。设计 §10.1/§8.3/§9.1 同批修订（M1-08）。
  - **④ DEC-016 口令常量 + verifier 真实化**：`kAkiPairingPassword` 冻结
    常量（26 标量 ≥8 策略下限；2026-09-27 收口订正，原文误记 20）；
    `converge_local_initialization` created
    分支以 `create_password_verifier` 生成真实 argon2id verifier（取代
    M3-03 占位假编码串——冻结调研探针实测对任何口令均拒绝）；存量 profile
    处置=删除 db/profile.sqlite 重建（本机 %APPDATA%ki 即存量，GUI/
    smoke 运行不受影响——verifier 仅配对时消费；不静默迁移，登记于
    DEC-016）。确认弹窗无口令输入框（aki_ui_design §3 规格）。
  - **⑤ UiActions + 视图模型扩展**：UiActions 增 confirm_pairing/
    reject_device/revoke_device 绑定（M5-03 make_ui_actions 形态）；
    DeviceView 增逐设备 connection_path（Store join）、
    fingerprint_available（公钥缺失显式不可用态——relay 来源防御）、
    can_confirm/can_reject/can_revoke（§4 转移边派生）。
  - **⑥ Devices 页实体化**：设备行（presence 圆点 Online success/Offline
    subtlest、名称/类型/OS/逐设备路径徽标（caption 中性，Unknown 显
    "--"）、信任语义色徽标（Pending warning/Trusted success/Rejected·
    Revoked destructive/Unknown subtlest）、mono 指纹列）+ Pending 行
    Confirm（弹窗 mono 指纹核对 + Confirm/Reject 按钮）/Reject 按钮 +
    Trusted 行 Revoke 按钮（§4 转移边控制可用性）+ 发现启停按钮与来源
    分期披露（"LAN only (Relay / invite link / manual input are
    registered follow-ups, M3-09)"——如实呈现）。截图
    `build/scratch/aki-m5-04-devices.png`（点击导航切页后实拍：本地身份
    行 hy1_ 56 字符 mono 指纹、Unknown 徽标、发现启停）。
  - **⑦ 测试**：新建 test_device_trust（4 用例 76 断言：配对结果路由
    Pending→Trusted/Rejected + 未知设备 handler 拒绝可见；三操作转移边
    （reject 无 wire 面/revoke SPI 记录/非法转移 updates_rejected/无
    grant revoke 无操作可见）；UiActions 信任通道（泵→Fake SPI→状态）；
    连接→换路→断连→重连全序列路径断言）；test_local_identity 增口令
    往返用例（DEC-016：kAkiPairingPassword↔create↔verify，他串拒绝、
    短串生成被策略拒）；既有测试同步（BridgeSink 等 6 处 sink 增第 12
    方法；path 断言改逐设备；on_connected 签名；StubAdapter 信任方法）。
  - **⑧ 验证（可复现命令与结果）**：`ctest --preset debug` → 100% passed
    43/43（+test_device_trust，test_heyaki_adapter 路径断言重写）；
    release `--config Release` 构建 0 error 0 warning + `ctest --preset
    release` → 100% passed 43/43；GUI 本机会话（Release aki.exe）：装配/
    onShutdown 关闭序完好（aki-run.log），Devices 页点击切页截图归档
    （aki-m5-04-devices.png）；退出-3 grep 七项全 0（自建线程/禁用面/EUI
    越层/ui 持 transport/ui 直写 Store/退役全局路径残留 0/kAkiPairing
    Password 引用 heyaki/adapter 2 文件 + tests 9 文件，均为合法引用）。
    ⑧ 订正（2026-09-27 收口批，第 3 轮评审发现）：退役全局路径注释残留
    实有 2 处（app_state_updates.hpp 重复词组、update_jobs.hpp 退役
    SetConnectionPath 旧口径），本批修正后「残留 0」成立；「仅 heyaki/
    adapter 两文件」为评审修正批（集成测试字面量替换）之前的口径。
  - 评审修正（2026-09-27）：DEC-016「测试同步」两条在原变更集漏执行，
    本日补齐并复验——
    **集成回环口令字面量替换**：8 个集成测试文件 19 处 `pair_peer`
    口令字面量（aki-loopback/ps/rec/img/msg/ra/full/send-pw）中 17 处
    成功面同批替换为 `aki::heyaki::kAkiPairingPassword`（grep 替换后旧
    字面量残留 0）；错误口令
    负例（test_discovery_pairing_loopback）改用显式非匹配值
    aki-invalid-pw-c/-d 并注明理由——真实 verifier 只接受冻结常量，历史
    right/wrong-password 命名在假 verifier 下无区分度（DEC-016 背景
    自认）。替换后 debug/release 全量 ctest 43/43 复验零回归（本机链路
    环境为既有 [skip] 降级路径，本次替换使补跑面语义就绪，不改变当前
    结果）。**created 分支 argon2id 创建耗时实测（DEC-016 影响与风险/
    验证方式③）**：一次性探针 build/scratch/verifier_probe/（.gitignore
    内，不入库）直测生产同参调用 `create_password_verifier(
    kAkiPairingPassword, PasswordHashParameters{})`（local_identity.hpp
    created 分支），Release 构建，本机（Windows 11 工作站/MSVC 14.44）
    连跑 5 次：创建 67/60/58/61/57 ms，正确口令 verify 全 MATCH
    （55-67 ms），encoded 前缀实测 `$argon2id$v=19$m=65536,t=2,p=1$`
    （与 DEC-016 m=64MiB/t=2 披露一致）。复现：`cmake -S
    build/scratch/verifier_probe -B build/scratch/verifier_probe/build
    && cmake --build build/scratch/verifier_probe/build --config
    Release --target verifier_probe &&
    build/scratch/verifier_probe/build/Release/verifier_probe.exe`。
    ④ 的存量 profile（本机 %APPDATA%\aki 走既有分支）声明不变；
    首启动耗时面以本条实测登记，⑧ 的 43/43 数据经复验仍然成立。
  - 收口批（2026-09-27，第 3 轮评审 should-fix 项）：①Fake revoke 去除
    合成 on_pairing_completed（真实 NodeSession revoke 从不触发配对观察
    器，合成事件必然被状态机拒绝、徒增 updates_rejected 噪声）；②集成
    回环 presence/发现断言改谓词内 drain+load 截止轮询（connected 事件
    回调先 submit 后自增计数，单次快照在 owner 应用前抢跑——CI run
    36278949737 asan 实测；本机防火墙降级路径未实跑，由 CI 实跑验证）；
    ③test_app_managers 路由用例标题 eleven→twelve；④④⑧ 记录数字订正
    （见上）；⑤UpsertDevice 整行替换覆写隐患兑现登记（「风险与阻塞」）；
    ⑥退役 SetConnectionPath 注释残留 2 处清理；⑦test_peer_sessions_
    loopback 持久化载体 :memory:→<data_root>/db/aki.db3（CI 第 5 轮
    run 36295393398 asan/tsan 同点暴露：轮询修复使测试首次跑通全链后，
    尾部重启恢复断言得 0 行——:memory: 载体无 schema、设备行从未落盘，
    M3-06 起从未实跑段，按 §11.1 ②③ 恢复组合先行修复）。debug 全量
    ctest 复验。
  - **⑨ 如实降级（M3-09 纪律）**：双端配对→信任全链路（pairing
    restricted → pair_peer → observer 结果 → Trusted 端到端）受本机防火墙
    拦截 TLS 入站限制未执行——网络无关半边全部验证（SPI 路由/状态机转移
    边/UiActions 通道/verifier 往返/Fake 注入路径）；补跑条件沿 M3-09
    登记（防火墙放行入站 TCP / LAN 双端真机，M5-08 双端验收同批）。
  - 限制与补跑条件：CI 五档门禁随本工作项 PR 首跑（本会话未推送）；存量
    profile 的配对在 M5-07 设置面前不可用（DEC-016 处置：删除重建）；
    fingerprint 渲染为 Windows CascadiaMono 实拍证据，Linux mono 路径随
    CI/本机 Linux 会话确认（DEC-014 口径）。
  - 同步：本里程碑（M5-04 勾选、本记录）、
    [aki_design](../design/aki_design.md)（§4 指纹合并展示、§8.1 信任
    SPI/第 12 sink 方法、§8.3 路由表、§9.1 消费面、§10.1 comm 映射）、
    [aki_ui_design](../design/aki_ui_design.md) §3（指纹=DeviceId 规范串、
    弹窗无口令框）、[DEC-006](../decisions/DEC-006-heyaki-api-contract.md)
    映射 3（展示形式增补）、[DEC-015](../decisions/DEC-015-per-device-
    connection-path.md)、[DEC-016](../decisions/DEC-016-pairing-password-
    verifier.md)（均新建 Accepted）、总计划（当前状态条目 + 决策表）。

- 2026-09-27（`M5-05` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① 会话列表（SCOPE-05）**：`ui/pages/conversations_page.{hpp,cpp}`
    落地列表栏会话行（复用 M5-03 `derive_conversation_views` 派生；扩展
    `LastMessageSummary.outbound`——投递徽标仅己方最后消息展示，§3）；
    行内绝对排版（名称/预览/时间/投递徽标/Disconnected·Archived 徽标）+
    透明点击面；Rejected/Revoked 远端行无点击面（§3 会话入口禁用）；
    New chat 弹窗 = Trusted 设备选择面（`UiActions::ensure_conversation`，
    幂等）。
  - **② 聊天窗口（SCOPE-06/07）**：头部（对端名 + 路径 caption 徽标 +
    会话态 + 信任语义色徽标）；§3 断连横幅（Disconnected → `warning` 底
    + 三角叹号 + 「连接断开，等待恢复」）；消息历史 = scrollview + 气泡
    card（wrapContentHeight）+ text(wrap)（M5-01 复核结论「卡片自绘 +
    scrollview」实测成立——变高列由内容列 wrapContent 度量，virtuallist
    固定行高模型不适用处按组合策略承接，不改 pinned 依赖）；气泡左右
    归属 = 己方 accent 右侧 / 对方 card+边框左侧（§4 surface 层级区分）；
    投递态图标（§2.6 码点：Queued 时钟/Sending paper-plane/Sent 单勾/
    Delivered 双勾 `success`/Failed 叹号 `destructive`，仅己方消息）；
    系统消息居中 caption。
  - **③ 会话内文件卡片（SCOPE-08）**：Image/Video/File 载荷渲染为
    card+progress+button 卡片（§4 映射，M5-06 Transfers 页复用同一形态
    与语义色）：方向箭头（§2.6 f175/f176）+ 文件名 + 大小/mime + 进度条
    + 态文案（Transferring `brand`/Paused `warning`/Failed `destructive`/
    Completed `success`/其余中性，§3）；TransferStore 消费侧 join（§6.1②
    按 TransferId；进度 fraction total==0 → 0 不除零）；无传输行 →
    `warning` "no transfer row (single-side arrival)" 兜底态（DEC-010/
    DEC-013 边角，不猜进度）；图片卡片带 Preview 按钮 → 预览弹窗
    （image+dialog，§4；出站本地路径已知时渲染 image 元素，接收侧/无
    路径显式 metadata 态——不以占位图冒充）。
  - **④ 文本/图片发送链路**：输入区 input+button（onEnter/发送按钮 →
    `send_draft`；图片按钮 → `eui::platform::openFileDialog` 只读选取
    （M5-01 复核满足）+ 扩展名→mime 映射 + `std::filesystem::file_size`
    → `send_image`）——对话框与文件 stat 仅在点击回调上下文（主线程
    事件处理）执行，compose 三不纪律不破。**发送面页面形改造（M5-05）**：
    (a) wire id 生成收敛 `heyaki/adapter/wire_ids.hpp`（local_identity.hpp
    先例：单头 inline 实现；`new_message_id`/`new_transfer_id` = 16 随机
    字节全零重抽 → `heyaki::to_string` 规范串；`NodeSession::
    new_transfer_id` 改为委托同入口，生成入口单一——§6.1 同步）；(b)
    `UiActions` 增 `new_message_id`/`new_transfer_id` 生成器绑定（页面不
    携带 wire 编码知识，RULE-10）+ `send_image` 签名改 `source_path`、
    绑定改编排层 `send_image_message_with_hash`（hash-first：先传输准入，
    消息等 stored_sha256 完成后经 TM 泵延续发出，DEC-010/DEC-011）。
    `send_text` 页面形态不变（id 经生成器取得后传入）。
  - **⑤ 唤醒缺口修复（本机 GUI 实测发现，§9.1 同步）**：现行管线
    Manager handler 跑在 executor 任务上、publish 只发生在主线程 drain
    （`HostRuntime::pump_state`）——M5-03 的 on_publish 唤醒仅在发布后
    触发，主循环无从得知「有待 drain 的更新」：GUI 发送文本后 admission
    反馈可见（点击自身触发重组）而消息行滞留至下一次输入事件。修复：
    `AppStateOwnerOptions` 增 `on_update_submitted`（`submit_update`/
    `submit_update_for` 受理成功后于提交者上下文同步调用——唯一的跨线程
    状态生产点；拒绝不触发；异常全捕获计数 `submit_hook_failures`，原子
    计数——多线程提交者）；`HostRuntime` 注入同一 wake 回调。GUI 复测：
    发送后气泡当帧可见。设计 §9.1 快照消费与唤醒条款先行修订（M1-08）。
  - **⑥ 视图模型扩展（`ui/models/view_models`）**：`MessageView` +
    `derive_message_views`（端点归属守卫、方向、投递态、文本/媒体载荷
    语义、TransferStore join 进度/状态、无传输行兜底态）；消息预算 4096
    内有界派生。页面模型 `ConversationsPageModel`：选中会话/输入草稿/
    消息视图派生缓存（水位×选中去重重派生）/滚动代数/弹窗 open 态/
    出站图片源路径登记（容量 64，RULE-09）。
  - **⑦ 滚动/弹窗契约（探针定形）**：pinned scrollview 运行期滚动状态
    按元素 id 持有（首次构建播种 offset、其后运行期所有）——「回到底部」
    以 `history_scroll_gen` 代数进位切换 scrollview id 表达（选中切换/
    新消息入流 +1；投递态原位更新不进位，不打断阅读位置）；用户滚动
    位置在两次代数进位之间由运行期保持；measure cache 以
    代数:条数:末条 id 为 contentKey。气泡 x 归属须在行内 stack 绝对定位
    （直接挂内容列会被列布局拉回左缘——本机实测）；dialog 背板/居中
    须显式 `.screen(窗口宽高)`（默认 800x600——实测）；背板关闭请求经
    `onOpenChange` 回写页面持有 open 态（单向数据流对称面）。
  - **⑧ 测试**：test_ui_models 扩展（MessageView 派生：方向/媒体 join
    进度 0.5/无传输行兜底/端点守卫；LastMessageSummary.outbound；新
    on_update_submitted 用例——受理触发/拒绝不触发/钩子异常全捕获计数
    /executor 任务内受理跨线程触发，共 116 断言 10 用例，直跑通过）；
    test_ui_actions 更新+扩展（send_image 新签名路由 hash-first 编排：
    TM 准入 + Fake 传输命令 + 无 IO 承载空 hash 延续不发消息（§6.1②，
    全链路归 test_transfer_send_path）；wire id 生成用例——
    parse_message_id/parse_transfer_id 规范性 + 唯一性，共 65 断言
    2 用例，直跑通过）；既有 43 项语义零损失。
  - **⑨ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 43/43
    （179.6s）；release `cmake --build --preset release --config
    Release` 0 error 0 warning + `ctest --preset release` → 100%
    passed 43/43（170.5s）；直跑 test_ui_models.exe（116 断言）/
    test_ui_actions.exe（65 断言）/test_host_runtime.exe（67 断言）
    全过。
  - **⑩ GUI 本机会话（RULE-11 归档）**：Release aki.exe，GUI 数据根经
    `APPDATA` 注入 scratch 剖面 `build/scratch/m5-profile/`（真实
    `%APPDATA%ki` 未触碰）；演示数据经 scratch 种子探针
    `build/scratch/m5-demo-seed/`（gitignored，standalone CMake 直链
    Release 树 aki_persistence 静态库，不入产品构建图——M5-01 探针
    先例）写入受信对端/会话/消息/传输行。GUI 实测：会话列表（最后消息
    摘要/方向投递徽标/时间）、聊天窗口（气泡左右归属 + 投递语义色 +
    文件卡片 Completed `success` 100% / 恢复降级 Paused `warning` 72%
    ——DEC-013 重启降级边在 GUI 可见）、文本发送实测（real Adapter 无
    会话 → admission `hym1_` 规范 id 可见 + 消息行 Failed 徽标 + 列表
    摘要实时更新——发送/唤醒/消费/派生全链）、图片预览弹窗（无本地
    路径 metadata 态 + DEC-011 登记的「消息行重启重建 stored_sha256
    为空」在 GUI 呈现）、New chat 弹窗、onShutdown 关闭序完好（8 步 +
    fully_stopped=1 + workers 2/2 + db_drained=1）。截图 ×5 +
    aki-run-m5-05-final.log 归档 `build/scratch/`（会话内文件卡片
    Completed/Paused、文本发送 Failed 徽标、预览弹窗、New chat 弹窗）。
  - **⑪ 退出-3 grep（本项扩面，最终代码态）**：第一方线程创建 0；
    ui/+main.cpp 的 app::async/core::network/eui::network 0；EUI-NEO
    include/类型越过 ui/ 0；ui/ 直写 Store（submit_update/post_event/
    drain_updates）0；ui/ 持 transport 对象 0（ui_actions.hpp 注释提及
    NodeSession 为文档性引用）；eui include 仅 main.cpp + ui/pages 渲染
    面（models 无 eui）；test_ui_models.exe dumpbin 对
    eui::/components::/glfw 符号 0 命中（DEC-005 维持）。
  - 限制与补跑条件（沿 M3-09/M4-07/M5-04 ⑨ 降级纪律）：
    (a) 双端真机的文本 Delivered 回报、图片 wire 面（codec/文件本体）、
    断连→恢复横幅转换、传输暂停/恢复/取消操作面未在本会话执行——
    防火墙拦截入站 TCP / 无 LAN 双端；网络无关半边（视图派生、UiActions
    路由、hash-first 闸门、真实 Adapter 无会话拒绝路径）已验证；补跑归
    M5-08 双端验收同批。(b) GUI 会话中的图片发送按钮点击 → 原生文件
    对话框 → hash-first 闸门拒绝路径（无会话 → 传输行 Failed、
    message_id NULL、canonical `hyt1_` id）在 GUI 会话期间实际产生
    （DB 实证；点击来源无法精确归因——本机为使用者台面机，窗口自动化
    期间可能存在人工交互）——该行语义与闸门契约一致，作为 GUI 会话
    副产品如实登记。(c) 文件卡片 Failed/Cancelled 态、断连横幅、
    Archived 徽标为条件分支，本会话不可达（无真实断连）——Failed 态由
    test_ui_models 派生断言承载，横幅渲染随 M5-08。(d) 会话行/弹窗
    设备选择超视口滚动、图片本体（image 元素本地路径态）像素级复核随
    M5-06+/M5-08。(e) CI 五档门禁随本工作项 PR 首跑（本会话未推送，
    如实登记）；TSAN 运行期 UI 证据按 DEC-014 口径归本机 Linux。
  - 同步：本里程碑（M5-05 勾选、本记录）、
    [aki_design](../design/aki_design.md) §6.1（wire id 生成入口收敛）、
    §9.1（UiActions 发送面形态 + on_update_submitted 唤醒条款）、
    [aki_ui_design](../design/aki_ui_design.md) §3（无传输行兜底态）、
    §5（M5-05 落地记录）、总计划（当前状态条目）。无新决策记录：
    wire id 收敛/受理唤醒为 §6.1/§9.1 既有契约的修订与具体化（M1-08
    先改设计后合代码），未越契约边界。

- 2026-09-27（`M5-06` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① Transfers 页实体化（SCOPE-08）**：`ui/pages/transfers_page.{hpp,cpp}`
    —— 传输行（固定高 88，scrollview 容纳；行数预算 256，RULE-09）=
    文件卡片本体（方向箭头/文件名/大小·mime/进度条/状态文案，§4 共享
    形态）+ 方向·对端 caption（快照设备 join）+ 操作按钮纵排（状态门控）；
    页头计数 + 页脚登记披露 + 空态文案。无独立页面模型（无弹窗/草稿/
    滚动代数需求——滚动位置由 pinned scrollview 运行期状态保持；操作
    反馈经共享 `MainWindowModel::last_action_feedback`，RULE-09）。
  - **② 共享传输卡片抽取（§4 复用契约兑现）**：`ui/components/
    transfer_card.{hpp,cpp}`（命名空间 `aki::ui::widgets`——避让 EUI
    `::components` 限定名，aki::ui 内 `components::` 解析冲突实测规避）；
    M5-05 ③ 会话内文件卡片本体原样迁移（方向/文件名/大小·mime/进度/
    状态 + `format_bytes`/`transfer_state_color`（§3 语义色）），会话页
    气泡卡片改消费共享体（形状与语义色不变——M5-05 前瞻条款落地）；
    `TransferView` 增 `mime_type` 透传（行侧真实媒体标注，空 → 卡片
    `application/octet-stream` 兜底）。
  - **③ 操作面与 DEC-013⑥ UI 触达**：Pause/Resume/Cancel 按
    `TransferView::can_pause/can_resume/can_cancel` 门控（§7 固定边纯派生：
    Transferring→Pause、Paused→Resume、非终态→Cancel、终态全不可用——
    Queued→Paused/Negotiating→Paused 为 DEC-013 重启降级边，非用户动作
    面）；操作经 `UiActions` 既有传输三接口（M5-03 建面），返回值即泵
    入队 admission、拒绝经反馈行可见（RULE-09）。**Paused 行 Cancel =
    DEC-013⑥ 无会话行 `cancel_transfer` 直接终态写入的 UI 触达**（M4
    唯一出口）：GUI 实测 policy.pt Paused·72% 行经 Cancel 即转
    `Cancelled · 72%`（中性进度条、按钮集收起、反馈行
    "cancel admitted (hyt1_seedpolicy72pct001)"——发送/唤醒/消费/派生/
    渲染全链 + owner 状态机终态校验；该反馈行文案在归档截图底部与页脚
    登记披露行叠印不可辨读——页脚分行修正与复验见⑩，终态转换与全链
    断言不受影响）。
  - **④ 孤儿接收行 re-push 触发面登记（M4-05/06 移交项）**：接收行恢复
    按 DEC-013② 为 wire 事件驱动（对端重推/committed-窗口重发收敛），
    Aki 侧无 re-push 请求出站接口——本项以页脚登记披露呈现
    （"orphan receive rows: re-push trigger is a registered follow-up
    (M4 handover)"），不冒充可用动作；功能化触发条件=出现 re-push 出站
    接口决策（wire 面需先立 DEC）。
  - **⑤ 接收根残留 GC 议题处置（DEC-012③ 登记项；两向决策点）**：
    **显式延后**。理由：收敛路径已存在且运行——Completed 作业同源删除
    （DEC-012①）、Failed/Cancelled 触发 discard 幂等删除 + heyaki 失败/
    取消自清其接收侧残留（DEC-012③）、Paused 行恢复后经作业幂等重跑
    收敛（DEC-013②）；实现即时 GC 需扩 M2-06 启动清扫面/新增清扫作业
    （M3/M4 既有语义面），在残留无实证累积前属无验收对象的预防性复杂度。
    **实现触发条件（登记，任一出现即立项）**：(a) M5-08 双端验收实证接收
    根残留累积（heyaki 自清不成立/部分传输残片堆积）；(b) 接收根容量预算
    触顶需求出现；(c) 产品引入用户清理/删除传输残留动作需求。处置结论
    已同步 aki_design §11.1（清扫条目，M1-08 先行）。
  - **⑥ 测试**：test_ui_models 扩展（TransferView 操作可用性派生六态
    断言：Transferring/Completed/Paused/Queued/Failed 的
    can_pause/can_resume/can_cancel 组合 + mime 透传/兜底契约，135 断言
    10 用例直跑通过）；test_ui_actions 扩展（resume 入口经绑定面 → Fake
    Resume 命令记录——传输控制通道 Start/Pause/Resume/Cancel 全序锁定，
    67 断言 2 用例直跑通过）；无新增并发路径（DOD-02 六项无新增面——
    既有宿主生命周期覆盖维持，test_host_runtime 67 断言）。
  - **⑦ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 43/43；
    release `cmake --build --preset release --config Release` 0 error
    0 warning（一次中途链接失败复测：aki.exe 被在跑 GUI 占用——关窗后
    全量重建通过，非代码缺陷）+ `ctest --preset release` → 100% passed
    43/43；直跑 test_ui_models.exe（135 断言）/test_ui_actions.exe
    （67 断言）全过。
  - **⑧ GUI 本机会话（RULE-11 归档）**：Release aki.exe，GUI 数据根经
    `APPDATA` 注入同一 scratch 剖面（真实 `%APPDATA%ki` 未触碰）；
    演示行经 scratch 种子探针增补（入站 report-q2.pdf Paused·45%——
    DEC-013 重启降级形态）。GUI 实测：四态行同屏（Completed `success`
    100% / Paused `warning` 72%·45% / Failed `destructive` / 经 UI 操作
    产生的 Cancelled 中性）、终态行按钮集收起、mime 真实标注
    （image/png、application/pdf）、DEC-013⑥ 取消全链（见 ③）、页脚
    登记披露、onShutdown 关闭序完好（8 步 + fully_stopped=1 +
    workers 2/2 + db_drained=1）。截图 ×2
    （aki-m5-06-transfers.png / aki-m5-06-cancel-clicked.png）+
    aki-run-m5-06-final.log 归档 `build/scratch/`。
  - **⑨ 退出-3 grep（本项扩面，最终代码态）**：第一方线程创建 0；
    ui/+main.cpp 的 app::async/core::network/eui::network 0；EUI-NEO
    include/类型越过 ui/ 0；ui/ 直写 Store（submit_update/post_event/
    drain_updates）0；ui/ 持 transport 对象 0；eui include 仅 main.cpp +
    ui/ 渲染面（ui/components 为 aki_ui 目标内单元，models 无 eui）；
    test_ui_models.exe dumpbin 对 eui::/components::/glfw 符号 0 命中。
  - **⑩ 页脚分行修正（2026-09-27 独立评审发现；同日修复并复验）**：
    归档证据 aki-m5-06-cancel-clicked.png 底部反馈文字 "cancel admitted
    (hyt1_seedpolicy72pct001)" 与页脚登记披露行叠印不可辨读——Transfers
    页经 main_window 以 y=0 全高调用，披露行原锚点（height-caption-
    section）与跨页反馈行完全重合，③ 与工作项摘要所记「admission 反馈
    可见」恰在拒绝场景不成立（渲染正确性缺陷）。修复：transfers_page
    披露行上移一行（caption 行高 + tiny 间距），列表底预留同步扩为两行
    页脚，反馈行保持 main_window 既有锚点不动。复验：debug 全量
    `ctest --test-dir build/debug --preset debug` → 100% passed 43/43
    零回归；GUI 本机会话（debug aki.exe，默认 %APPDATA% 数据根——M5-06
    原 scratch 剖面未留存、传输行不可复现，以跨页 sticky 反馈路径复验
    同一几何锚点）：Devices 页 Start Discovery admission 反馈置位后切
    Files 页，反馈行与披露行两行并存、各自可辨读
    （aki-fix-footer-1-nofeedback.png / aki-fix-footer-3-feedback-devices.png /
    aki-fix-footer-4-files-split.png + 页脚裁切，归档 `build/scratch/`；
    onShutdown 关窗退出）。补跑条件：传输行 Pause/Resume/Cancel 操作
    反馈与披露行同屏截图随 M5-08 双端验收补跑（两行行位由固定锚点派生，
    与列表行数无关；非空列表场景底部预留已同步扩为两行页脚）。
  - 限制与补跑条件（沿 M3-09/M4-07 降级纪律）：(a) Pause/Resume 的 wire
    侧效果（对端暂停确认/断点续传推进）需真实双端——本会话仅断言入队
    admission 通道与 UI 门控派生，wire 语义归 M4-05 既有单测与 M5-08 双端
    验收；(b) Cancelled/Failed 行的 .part discard 作业语义归 M4 既有单测，
    本页渲染不重复断言；(c) CI 五档门禁随本工作项 PR 首跑（本会话未推送，
    如实登记）；TSAN 运行期 UI 证据按 DEC-014 口径归本机 Linux。
  - 同步：本里程碑（M5-06 勾选、本记录）、
    [aki_design](../design/aki_design.md) §11.1（接收根 GC 显式延后 +
    触发条件登记，M1-08 先行）、[aki_ui_design](../design/aki_ui_design.md)
    §5（M5-06 落地记录：共享组件抽取 + 操作面门控/披露形态）、总计划
    （当前状态条目）。无新决策记录（GC 延后为 DEC-012③ 登记议题的两向
    处置之一，触发条件登记于本记录并被 aki_design §11.1 引用）。

- 2026-09-27（`M5-07` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① Settings 页实体化（SCOPE-12）**：`ui/pages/settings_page.{hpp,cpp}`
    —— 主题三选 `segmented`（§4 Settings 页组件映射；三段
    Follow system/Light/Dark，field 36 高，selection=页面持有 UI 态
    `MainWindowModel::theme_setting`）经既有 `akiTheme()/
    akiSemanticColors()` 装配面即时生效（§9.1 覆写清单与
    aki_theme_values 回归对照不变——仅档位选择；GUI 深浅两档全壳渲染
    对照实测）；最小设置项只读展示：数据目录（`HostRuntime::data_root()`
    装配面，std::string）+ 本地设备 id（快照
    `UiStateSnapshot::local_device`），mono caption（§2.1 mono 用于
    路径/技术内容）。
  - **② DEC-005 缺口处置 = 平台层查询路径**：`app/lifecycle/system_theme.
    {hpp,cpp}` 平台条件编译单元（persistence/storage/data_root.cpp 先例：
    平台分支隔离、公开面仅 std/aki 枚举，RULE-10）——Windows 查询
    `HKCU\...\Themes\Personalize\AppsUseLightTheme`（有界单次注册表
    读取，RegGetValueA，advapi32 仅 WIN32 链接）；缺失/类型不符/读失败/
    非 Windows 平台一律 `Unknown` → `resolve_effective_theme` 回落
    Light（不猜测），页内披露平台支持面（"unsupported platforms fall
    back to Light"）。DEC-005 冻结时登记的「无系统主题检测 API」缺口按
    其预设的平台层处置路径收口（里程碑风险节同步）。
  - **③ 主题三选语义（§9.1 Settings 页装配条款，M1-08 先行同步）**：
    `ThemeSetting = FollowSystem/Light/Dark` 页面持有；Light/Dark 直取、
    FollowSystem 即时重查（点击回调上下文的有界读取——非 compose 树内）；
    **主题选择为会话级**——不跨启动持久化（schema v1 无设置表，扩表属
    公开契约变更须先立决策，不私自扩表；页内披露 + 本记录登记，后续项）；
    启动初值 = FollowSystem 解析一次（组合根首帧装配期；GUI 日志证据
    "settings: theme follow system -> light (system=queried), data
    directory: ..."）。
  - **④ ThemeMode 迁出 + 解析状态机可测面**：`ThemeMode` 自
    aki_theme.hpp 迁至 EUI-NEO 无关的 `ui/theme/theme_mode.hpp`（连同
    `ThemeSetting`/`SystemTheme` 与 `resolve_effective_theme` constexpr
    纯函数——FollowSystem+Unknown 回落 Light 等），aki_theme.hpp 保留
    EUI 装配面（aki_ui_design §2 映射不受影响）；akiTheme()/语义色消费方
    零改动。
  - **⑤ 测试**：test_ui_theme_values 增「主题三选解析状态机」用例
    （Light/Dark 直取、FollowSystem×{Dark,Light,Unknown} 三路径——
    Unknown 回落 Light 断言；62 断言 7 用例直跑通过）；无新增并发路径
    （系统查询为点击回调/首帧装配内的有界同步读取，DOD-02 无新增面，
    宿主生命周期覆盖维持）。
  - **⑥ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 43/43；
    release `cmake --build --preset release --config Release` 0 error
    0 warning + `ctest --preset release` → 100% passed 43/43（最终代码
    态复跑）；直跑 test_ui_theme_values.exe（62 断言）全过。
  - **⑦ GUI 本机会话（RULE-11 归档）**：Release aki.exe，GUI 数据根经
    `APPDATA` 注入同一 scratch 剖面（真实 `%APPDATA%ki` 未触碰）；
    深浅两档渲染对照截图 ×3（aki-m5-07-settings.png 跟随系统选中
    light、aki-m5-07-dark.png Dark 选中全壳深色、aki-m5-07-light.png
    Light 选中全壳浅色——切换即时生效，requestUpdate 唤醒拾取）+
    aki-run-m5-07-final.log（settings: theme follow system -> light
    (system=queried) 启动解析证据 + onShutdown 8 步关闭序）归档
    `build/scratch/`。
  - **⑧ 退出-3 grep（本项扩面，最终代码态）**：第一方线程创建 0；
    ui/+main.cpp 的 app::async/core::network/eui::network 0；EUI-NEO
    include/类型越过 ui/ 0；ui/ 直写 Store 0；ui/ 持 transport 对象 0；
    **system_theme 公开头无平台类型**（0 命中——平台 API 封死在
    system_theme.cpp 条件编译分支内，RULE-10）；eui include 仅 main.cpp
    + ui/ 渲染面。
  - 限制与补跑条件：(a) 主题选择跨启动持久化未实现（schema v1 无设置
    表；扩表须先立决策登记——本项如实降级为会话级并页内披露，触发条件
    = M5-08 后产品确认持久化需求）；(b) 非 Windows 平台的系统主题查询
    返回 Unknown（XDG portal 需 dbus 依赖，不引入——DEC-005 供应链
    纪律），回落 Light 页内披露；跨平台扩展出现需求时再立决策；(c) CI
    五档门禁随本工作项 PR 首跑（本会话未推送，如实登记）；深色档在
    Linux/GPU 栈的渲染复核按 DEC-014 口径归本机 Linux 会话。
  - 同步：本里程碑（M5-07 勾选、本记录、风险节处置收口）、
    [aki_design](../design/aki_design.md) §9.1（Settings 页与主题三选
    装配条款）、[aki_ui_design](../design/aki_ui_design.md) §5（M5-07
    落地记录）、总计划（当前状态条目）。无新决策记录（平台查询为
    DEC-005 缺口的预设处置路径之一，条款级同步；设置持久化留待决策）。

- 2026-09-27（`M5-08` 完成（验收归集，无产品代码变更）；Windows 11 工作
  站（桌面会话）/ MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0；负责人：
  Linductor）：
  - **① 验收基线复跑**：工作树干净（`git status` 0 项）；debug 全量
    `ctest --preset debug` → 100% passed 43/43；release `ctest --preset
    release` → 100% passed 43/43（无代码变更基线确认）。
  - **② MVP 清单归档（设计第 15 节 + `SCOPE-01`~`SCOPE-12`，两态标注：
    「已验证」/「本机已验证、双端待补跑」——不冒充已验证，工程规范 §4
    规则 3/4）**：
    - `SCOPE-01` 设备身份（Heyaki 密码学身份、无账户层）：**已验证**——
      M3-03（test_local_identity：身份跨重启稳定、公钥逐字节一致；
      aki_host_smoke 重启恢复断言）；DEC-016 口令 verifier 真实化
      （M5-04）；GUI：Settings 本地设备 id mono 展示
      （build/scratch/aki-m5-07-settings.png）。
    - `SCOPE-02` 设备发现（LAN/已知设备记录）：**本机已验证（发现 SPI/
      启停/事件字段）、去重/不重放未测、双端待补跑**——已验证锚点：
      Adapter 发现 SPI/注入→快照/事件面（test_heyaki_adapter，Fake
      注入）；真实 LAN 管道启停与 discovered 事件字段（method=LAN、对端
      公钥、Unknown 态、stop 后零新增——test_discovery_pairing_loopback，
      本机经 [skip] 受控退出点仅保发现/停止断言）；GUI 发现启停实拍
      build/scratch/aki-m5-08-discovery.png "start_discovery admitted"。
      **未测（2026-09-27 评审修正：原锚点「lan_discovery 单测」不存在
      ——ctest 43 项无该目标，tests/ 无直接测 LanDiscoveryPipeline 的
      单测，GUI 单次渲染亦不构成去重观察）**：lan_discovery.hpp:94-98
      的 trusted 跳过（已知设备记录不重放 discovered，§8.1）与 seen_
      去重（重复端点不重复合成）语义无任何测试断言。补测条件：新增
      LanDiscovery 管道单测（合成 endpoints 含 trusted/重复项，断言不
      重放/不重合成），随双端补跑一并执行；Relay/邀请链接/手动输入为
      M3-09 登记分期（GUI 已披露），双端真实发现（另一台 Aki 设备广播/
      被广播）随补跑。
    - `SCOPE-03` 信任建立（Unknown→Pending→Trusted/Rejected/Revoked +
      指纹确认）：**网络无关半边已验证、GUI 确认弹窗被缺陷阻塞、双端
      待补跑**——状态机（test_trust_state）+ 信任三操作 SPI/路由/转移边
      （test_device_trust 76 断言）+ UiActions 通道（test_ui_actions）；
      **缺陷登记（本轮验收发现）**：Devices 页 Pending 行 Confirm 按钮
      点击无响应（弹窗不出现、无任何状态变化；复现 2 个全新会话各 ≥2
      次点击 100% 复现；同行 Reject/邻行 Revoke/导航/其他页按钮均正常
      ——证据 build/scratch/aki-m5-08-confirm-dialog.png + 本会话操作
      序列日志）；伴随一次疑似错乱触发（后证实为自动化窗口坐标漂移误点
      alpha 行 Revoke 所致——该行点击本身工作正常，撤销嫌疑）。
      **BUG-20260927-001 修复闭环（2026-09-27 同日）**：根因=Aki 页面
      代码缺陷——确认弹窗 builder 链缺尾部 `.build()`（DialogBuilder 仅
      在 build() 中创建元素，缺失时弹窗根本不进入 UI 树，点击自然无任何
      效果；非 pinned 缺陷、无上游依赖）；修复=补 `.build()` + 同批补
      `.screen(width,height)`（背板覆盖窗口，原默认 800x600 不覆盖全窗）
      /`.theme(tokens)`（跟随浅深档）/`.onOpenChange`（背板点击关闭
      回写页面 open 态，M5-05 弹窗同款；无 Escape 路径，见复测修正注）；
      复测=登记复现序列 2 个全新
      会话 ×≥2 次点击不再复现（弹窗即现：mono 指纹、无口令框、Confirm
      pairing 提交 admission 反馈可见、Cancel 关闭、背板点击关闭均实测，
      截图 build/scratch/aki-bug001-fix-dialog.png/-confirmed.png/
      -dialog2.png/-cancelled.png + aki-run-bug001-fix.log）；回归=
      Reject/Revoke/导航正常 + debug/release 全量 ctest 43/43；本条 GUI
      证据以此回填，双端 pair_peer wire 面仍随补跑。
    - `SCOPE-04` 设备列表（名称/类型/OS/连接方式/在线状态）：**已验证**
      ——M5-04（DEC-015 逐设备路径 test_device_trust + GUI 设备行/
      徽标/指纹列 build/scratch/aki-m5-04-devices.png、
      aki-m5-08-discovery.png 三行三信任态同屏）。
    - `SCOPE-05` 一对一 Conversation（路径无关模型 + List）：**已验证**
      ——会话模型 M1/M3（test_conversation_state、DEC-009 归属约定单测
      test_ui_models）；GUI 会话列表（最后消息摘要/方向/投递徽标，
      build/scratch/aki-m5-05-chat.png、aki-m5-08-discovery.png 左栏）
      + New chat 建会话入口（aki-m5-05-newchat.png）。
    - `SCOPE-06` 一对一文本消息（发送/接收/送达状态）：**本机已验证、
      双端待补跑**——发送路由/UI（test_ui_actions、GUI 实测：canonical
      hym1_ admission + Failed 徽标 + 列表实时更新，
      aki-m5-05-sent.png）；接收侧消息行（test_app_managers 收到路由）；
      双端 Delivered 回报/ack 链路随补跑（M3-07 先例降级）。
    - `SCOPE-07` 图片消息：**本机已验证、双端待补跑**——codec 往返/拒收
      （test_image_payload_codec）、SPI/Manager/编排闸门
      （test_heyaki_adapter、test_transfer_send_path、M4-03/06）；GUI
      发起面（文件对话框只读选取→hash-first 编排）M5-05 实测（闸门
      拒绝路径 GUI 会话 DB 实证）；双端 aki.image wire 面 + 对端展示随
      补跑。
    - `SCOPE-08` 文件传输（独立 Session/进度/暂停/取消 + Transfers 页）：
      **本机已验证、双端待补跑**——发送承载/接收合并/控制面/重启处置
      （test_transfer_send_path/receive_path/recovery/state，DEC-011/
      012/013，M4-02~06）；GUI Transfers 页四态行同屏 + DEC-013⑥ 无
      会话行取消直接终态实测（aki-m5-06-transfers.png、
      aki-m5-06-cancel-clicked.png）；双端 wire 侧暂停/恢复对端效果随
      补跑。
    - `SCOPE-09` 本地持久化（重启恢复）：**已验证**——M2-07/08
      （test_persistence_*、restart 恢复用例、 aki_host_smoke
      smoke.device_lifecycle）；GUI scratch 剖面跨会话恢复多次实测
      （M5-05/06/08 各次启动 devices/conversations/messages/transfers
      计数恢复 + DEC-013 非终态降级 Paused 行 GUI 可见，
      aki-m5-06-transfers.png）。
    - `SCOPE-10` Presence 与连接路径（LAN/P2P/Relay）：**状态面已验证、
      实时变化待补跑**——管道/映射/逐设备路径（test_peer_sessions_
      pipeline、DEC-015 test_device_trust 路径序列）；GUI 徽标展示
      （aki-m5-04-devices.png，未连接路径 "--"）；真实 presence Online/
      路径切换需双端会话（补跑；Relay 路径依赖 relay 接入登记项）。
    - `SCOPE-11` 断线恢复：**网络无关半边已验证、双端待补跑**——
      reconnect_loop 重连协调器单测（test_reconnect_loop 7 用例，
      EXEC-05 可中断切片/预算/取消）+ 会话模型路径无关（RULE-06 单测）；
      close_lan 合成断开回环与真实断连→恢复横幅转换（§3 warning 横幅
      已实现、条件分支 GUI 未触发——如实登记）随补跑。
    - `SCOPE-12` EUI-NEO 主窗口与基础主题：**已验证**——M5-02（三栏壳+
      四页导航 aki-m5-02-window.png、主题覆写双份回归对照）+ M5-07
      （主题三选深浅两档全壳对照 aki-m5-07-settings/-dark/-light.png +
      onShutdown 关闭序日志）。
    - 设计第 15 节链路清单逐环节映射：身份建立（SCOPE-01）→发现
      （SCOPE-02）→信任确认（SCOPE-03，GUI 弹窗被缺陷阻塞——见上）→
      会话（SCOPE-05）→文本/图片/文件（SCOPE-06/07/08）→进度→暂停/
      恢复/取消（SCOPE-08）→重启恢复（SCOPE-09）→断线恢复（SCOPE-11）
      ——逐环节证据同上；**全链路串联（双端）未执行**（退出-1 保持
      未勾选，见 ⑤ 降级声明）。
  - **③ 本机补证（本项新增归档）**：发现启停 GUI 实拍
    （aki-m5-08-discovery.png：Devices 页三行三信任态 + Start Discovery
    点击后 "start_discovery admitted" 反馈——真实 LAN 发现启动）；GUI
    数据根经 `APPDATA` 注入 scratch 剖面（真实 `%APPDATA%ki` 未触碰，
    演示行经 gitignored scratch 种子探针写入）；aki-run-m5-08-final.log
    （设置解析行 + onShutdown 8 步关闭序 fully_stopped=1）。
  - **④ M3 登记补做条件复核闭环（M3-09 ④ 归档项逐项结论）**：
    - (a) **占位口令 verifier：闭合**——DEC-016（M5-04）落地
      kAkiPairingPassword + 真实 argon2id verifier
      （heyaki/adapter/local_identity.hpp:125-129 create_password_
      verifier 接线；test_local_identity 口令往返用例；M5-04 评审修正
      argon2id 创建 57-67ms 5 连跑实测）；M3-03 占位假编码串已退役。
    - (b) **kAkiPairingPassword 维持（登记不变）**：DEC-016 移除条件
      「M5-07 设置面引入用户口令」未触发——M5-07 交付的最小设置项为
      数据目录展示等（M5 范围条款 :70-71），不含口令输入；配对口令仍为
      公开弱口令（DEC-016 安全语义披露维持），再登记至用户口令流程
      立项。
    - (c) **secret backend prefer_os_backend=false：再登记（触发条件）**
      ——代码现状 heyaki/adapter/local_identity.hpp:75（heyaki 加密文件
      回退承载，确定性配置）；M3-03 登记的「OS 钥匙串随 M5 设置面」未
      触发（同 (b)，M5-07 设置面未含 secret backend 项）；再登记触发
      条件：OS 钥匙串集成的安全/产品需求确认（届时经决策记录定
      prefer_os_backend 平台策略与用户口令流程）。
    - (d) **DeviceIdentity display_name/device_class/os_name/capabilities
      元数据占位：再登记（触发条件）**——代码现状 heyaki/adapter/
      lan_discovery.hpp:105-107（display_name 取 DeviceId 前 16 字符
      占位；class/os/capabilities 缺省——DEC-006 缺口条目：pinned
      LanPresence 不携带元数据）；再登记触发条件：heyaki 协议演进使
      LanPresence 携带元数据，或 RPC 能力查询可用（届时映射至
      DeviceIdentity 并同步 GUI 列）。
    - (e) **发现来源分期（Relay/邀请链接/手动输入）：维持 M3-09 登记**
      （GUI 分期披露维持，aki-m5-08-discovery.png 页脚可见）。
  - **⑤ 双端真机项显式降级声明（退出-1 保持未勾选）**：范围=文本
    Delivered 回报、图片/文件 wire 面与对端效果、断连→恢复横幅转换、
    传输暂停/恢复/取消 wire 侧、M3/M4 退出-1/3 同批补跑（含 CI tsan
    运行期证据）；**原因**：本机防火墙拦截入站 TCP、无第二台同网段
    Aki 设备（M3-09 起同批约束，网络无关半边已全部验证——见 ②）；
    **负责人**：Linductor；**补跑条件**：防火墙放行入站 TCP + LAN 双端
    真机（两台设备同网段运行 Release aki.exe），按 M3-09/M4-07 先例
    执行 M5-08 退出-1 全链路清单并归档证据。
  - **⑥ 同步**：本里程碑（M5-08 勾选、本记录）、总计划（当前状态条目
    含 PR #44 欠账补记）；aki_design §15 无变更（清单原文为准）。缺陷
    （Confirm 无响应）走独立修复工作项/MR，修复后回填 SCOPE-03 GUI
    证据并复跑全量 ctest。（已闭环：BUG-20260927-001 同日修复，见下方
    专用验证记录。）

- 2026-09-27（**BUG-20260927-001 修复与闭环**；M5-08 验收发现缺陷的独立
  修复工作项，工程规范 3.3 编号；Windows 11 工作站（桌面会话）/ MSVC
  2022 BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **症状（登记复现序列）**：Devices 页新建第二设备到 Pending → 点击该
    行 Confirm——弹窗不出现、无任何状态变化；同行 Reject/邻行 Revoke/
    导航/其他页按钮均正常；2 个全新会话各 ≥2 次点击 100% 复现；并伴随
    一次 Confirm 位置点击后出现 alpha 行 "revoke admitted" 的疑似错乱
    触发。
  - **根因（Aki 页面代码缺陷，非 pinned 缺陷）**：main_window.cpp 的
    信任确认弹窗 builder 链以 `.content([...]);` 结尾——**缺尾部
    `.build()`**。DialogBuilder 仅在 build() 中创建元素树（构造器只存
    ui/id 字符串），缺失时弹窗根本不进入 UI 树：点击 Confirm 仅改写页
    面模型 pending_confirm_device，重组后树中无弹窗元素，渲染帧无变
    化（本机实测帧率归零、连续截图逐字节一致）。M5-05 的两处弹窗有
    `.build()`，故同机制下工作正常——缺陷定位由临时插桩日志证实
    （onClick 触发 → 模型置位 → 重组 dialog_open=1 → builder 链执行但
    无元素产生）。M5-08 登记的「错乱触发」经复核为自动化窗口坐标漂移
    误点 alpha 行 Revoke（该按钮工作正常），非回调错绑。
  - **修复（ui/pages/main_window.cpp，一处链尾补全 + 同批对齐）**：
    补 `.build()`；同批对齐 M5-05 弹窗形态——补 `.screen(width,height)`
    （背板/居中锚点覆盖宿主窗口，原默认 800x600 不覆盖 1080x720 窗、
    面板错位）、`.theme(tokens)`（跟随浅/深档，原默认恒深色样式）、
    `.onOpenChange`（背板点击关闭请求回写页面持有 open 态，
    与 M5-05 弹窗行为一致；无 Escape 路径，见复测修正注）。不改
    pinned EUI-NEO；不改网络/持久化语义；无新增并发路径（DOD-02 无新增面）。
  - **可测面与测试边界（如实登记）**：缺陷为纯 compose 交互层（builder
    链缺调用），页面模型/派生层无逻辑变更——无可新增的网络无关单测断
    言面（test_device_trust 的信任操作路由/转移边断言维持承载）；回归
    守卫按 RULE-11 以 GUI 复现序列日志 + 截图归档承载（见复测）。同批
    审计其余 builder 链（M5-05 预览/新建会话弹窗、M5-07 segmented、
    传输卡片 progress）：均以 `.build()` 结尾，无同类缺失。
  - **复测（登记复现序列）**：2 个全新会话（App 重启）各 ≥2 次点击
    Confirm——弹窗即现（mono 指纹 hy1_seedbeta…、无口令框，aki_ui_design
    §3 规格）；「Confirm pairing」提交 admission 反馈可见
    （pairing submitted for hy1_seedbeta…——真实 Adapter 无会话，配对
    结果按 DEC-006 映射 3 异步，本地维持 Pending 如实呈现）；「Cancel」
    关闭不提交；背板点击关闭回写实测。**修正（2026-09-27 评审）**：本条
    原记「背板/Escape 关闭回写实测」中 Escape 关闭不实——该路径在仓库中
    不存在：pinned DialogBuilder 仅将 requestClose 接到背板 `.onClick`
    （third_party/EUI-NEO/components/dialog.h:107；closeCallback :211-218），
    未注册任何 onKeyEvent（按键按元素分发，core/dsl.h:725）；EUI-NEO 内
    唯一 Escape 处理为文本输入组件提交文本（components/input.h:315）；
    Aki 侧 main.cpp/ui/ 均无 Escape 处理。按 RULE-11 如实收缩为仅背板
    点击关闭实测；如确需 Escape 关闭，先按决策流程立项补路径后再测。
    回归：Reject/Revoke 按钮、
    导航四页切换正常；debug/release 全量 ctest 各 43/43 零回归（修复后
    最终代码态）。证据：build/scratch/aki-bug001-fix-dialog.png
    （弹窗）、aki-bug001-fix-confirmed.png（提交反馈）、
    aki-bug001-fix-dialog2.png（会话1 第 2 次点击）、
    aki-bug001-fix-cancelled.png（取消后）、aki-bug001-s2-dialog1/
    dialog2.png（会话 2）+ aki-run-bug001-fix.log（关闭序）。复现/
    复测命令：沿 M5-08 记录 ③（scratch APPDATA 注入 + 种子探针）+
    build/scratch/gui_demo.ps1 自动化（pin/click/winshot）。
  - **同步**：M5-08 验证记录 ② SCOPE-03 条目（BUG 闭环回填）、M5-08
    记录 ⑥（闭环注记）、总计划当前状态（BUG 编号落档 + 修复条目）。
    无决策记录（根因为 Aki 页面代码缺陷，无上游能力诉求）。

- 2026-09-27（`M5-09` 完成（收口审计与退出证据归集，纯审计与文档，无产品
  代码变更）；Windows 11 工作站（桌面会话）/ MSVC 2022 BuildTools
  14.44.35207 / CMake 4.1.0 / gh CLI（GitHub API）；审计基线 =
  master @ `afaab0b`（#49 合入，工作树干净，`git status` 0 项）；负责人：
  Linductor；沿 M1-08/M2-08/M3-09/M4-07 纪律）：
  - **① 设计-实现审计矩阵（逐项一致，未记录偏差数 0）**：
    - aki_design §6.1（wire id 生成入口收敛）：`heyaki/adapter/wire_ids.hpp`
      单一入口（`new_message_id`/`new_transfer_id` = 16 随机字节全零重抽 →
      `::heyaki::to_string` 规范串）在码；`NodeSession::new_transfer_id`
      委托同入口（runtime_node.hpp:417-420）；页面经 `UiActions::
      new_message_id/new_transfer_id` 取 id（ui_actions.hpp，RULE-10 不携带
      wire 编码知识）——与 §6.1② 文字一致。
    - aki_design §9（三栏 + 四页导航）：main_window.cpp 三栏壳（导航/列表/
      内容）+ Conversations/Devices/Transfers/Settings 路由；页面模型持
      UI 态、compose 只读派生——一致。
    - aki_design §9.1（UI 装配契约五条 + M5-02~07 增补条款）：视图模型
      四域纯函数派生（ui/models/view_models）；快照消费水位
      （ui_state_consumer `load_snapshot_newer_than`）+ 双唤醒钩子
      `on_publish`/`on_update_submitted`（app_state_owner.hpp:84/:92，
      host_runtime.cpp:367/:372 同注 `app::requestUpdate`）——一致；
      首帧装配例外（main.cpp:204-261 frames==1 同步 `ensure_assembled`，
      唯一显式例外）——一致；启动↔关闭配对（main.cpp onShutdown 薄委托
      `shutdown_with_report`）——一致；关闭序 §8.3 钩子原序 + EXEC-01
      步骤 2~5（host_runtime.cpp:606-678：request_cancel_all→flush 四
      Manager→adapter.stop_delivery→peer_pipeline.stop→reconnect.stop_all→
      node_session.shutdown→state_owner.close→db.request_drain，
      hook_sequence 逐项入报告）——一致；主题档位覆写清单（aki_theme_
      values.hpp 与 §9.1 清单逐值一致：title 18/subtitle 16/body 14/
      caption 13/hint 12/micro 10/label 14、radius.small 4、field 36/
      menuItem 28、间距六档零覆写）——一致；Settings 页与主题三选
      （theme_mode.hpp `resolve_effective_theme` Unknown 回落 Light +
      system_theme 平台单元）——一致；渲染层验证策略（RULE-11 截图/日志
      归档 `build/scratch/`）——一致。
    - aki_design §10/§10.1（状态边界与 comm 映射）：`AppState` 四域
      Store；UI 只读消费（ui/ 无 submit_update/post_event/drain——grep 0）；
      `SetDeviceConnectionPath` 部分更新（app_state_updates.hpp:46-49）、
      退役全局 `SetConnectionPath` 代码引用 0（唯一命中
      app_state_updates.hpp:45 注释「取代退役的全局…」为 DEC-015 登记的
      文档性退役注）——一致；无 ad-hoc 队列/自建条件变量等待——一致。
    - aki_design §11.1（接收根 GC 显式延后条款，:1173-1179）：设计条款
      与 M5-06 ⑤ 处置及触发条件三则互引一致——一致。
    - aki_design §14（工程目录）：ui/components|pages|models|theme 实体化
      与目录树一致；`transfer/manager/` 暂空注记、`transfer/storage/`
      抽象面落位注记（M4-04）维持——一致。
    - aki_ui_design §2~5：§2.1/2.2/2.3 令牌常量（aki_theme_values.hpp，
      test_ui_theme_values 62 断言独立抄录对拍）；§2.4 深度锚点
      （panelShadow/popupShadow，M5-01 复核结论维持）；§2.6 图标码点
      （FA7 码点表 M5-05 起按登记消费）；§3 状态视觉语义（投递/传输/
      信任/路径徽标语义色、断连横幅、无传输行兜底态——conversations_page/
      transfer_card 实现）；§4 组件映射（会话列表=scrollview 行（预算
      256，非 virtuallist）、气泡=card+text、图片=image+dialog、文件卡片
      =card+progress+button 共享 transfer_card、输入区=input+button、
      主题三选=segmented——组件使用 grep 统计与映射表逐项对应）；§5
      复审记录（M5-01）与落地记录（M5-05/06/07）已归档——一致。
    - DEC 逐一核对（4 项）：[DEC-005](../decisions/DEC-005-eui-neo-integration.md)
      （pinned `b9032a8a` v0.6.0、八项开关 CACHE FORCE、并发边界禁用面
      grep 0、`eui::neo` 仅 aki_ui 链接、测试 exe 不链 eui（链接图核验，
      见 ④——原 dumpbin 符号复核经复跑证实无区分力已更正）、requestUpdate
      唤醒、「影响与风险」验证项 M5-02
      回填在档）——一致；[DEC-014](../decisions/DEC-014-eui-tsan-coverage.md)
      （全图插桩零豁免 + CI Linux 完整依赖集 + 覆盖声明五条 + 抑制表
      登记制；CI 五档含 tsan 全绿见 ⑤）——一致；
      [DEC-015](../decisions/DEC-015-per-device-connection-path.md)
      （逐设备路径集合 + 部分更新类型、断连置 Unknown、初连补发、退役
      全局摘要 0 残留）——一致；
      [DEC-016](../decisions/DEC-016-pairing-password-verifier.md)
      （`kAkiPairingPassword` 冻结常量、created 分支真实 argon2id
      verifier、弹窗无口令框（main_window.cpp 确认弹窗仅 mono 指纹 +
      Confirm/Cancel）、集成回环字面量替换在档）——一致。
  - **② 已知偏差锚点核实（6 项，登记全部维持，无需新增偏差）**：
    1. SCOPE-02 去重/不重放未测——锚点在码：lan_discovery.hpp:94-98
       （`entry.trusted → continue` 不重放 + `seen_` insert 去重）；tests/
       无直接 LanDiscovery 管道单测（grep 证实仅 loopback/adapter 间接
       触达）——M5-08 ② 评审修正登记维持，补测条件（新增管道单测）随
       双端补跑同批。
    2. UpsertDevice 整行替换覆写隐患 + PairingCompletedWork 结果丢弃——
       锚点在码：device_manager.hpp:230-252（`apply_trust_transition` 从
       最近已发布快照读-改-写整行 upsert）+ :280-288（`handle(
       PairingCompletedWork&)` 仅返回 `apply_trust_transition` 结果，拒绝
       仅 `handler_rejections` 计数、无延迟重排）——「风险与阻塞」节
       后续工作项登记维持（与本覆写隐患同批处置）。
    3. 主题选择会话级不持久化——锚点在码：main.cpp:239-249 组合根解析
       一次，全仓无主题持久化写入路径（schema v1 无设置表，扩表须先立
       决策）——M5-07 降级登记维持。
    4. 接收根残留 GC 显式延后——设计 §11.1 条款 + M5-06 ⑤ 触发条件
       三则互引完整——登记维持（DEC-012③ 冻结）。
    5. 传输行操作反馈与披露行同屏截图——M5-06 ⑩ 登记的复验为跨页
       sticky 反馈路径（aki-fix-footer-*.png，归档在）；传输行同屏归
       M5-08 双端补跑（M5-06 原 scratch 剖面未留存、传输行不可复现，
       如实维持）——登记维持。
    6. BUG-20260927-001 闭环与 Escape 修正注——修复在码：main_window.cpp
       确认弹窗链尾 `.build()`（:579）+ `.screen/.theme`（:505-506）+
       `.onOpenChange`（:574-578）；「无 Escape 路径」收缩注在 M5-08
       记录与总计划（pinned dialog 仅背板 onClick 接 requestClose）——
       登记维持。
  - **③ 退出-2 复验（DOD-02 六项沿 UI 并发路径映射 + 直跑断言计数）**：
    六项沿宿主生命周期路径承载——tests/unit/test_host_runtime.cpp 单用例
    67 断言（① 正常完成 :116 submit_auto + Manager 泵 + 真实发现启停；
    ② 任务异常 :140 future 上浮 + failure 计数；③ 执行中取消 :152
    submit_cancellable + request_task_cancel 协作退出；④ 提交拒绝 :185
    max_in_flight 耗尽 → CapacityExhausted 即时就绪；⑤+⑥ 超时与
    shutdown :207-269——completion_wait 预算耗尽如实记录 + §8.3 钩子
    原序 8 步 + 两 blocking worker 2/2 回收 + 幂等 + 关闭后提交显式
    拒绝）。跨线程唤醒/钩子异常：test_ui_models（on_publish 发布→唤醒
    调序 + executor 任务内驱动 drain + 钩子异常全捕获 + on_update_
    submitted 受理点，10 用例 135 断言）；出站通道：test_ui_actions
    （页面→UiActions→Manager 泵→Fake SPI 全通道，2 用例 67 断言）。
    UI 新增路径自 M5-03 起未新增池上线程/周期任务/自有调度面（M5-06/07
    记录「无新增并发路径」结论维持），六项由宿主路径承载成立。本会话
    直跑（当前代码态）：`build/debug/tests/Debug/test_host_runtime.exe`
    → 67 断言全过；`test_ui_models.exe` → 135 断言 10 用例全过；
    `test_ui_actions.exe` → 67 断言 2 用例全过；`test_ui_theme_values.exe`
    → 62 断言 7 用例全过。
  - **④ 退出-3 复验（边界 grep，命令与输出；终态代码 = afaab0b）**：
    1. 第一方线程创建：`grep -rn "std::thread\|std::jthread\|std::async\|
       CreateThread\|pthread_create" app/ conversation/ device/ heyaki/
       persistence/ transfer/ ui/ main.cpp`（剔除 `std::this_thread`）→
       **0 命中**（tests 侧 `std::this_thread::get_id` 线程标识比较为
       轮询/断言纪律内，无线程创建）。
    2. DEC-005 禁用面：`grep -rn "app::async\|core::network\|eui::network\|
       eui::audio\|beginTask" ui/ main.cpp` → **0 命中**。
    3. EUI 越层：`grep -rln "eui/\|eui::" app/ conversation/ device/
       heyaki/ persistence/ transfer/` → **0 命中**。
    4. ui/ 持 transport 类型：`grep -rn "HeyakiAdapter\|NodeSession\|
       HeyakiNodeAdapter\|RuntimeNode" ui/` → 仅 ui/CMakeLists.txt:15
       注释 1 处（文档性）。
    5. ui/ 直写 Store：`grep -rn "submit_update\|post_event\|drain_updates\|
       publish_if_dirty" ui/` → **0 命中**（RULE-02）。
    6. eui include 落点：仅 main.cpp + ui/pages/*（5 文件）+
       ui/components/transfer_card.hpp + ui/theme/aki_theme.hpp（aki_ui
       目标内渲染面；ui/models 无 eui——DEC-005「测试 exe 不链 eui」
       维持）。
    7. RULE-10 公开面：`grep -rn "eui::\|components::\|GLFW\|glfw\|
       windows.h\|RegGetValue" ui/models/*.hpp app/lifecycle/system_theme.hpp
       app/lifecycle/host_runtime.hpp` → **0 命中**。链接面复核**更正**
       （2026-09-28 独立评审复跑）：原记 `dumpbin //SYMBOLS
       test_ui_models.exe`/`test_host_runtime.exe` 对
       `eui::|components::|glfw` 0 命中不构成证据——同命令复跑
       （BuildTools dumpbin 14.44.35214）两 exe 输出各仅 19 行节摘要、
       连 `aki::` 亦 0 命中（默认 MSVC 链接把符号表剥离至 PDB，该检查
       对链接产物无区分力，原「0 命中」记录作废）。改用有区分力的链接
       图核验（2026-09-28 复跑）：`cmake
       --graphviz=build/debug/aki_target_graph.dot build/debug` →
       `test_ui_models -> {Catch2WithMain, aki_ui_models}`、
       `test_host_runtime -> {Catch2, aki_host}`、`aki_ui_models ->
       {aki_app, aki_heyaki, aki_persistence, heyaki_core}`
       （aki_target_graph.dot:251-252/348-349/99-102）；`aki_ui ->
       eui_neo (eui::neo)` 仅被 GUI 宿主目标 `aki` 链接
       （aki_target_graph.dot:132-135）。DEC-005「测试 exe 不链 eui」
       结论维持，源级一致：ui/CMakeLists.txt:8-24（aki_ui_models 纯
       std/aki 面）、tests/CMakeLists.txt:528-530/547-549（直链
       aki_host/aki_ui_models，不经 aki_ui）。
  - **⑤ 退出-4 复验（构建/测试/CI）**：本机复跑（本会话执行，当前代码
    态 afaab0b）——debug：`ctest --test-dir build/debug --preset debug`
    → `100% tests passed, 0 tests failed out of 43`（171.6s；`cmake
    --build --preset debug` 增量重建后复跑 exit 0，159.8s）；release：
    `cmake --build --preset release --config Release` 增量重建（0 error）
    + `ctest --test-dir build/release --preset release` → `100% tests
  passed, 0 tests failed out of 43`（174.2s）。`gh pr checks` 逐 PR
  核实（本会话执行）M5 系列 #39~#49 共 11 个 PR 全部五档（Linux
  debug/asan/ubsan/tsan + Windows MSVC）最终态 pass——#39（M5 文档）
  run 36220267115、#40（M5-01）run 36223297589、#41（M5-02）run
  36260288692、#42（M5-03）run 36265960159、#43（M5-04）run
  36296441677、#44（图标）run 36296528961、#45（M5-05）run
  36304569570、#46（M5-06）run 36310345155、#47（M5-07）run
  36314463441、#48（M5-08）run 36319355761、#49（BUG-001）run
  36327544916。渲染层本机证据归档复核：`build/scratch/` 截图 ×29 +
  运行日志 ×8（M5-02~08 各批 + BUG-001 + 页脚修正，RULE-11 口径）。
  **master push 触发档如实登记（见 ⑧）**：当前 HEAD（afaab0b）的 push
  run 36328366422 Linux/tsan 失败（42/43，test_peer_sessions_loopback
  ——Catch2 内部状态竞争，间歇性；同内容树在 #49 PR run tsan 通过），
  合并门禁（PR 档）全绿结论不受影响，缺陷已登记 BUG-20260927-002。
  - **⑥ 退出-5 复验（文档同步与链接）**：设计（§6.1/§9/§9.1/§10/§10.1/
    §11.1/§14）、决策（DEC-005 回填/DEC-014/015/016）、aki_ui_design
    （§5 复审 + M5-05/06/07 落地记录）、总计划（当前状态 M5-01~08 +
    BUG-001 条目 + 决策表）逐项核对一致（见 ①②）。相对链接核验（本
    会话执行，脚本对 M5 系列变更文档集：aki_design/aki_ui_design/
    zcode-design-system/m5 里程碑/总计划/DEC-005/014/015/016/
    assets/icons/README.md 共 10 文档）：`links checked: 145, broken: 0`；
    M4 勾选对齐编辑后终态复跑（M4 文档并入，共 11 文档）：
    `links checked: 173, broken: 0`。
    本会话文档订正两处：总计划「更新日期」2026-09-26 → 2026-09-27（原
    值滞后于 2026-09-27 的 M5-05~08/BUG-001 条目）；本文档「更新日期」
    「M5-05 完成同日」→「M5-09 收口审计同日」（同因）。
  - **⑦ 审计附带复核与处置**：
    - **M4 退出-2~5 勾选对齐**：M4-07 记录（2026-09-26）已归集退出-2~5
      完整证据（DOD-02/状态机映射「映射完整」、本机复跑 38/38、gh 核实
      #30~#37 五档全绿、链接 `219 checked, 0 broken`——m4-image-file-
      transfer.md M4-07 记录「退出-2~5」段），但四个勾选框全部保持
      `[ ]`，与 M3-09 先例（m3-heyaki-integration.md 退出-2/4/5 为
      `[x]`，仅环境受限退出项保持未勾选）不一致。本审计逐项核实证据
      后按工程规范 §4 规则 1/5 与先例对齐勾选（依据为 M4-07 归集证据，
      非本审计新产生验证；本审计全量 ctest 43/43 零回归交叉佐证）；
      退出-1 双端真链路属环境受限，保持未勾选。对齐说明注记于 M4 文档
      退出条件节首（M3-09 评审修正注同款形态，不改写历史记录）。
    - **许可证检查（设计第 9 节，本工作项登记的复核项）**：**维持发行
      前项、不冒充已完成**。现状盘点：EUI-NEO 本体 Apache-2.0
      （third_party/EUI-NEO/LICENSE；锁文件 `license`+`license_file`
      已登记，`used_by` 注明「assets 许可证在发行前审计」）；bundled
      3rd 十件套（glfw/freetype/libpng/zlib/glad/md4c/miniaudio/tray/
      yyjson/nanosvg+stb）各自许可证随 pinned 源码树分发；assets 面含
      Font Awesome 7 Free-Solid-900.otf、中文字体 ×2（JingNanJunJunTi
      Bold/YouSheBiaoTiHei）、svg 插画、icon、shadertoy shader——
      逐一许可核验留待正式发行前（含 aki_ui_design §2.6/§6 与
      assets/icons/README.md 已登记的 heyaki 图标 MIT 溯源）。触发条件
      = 发行流程启动。
    - **里程碑状态处置**：`M5-01`~`M5-09` 工作项全部完成（本项勾选）；
      退出-1 双端全链路属环境受限（防火墙拦截至端 TCP、无 LAN 双端，
      原因/负责人 Linductor/补跑条件已登记 M5-08 记录 ⑤）——沿 M3-09/
      M4-07 先例**保持未勾选**，**M5 保持 In Progress**（工程规范 §4
      规则 5：工作项全部完成不自动关闭里程碑；关闭待退出-1 补跑后
      复核，如需缩小退出口径须先经决策记录重新划界）。
  - **⑧ 审计发现登记：BUG-20260927-002（CI master push tsan 间歇红档；
    本项纯登记，修复走独立 MR）**：
    - **现象**：master push 触发的 CI 六档中 Linux/tsan 间歇失败——当前
      HEAD（afaab0b，#49 合入）run 36328366422（2026-09-27）42/43，
      test_peer_sessions_loopback Failed；回溯同因先例：55206b0（M5-06
      合入）run 36311065064（2026-09-27）同测试同报告。近 6 次 master
      push run 中 2 次红（其余 4 次含同测试全绿），同期全部 M5 系列 PR
      档 tsan 全绿（含同内容树 #49 run 36327544916）——间歇性，
      时序依赖。
    - **报告本体（vendored Catch2 内部状态）**：
      `SUMMARY: ThreadSanitizer: data race src/catch2/internal/
      catch_run_context.cpp:598 in Catch::RunContext::assertionPassed
      FastPath`——`m_lastAssertionPassed` 快路径无锁读与互斥下写竞争；
      竞争两侧栈均无 `aki::` 第一方帧。
    - **根因（第一方测试代码诱因，tests/ 属第一方面）**：
      test_peer_sessions_loopback.cpp:246-274 在 `PeerSessionEvents`
      三个事件回调（on_connected/on_disconnected/on_connection_path_
      changed——经 `PeerSessionPipeline` 于 **executor timer 线程**执行）
      内使用 `REQUIRE(...)`；主线程断言评估（如 :310）与之并发时触发
      Catch2 RunContext 非线程安全内部状态（Catch2 断言仅保证主线程
      使用）。DEC-014 覆盖声明第 4 条：第一方（含 tests/）竞争必修、
      不进抑制表——本缺陷不引入 `race:Catch` 抑制。
    - **影响面**：仅 CI tsan 档该测试二进制（CI 专有触发形态——依赖
      事件在主线程断言窗口内到达）；不涉产品代码竞争（报告零 `aki::`
      帧；debug/release/asan/ubsan 全绿；产品并发路径由既有全档门禁
      与 test_host_runtime 等覆盖）。合并门禁纪律未破：六次合并的
      PR 档全绿在先。
    - **修复路径（独立 MR，另行排期）**：回调内 `REQUIRE` 改为原子
      记录（沿同文件 connected_events/path_events 既有原子计数形态），
      主线程 `wait_until` 截止后统一断言；补验 = tsan 档多次连跑 +
      debug/release 全量零回归。登记期间 master push tsan 红档为已知
      间歇缺陷，不冒充绿档。（已修复：2026-09-28 修复落地工作树；同批
      审计扫描发现并同型修复同族第二触点
      test_disconnect_recovery_loopback.cpp:203-233——范围延伸披露，
      详见下方 2026-09-28（BUG-20260927-002 修复）专用验证记录。闭环
      注记沿 M5-08 记录 ⑥ 先例。）
  - 限制与补跑条件：退出-1 双端全链路（含 SCOPE-02 去重/不重放补测、
    传输行同屏截图、M3/M4 同批补跑项）待防火墙放行入站 TCP + LAN 双端
    真机，负责人 Linductor；许可证逐项审计留待正式发行前；
    BUG-20260927-002（master push tsan 间歇红档）待独立 MR 修复——修复
    前以 PR 档门禁为合并前置（既有纪律），master push tsan 红档按 ⑧
    登记理解，不冒充绿档。
  - 同步：本里程碑（M5-09 勾选、退出-1~5 处置、更新日期、本记录）、
    [M4 里程碑](m4-image-file-transfer.md)（退出-2~5 勾选对齐 + 更新
    日期，M4-07 记录原文未改写）、总计划（更新日期 + 当前状态条目）。
    无设计/决策变更（审计未发现需要改设计的偏差）；无产品代码变更
    （`git status` 仅 docs/ 三文档）。

- 2026-09-28（**BUG-20260927-002 修复**；工程规范 6.3 最小变更记录；
  Windows 11 工作站（桌面会话）/ MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0；基线 = master @ `108a41d`（#50 合入，工作树修复前干净）；
  负责人：Linductor）：
  - **范围**：仅测试代码——消除 master push CI Linux/tsan 间歇红档的
    第一方诱因（本里程碑 M5-09 验证记录 ⑧ 登记的缺陷）。不改产品代码、
    不改 pinned 依赖、不引入 `race:Catch` 抑制（DEC-014 覆盖声明第 4
    条：第一方竞争必修、不进抑制表）；无新增并发路径（DOD-02 六项无
    新增面——既有宿主生命周期覆盖维持）。
  - **修复内容（两文件，同一根因同型修复）**：
    - `tests/integration/test_peer_sessions_loopback.cpp`（⑧ 登记触点）：
      `PeerSessionEvents` 三回调（on_connected/on_disconnected/
      on_connection_path_changed，executor timer 线程执行）内的 `REQUIRE
      (submit_update(...))` 全部改为 bool 接收 + 原子计数 `submit_failures`
      （沿同文件 connected_events/path_events 原子计数形态）；主线程两处
      统一断言——connected 到达后首个主线程断言位（[skip] 退出点之后，
      提交缺陷不被环境降级掩盖）+ `pipeline.stop()` + 800ms 静置后的
      权威断言（timer 取消后全部回调静止）。
    - `tests/integration/test_disconnect_recovery_loopback.cpp`（**同族
      第二触点，本批发现并同型修复——范围延伸披露**：审计扫描其余集成
      测试时发现该文件 :203-233 同样在 `PeerSessionEvents` 回调内使用
      `REQUIRE`，含 `REQUIRE(coordinator.start(peer, hooks))`，为同一
      根因（executor timer 线程执行 Catch2 断言）的未爆点）：回调内
      REQUIRE 改为 `submit_failures` 原子计数 + `coordinator_start_
      failed` 原子记录；主线程两处统一断言——disconnected 到达后（依赖
      重连的断言之前，缺陷不表现为超时误诊）+ `pipeline.stop()`/
      `coordinator.stop_all()` 消费后的权威断言。修复语义等价性：原
      REQUIRE 失败即用例失败，现失败计数非零即用例失败（主线程），
      验证强度不降；`if (peer == identity_b.id)` 守卫内断言语义保持。
    - 其余 6 个集成测试（discovery_pairing/image_message/message/real_
      adapter/transfer_full/transfer_send loopback）的 `set_pairing_
      observer`/`set_file_event_observer` 回调逐一核验：均为原子/互斥
      记录形态，无 Catch 宏——同族残留 0；单测侧（test_peer_sessions_
      pipeline/test_reconnect_loop 手动驱动回调）核验无回调内断言。
  - **验证（可复现命令与结果，本会话执行）**：
    - 编译：`cmake --build --preset debug --config Debug` 增量重建——
      两测试二进制重编链接 0 error 0 warning（exit 0）；
      `cmake --build --preset release --config Release` 同。
    - debug 全量 `ctest --test-dir build/debug --preset debug` →
      `100% tests passed, 0 tests failed out of 43`（168.0s）；release
      全量 `ctest --test-dir build/release --preset release` →
      `100% tests passed, 0 tests failed out of 43`（177.6s）——零回归。
    - 修复二进制连跑（debug）：`test_peer_sessions_loopback.exe` ×10 与
      `test_disconnect_recovery_loopback.exe` ×10 全部 exit 0——**如实
      限定**：本机防火墙拦截至端 TLS，两用例在本机走 [skip] 受控退出
      （`[skip] pairing handshake blocked (firewall)` 实测输出），修复
      代码段（回调注册之后的回调执行）**本机不可达**；连跑仅证明编译
      链接与前段（发现/连接/受限判定）无回归。
    - **tsan 档本机不可达取证**：`cmake --preset tsan` → configure
      失败：`HEYAKI_SANITIZER=thread requires GCC or Clang in the M0
      baseline`（本机唯一工具链 MSVC；tsan 预设 displayName 即
      "GCC/Clang, Linux"）——验收项「tsan 档该测试二进制多次连跑零
      data race 报告」本机不可执行，沿 DEC-014 覆盖声明第 5 条
      （TSAN 声明限 Linux）与 M1 起既有登记口径，由 CI Linux/tsan 档
      承载。
  - **限制与补跑条件（如实登记，不冒充绿档）**：(a) 修复的直接证据
    （tsan 档零 data race 报告 + 用例通过）**尚未取得**——PR 档 CI
    五档与 master push tsan 档观察随本修复 MR 闭环执行（本会话未推送，
    如实登记；间歇缺陷以多次绿档佐证收敛，单次绿不宣称证明）；MR 描述
    须复述本条。（→ 已取得：同日 #51 合入后回填，见下方「CI 证据
    回填」条。）
    (b) 若 CI 观察轮次不足或再现他因红档，按工程规范 §4
    登记原因/负责人/补跑条件，不勾选完成。(c) 退出-1 双端项（含本测试
    的真实双端语义）沿 M5-08 ⑤ 降级登记不变，负责人 Linductor。
  - **CI 证据回填（2026-09-28；#51 已合入 master `5803240`，gh 只读
    核实，本会话执行）**：
    - **PR 档五档全绿（验收 (3)）**：`gh pr checks 51` → 五档全部
      pass——Linux asan job 108680281118 / Linux debug 108680281065 /
      **Linux tsan 108680281121（8m36s）** / Linux ubsan 108680281088 /
      Windows debug (MSVC) 108680280912，全部属 run `36340772532`。
      **headSha 订正（2026-09-28 复核）**：`gh run view 36340772532
      --json headSha` → `6104928c3db4a2d729ee5aa596e18a343037e990`＝
      修复分支 `fix/bug-20260927-002-peer-session-callback-assertions`
      tip（PR 档 run 跑在分支 tip 上，非合入提交）；原记「headSha =
      `58032409b63d` = 合入提交」为错误绑定——`58032409b63d` 实为
      master push run `36341461485` 的 headSha（`gh run view
      36341461485 --json headSha` 实测，见下条）。五档 job 号与全
      pass 结论不受影响。
    - **master push tsan 档观察（验收 (4)，第 1 轮）**：push run
      `36341461485`（headSha `58032409`，event=push，触发
      2026-09-27T18:39:37Z）→ `gh run view 36341461485` 轮询至完成：
      **completed success**，五 job 全绿——Windows 13m0s（job
      108682262057）/ ubsan 10m39s（108682262251）/ asan 10m49s
      （108682262280）/ **tsan 12m7s（job 108682262305）** / debug
      9m41s（108682262366）。
    - **tsan job 证据细读（命令 `gh run view 36341461485 --job
      108682262305 --log`）**：`100% tests passed, 0 tests failed out
      of 43`；**`ThreadSanitizer` 报告计数 0**（对照修复前 run
      36328366422 同 job 同测试 1 报告即红）；`[skip]` 行计数 0——两
      修复二进制**实跑全路径而非环境跳过**：`Test #24:
      test_peer_sessions_loopback ... Passed 27.59 sec`、`Test #26:
      test_disconnect_recovery_loopback ... Passed 42.69 sec`（回调内
      修复段在 tsan 下真实执行）。
    - **观察轮次与收敛口径（记录 (a)「间歇缺陷以多次绿档佐证收敛，
      单次绿不宣称证明」的履行）**：修复后 master push 观察轮次 = 1
      （绿）；对照修复前基线 = 2026-09-27 近 6 轮中 2 红（同因，
      run 36328366422 / 36311065064）。**不宣称缺陷已收敛**：首轮绿
      与修复前红档的间歇性（同内容树曾有绿档）尚不可区分于运气；
      后续 master push 合入随批继续观察并按本条格式累计轮次；若再现
      同因红档按 (b) 重开缺陷（负责人 Linductor），他因红档另行登记。
      观察不足轮次如实写明：**当前 1 轮**。
  - **同步**：本里程碑（M5-09 记录 ⑧ 闭环注记 + 本记录 + 本回填条）、
    总计划（当前状态条目）。无决策记录（修复为 ⑧ 已登记路径的执行，无新
    取舍）；无设计变更。

- 2026-09-28（`M5-10` 导航与视觉修整；本机 Linux x86_64，GCC 13.3.0，
  CMake 3.28.3，pinned EUI-NEO `b9032a8a`；工作树特性分支
  `codex/m5-navigation-visual-polish`；负责人：Linductor）：
  - **范围与依据**：`aki_ui_design` §2.4/§2.6/§4 与 ZCode 导航、语义色、
    密度约束；沿用捆绑 Font Awesome 7 Free Solid 的 comments `f086`、
    network-wired `f6ff`、arrow-right-arrow-left `f0ec`、gear `f013`
    （四码点此前已由 §2.6 本地 cmap 核实，官方图标页可查）。导航图标与
    文字纵排，不新增字体、供应商或 `third_party/` 修改。
  - **实拍定位与修复**：默认 1080×720 物理窗口在本机 2× 缩放后 UI
    逻辑空间约 540×360；原固定 264 列表栏使 Devices 双操作按钮、
    Transfers/Settings 文案与主题三选越过右边界，列表统计行与分隔线
    叠印；非会话页显示 `M5-04` 等内部占位。列表栏现按逻辑宽度在
    168~264 间调整，页内控件以剩余宽度为上限；四页标题/摘要/空态
    改为对应页面信息。深色档实拍发现 pinned `ButtonStyle` 的 primary
    文字默认浅色，在浅色 primary 底上不可读；六个 primary 按钮消费点
    显式指定 `primary-foreground`（发送图标指定 iconColor）。切页清除
    上一页的操作反馈，避免 Settings 反馈残留在 Devices。
  - **构建与测试**：`git submodule update --init --recursive`；
    `bash third_party/heyaki/scripts/fetch_third_party.sh --all`；
    `cmake --preset debug -DGLFW_BUILD_WAYLAND=OFF`（本机缺
    `xkbcommon` 开发包，只构建已存在的 X11 后端，不改项目配置）；
    `cmake --build --preset debug -j 6` 成功，`aki` GUI 可启动；
    `ctest --preset debug -j 6` → 43/43 通过、0 失败；
    `impeccable detect --json` 对五个修改的 UI 文件返回 `[]`。
  - **渲染验收**：1080×720 窗口四页逐页实拍（浅色），1600×900
    Settings 与深色 Devices 实拍，图标、标签、空态、操作按钮均在界内且
    不叠印；深色 Start scan 文字经修复复拍可读。截图归档于本机忽略目录
    `build/scratch/m5-10/`（默认窗口四页 + 较宽窗口四页 + 深色
    Devices/Settings，共十张，GUI 不进 CI 的 RULE-11 证据）。
  - **限制与后续**：本项未新增并发、网络或持久化路径。M5 退出-1 的
    LAN 双端真机链路仍未补跑，原因、负责人 Linductor 与防火墙放行入站
    TCP 后补跑条件沿 M5-08/M5-09 原记录维持；本轮不据本机 GUI/测试
    将 M5 改为 Completed。初次记录时 PR 档 CI 待回填；本机测试与
    CI 证据分开登记（见下条）。同步 `aki_ui_design` §2.4/§4/§5、
    总计划当前状态与本工作项。
  - **PR/CI 证据回填（2026-09-28）**：PR #53
    `fix(ui): improve navigation and narrow-window readability` 指向
    `master`，run `36401990518` 五档全绿（`gh pr checks 53`：Linux
    debug 9m19s、asan 10m39s、ubsan 11m45s、tsan 11m40s、Windows
    MSVC debug 11m26s；全部 pass）；Squash 合入 commit
    `ca1b5618300d84b31d1c38c3ff977480322ca5e6`（GitHub REST
    `pulls/53` 的 `merged=true`、`merged_at=2026-09-28T09:29:56Z`），
    远程与本地特性分支已删除，本地 `master` fast-forward 同步且工作树
    干净。该 CI 证据只证明 PR 门禁，不替代 M5 退出-1 双端补跑。

### 2026-10-01：M5-36~39 第一轮实现与验证

- 已实现本地归档事实/失败经原 DatabaseWorker future 与 AppStateOwner MPSC
  回写；恢复 stored_*、检查路径及存在性。Completed 缺记录显示保存失败，
  路径不可用保留位置说明；没有新增 worker、队列或第三方代码修改。
- 聊天图片默认 Contain 缩略图，原预览弹窗读取接收/发送归档；聊天和传输
  页显示保存位置、复制完整路径、打开所在目录。该目录交给系统文件管理器，
  不执行收到的文件。界面继续使用 ZCode 令牌；新增图片尺寸已登记。
- Linux 在 GLFW 初始化前设置 locale，并在 DISPLAY 存在时选择 X11。
  本机修复版启动日志 GLFW platform=X11，assembly ok 249ms，历史归档
  3/3 文件存在，其中两张图片；这不等于图片实际渲染或中文输入已通过。
- Windows profile 检查异常不再隐式继续装配，显示错误和重试。日志迁移至
  `<data_root>/logs/aki-run.log`，记录密码设置决策，不记录密码；既有有效
  密码保留。用户尚未提供目标机 profile 创建时间，首启现象原因未确认。
- 本地 Debug 全目标构建通过；Debug/ASAN/UBSAN 的 test_app_state、
  test_file_store、test_restart_recovery、test_host_runtime、test_ui_models
  各 5/5 通过（9.66s/10.67s/14.32s）。另 Debug test_local_identity 通过
  （新数据根无密码不能建 profile、首启检查、旧默认密码迁移和身份保持）。
  新覆盖：归档入站/出站 join、路径穿越、不可变回写、失败重跑、背压、
  关闭后提交、历史记录恢复/缺失及恢复预算拒绝。
- 界面为原生 C++ EUI-NEO，未运行 Web DOM 检测；实际窗口检验待操作者。
  Linux/Windows 图形输入、双向图片/重启历史、复制路径/打开目录仍未验收，
  负责人 Linductor，补跑条件为安装新 CI 包后双设备在线；M5-36~39 保持未完成。
  TSAN 本机受已知 runtime mapping 限制，本阶段以最新 head CI 门禁补跑。
