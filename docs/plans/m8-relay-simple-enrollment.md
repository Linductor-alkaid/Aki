# M8：中继接入简化（地址 + 密码）

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M7 主体能力（enrollment/发现/建链/ICE 已交付；M7-08/09 收口并行）、
> [DEC-028](../decisions/DEC-028-relay-cross-subnet.md)（2026-10-07 修订：
> 决策 11 分阶段简化）
> 建议发布点：v0.8.0
> 更新日期：2026-10-08（M8-07 依赖升级）

## 目标

设置页中继接入的主路径收敛为「中继地址 + 注册密码」两项：租户、
bootstrap token、证书文件退出主视图（降级为高级路径全程保留），TURN
概念退出用户视野（relay 自动下发短时效凭据）。分三阶段落地，本里程碑
阶段 1 无上游依赖先行交付；阶段 2/3 以上游两项能力落地为前置。

## 范围与非目标

**范围**（依据 DEC-028 决策 11）：

- 阶段 1（本 PR）：`ui/components/fold.hpp` 折叠组件；设置页中继区主视图
  收拢（地址 + 注册按钮，租户/令牌/证书入「高级中继设置」折叠区，必填
  校验失败自动展开）；i18n；chevron 码点登记与实证；enrollment wrapper
  token 原位擦除（断言缝合点）；决策/台账/计划文档同步。
- 阶段 2（上游前置 HEY-20261007-001）：密码模式注册主路径；TOFU pin
  锚定自注册交换所见证书；密码擦除断言测试；错误反馈对齐上游错误码。
- 阶段 3（上游前置 HEY-20261007-002）：relay 下发 ICE 自动参与选路；
  `ice-servers.txt` 降级为高级覆盖；生效 ICE 配置与来源展示；短时效凭据
  生命周期语义接入。

**非目标**：

- 修改 pinned `third_party/heyaki`（密码准入与 ICE 下发均为上游能力，
  走反馈台账流程，Aki 不代实现）。
- 移除 token 制注册流程（高级路径全程保留：多租户、严格证书分发部署）。
- relay 服务器部署与运营（沿 M7 非目标，引用上游 `deploy/coturn/`）。
- 注册热生效（HEY-20261006-001 未解除，重启生效披露不变）。

## 设计与决策依据

- [DEC-028](../decisions/DEC-028-relay-cross-subnet.md) 决策 11（本里程碑
  冻结决策；阶段 2/3 条款为暂定默认值，开工前冻结）
- [Aki UI 设计规范](../design/aki_ui_design.md)§2.5/§2.6/§4 与
  2026-10-07 落地记录
- [DEC-018](../decisions/DEC-018-user-pairing-password.md)（凭据纪律：
  secureInput/clear_secret/token 原位擦除）
- 反馈台账：HEY-20261007-001（密码准入 + 注册结果回传指纹）、
  HEY-20261007-002（relay 下发短时效 ICE 配置）、HEY-20261006-001/
  002（既有重启生效与 owned runtime 限制）

## 工作项

- [ ] `M8-01` 设置页中继区高级收拢（阶段 1）：折叠组件 + 主视图
  （地址 + 注册）+ 租户/令牌/证书折叠区 + 必填校验失败自动展开 +
  i18n 中英词条；token 流程零删减（高级区回归）。
  （实现与编译/全量回归完成，2026-10-07；GUI 视觉复核因环境限制未执行
  ——无截图/输入注入工具且 GUI 宿主使用真实数据根，不冒充已验证，
  补跑条件见验证记录。）
- [x] `M8-02` enrollment token 原位擦除与断言回补（阶段 1）：
  `enroll_relay_profile` 全部退出路径原位擦除 `bootstrap_token`（成功/
  校验拒绝/证书失败/交换失败），IVA 断言测试覆盖（单测失败路径 + 进程内
  e2e 成功路径）——阶段 2 密码擦除断言的同型缝合点。
- [x] `M8-03` 决策与台账同步（阶段 1）：DEC-028 决策 11、HEY-20261007-001/
  002 台账条目、UI 规范 §2.6/§4 与落地记录、总计划当前状态与里程碑索引。
- [ ] `M8-04` 密码模式注册主路径（阶段 2；上游能力已随 pin 1b0447b 落地
  ——HEY-20261007-001/heyaki #22：`enrollment_mode=password`、
  `RelayEnrollmentCredential`、`EnrollmentResult` 回传 leaf 指纹）：
  主视图 URL + 密码 secureInput；租户不出现在 UI（适配层空租户归一
  `"default"`，上游要求与 `enrollment_default_tenant` 严格相等）；TOFU
  pin 锚定注册交换所见 leaf 证书（relay 回传指纹 UPSERT 回写记录）；
  TOFU 首连窗口文案如实披露；密码擦除断言测试；错误码对齐
  （enrollment_password_rejected / enrollment_tenant_unknown / 限速原文
  可见）；配套 enrollment WSS 客户端切换借用 Runtime（HEY-20261006-002
  收口，`RelayEnrollRuntime` 句柄）。
  （实现与全部自动化验证完成，2026-10-08：IVA 三轮测试 49/49 全绿，
  三处缺陷（TOFU pin 不落库 / worker 内 Runtime 生命周期 / worker 名
  冲突）发现并修复；GUI 目视复核因本机无截图/输入注入工具未执行，
  补跑条件：图形会话运行 `build/debug/aki` → Settings → 中继区复核
  密码行/TOFU 披露/令牌高级路径，负责人 Linductor。）
- [ ] `M8-05` TURN 自动化（阶段 3；上游能力已随 pin 1b0447b 落地——
  HEY-20261007-002：relay_ice_config_v1 下发 + 快照计数器 +
  `merge_relay_ice_servers`）：选路零接线（上游 Node 自动合并，静态
  优先/过期剔除/心跳整体换新/断连回落静态）；静态 `ice-servers.txt` +
  TURN 高级区降级为高级覆盖（提示文案更新）；`RelayStatusView`/AppState
  `RelayStatus` 增下发 ICE 计数，设置页展示「TURN（中继下发）：N ·
  到期 HH:MM:SS」；凭据续期由 relay 心跳承载，无 Aki 侧定时器。
  （实现完成，2026-10-08；测试与回归见验证记录 M8-05 条。）

- [ ] `M8-06` 阶段 1 回归验证：全量 ctest 零回归 + CI 七项全绿 + GUI
  视觉复核（折叠开合/校验自动展开/深浅两档）；阶段 2/3 各自开工时另立
  回归项。
  （debug 48/48 与 CI run 37627726846/37630681885 已过，2026-10-07；
  GUI 视觉复核补跑条件见验证记录。）
- [x] `M8-07` 依赖升级 M8 前置解锁（2026-10-08）：Heyaki 7e9758a →
  1b0447b、Executor 同步升级 v0.6.0（更名 kairo，Aki 第一方 42 文件全量
  迁移 `executor::`→`kairo::`/include/CMake target/定时器与 `_ex` API）；
  移除 HEY-20261006-003 tsan 抑制并复跑 tsan；六条台账回写上游证据；
  supply-chain 审计登记（[heyaki-1b0447b-relay-upgrade.md]
  (../supply-chain/heyaki-1b0447b-relay-upgrade.md)）。
  （PR #73 / CI [run 37719878676]
  (https://github.com/Linductor-alkaid/Aki/actions/runs/37719878676) 七项
  全绿，Squash 合入 c92bb8a；本地 tsan 全量 48/48 零报告。）

- [ ] `M8-08` 注册/移除热生效（HEY-20261006-001 收口，2026-10-08）：
  `NodeSession::apply_relay_enrollment_now`（按 profile 首条有效记录镜像
  上游字段映射热更新；无记录传 nullopt 断开）+ enroll 任务成功后热连接 +
  remove 后热断开（失败经 RelayStatus.last_error 可见）+ 设置页全部
  「重启生效」文案废止（TURN 静态配置除外）+ 决策 3 修订。
  （实现完成，2026-10-08；测试与回归见验证记录。）

## 风险与阻塞

- **阶段 2/3 上游前置已落地**（2026-10-08 pin 1b0447b）：剩余风险从
  「上游未定型」转为「Aki 接入期语义实测冻结」（默认租户、错误码、ICE
  合并优先级、凭据续期），按决策 11 开工前冻结条款执行。
- **折叠区默认收起的可发现性**（阶段 1 过渡期租户/令牌仍必填）：缓解 =
  折叠标签明示内容清单 + 必填校验失败自动展开；阶段 2 主路径不再依赖
  折叠区必填项。
- **heyaki 侧 issue 互链**：HEY-20261007-001 已由用户提交并关闭
  （[heyaki #22](https://github.com/Linductor-alkaid/heyaki/issues/22)），
  台账已回填；002 无独立 issue（随 #22 衔接落地，台账已注明）。

## 测试与退出条件

- 退出-1（阶段 1）：M8-01/M8-02/M8-03 完成且 IVA 测试证据回填；全量
  ctest 零回归；GUI 视觉复核通过或如实记录补跑条件。
- 退出-2（阶段 1）：CI 七项全绿（PR 门禁）。
- 退出-3（阶段 2，届时细化）：全新 Aki + `--init` relay 只凭地址 + 密码
  注册成功；错密码/限速/未启用密码模式反馈可读；凭据不落盘不进日志；
  token 高级模式回归不变。
- 退出-4（阶段 3，届时细化）：打洞失败自动走 relay 下发 TURN（全程无
  TURN 字段）；静态文件高级覆盖语义与来源展示正确；短时效凭据过期行为
  明确可观测。

## 验证记录

### 2026-10-08：M8-08 注册/移除热生效（HEY-20261006-001 收口）

- 环境：Ubuntu 24.04 / GCC 13.3.0 / CMake 3.28.3 / debug + tsan 预设；
  heyaki 1b0447b。
- 实现：`NodeSession::apply_relay_enrollment_now`（+ profile_ 成员与
  构造/移动传递；字段映射镜像上游 load_relay_config_from_profile，有
  pin 时 tls_verify_peer=false，无有效记录传 nullopt）+ HostRuntime
  enroll 任务成功后热连接 / remove 后热断开（失败进
  RelayStatus.last_error，操作本身仍成功）+ 决策 3 修订 + 文案废止
  （"Enrolled."/"Relay enrollment removed."/hint 去 restart 句）。
- 测试（Independent-Verification-Agent，**token 模式——tsan 档保留
  覆盖**）：password e2e 新增 3 用例/116 断言——热连接（同会话 apply →
  ready ~10ms，服务器 login_ok/active_sessions 佐证；同配置二次 apply
  no-op 不拆重建）、热断开（撤销 → apply → disabled 亚毫秒，上游
  清空语义 url/tenant/last_error/ICE 计数归零 + 服务器会话归零；关闭后
  apply 以 node_not_running 可见拒绝——IVA 核实上游 shutdown 尾部
  impl_.reset() 后的确定性拒绝语义，任务描述的「关闭竞争 no-op」仅限
  在途竞态窗，已按实际语义断言）、错误可见（坏记录热更新 degraded +
  wss_connection_failed，可恢复可替换，空记录 nullopt 安全降级）；
  test_host_relay_config 增进程内 relay fixture（+310 行）——Host 级
  全闭环：enroll → 装配期 NodeSession 热连接（SetRelayStatus + sweep
  收敛 ready）→ remove → 热断开 + revoked 持久化 + 服务器归零
  （36→52 断言）。tsan 档 4 用例/152 断言实测通过零报告。
- 全量：`ctest --preset debug` **49/49**；password e2e 二进制 9 用例/
  259 断言复跑通过。
- 残余未验证（如实登记）：asan/ubsan/Windows 档归本 PR CI 门禁；
  host 层 hot-apply/hot-disconnect 失败分支仅 profile 读失败或 node
  已停时可达（健康态公开 API 不可达），未注入；UI 文案变更经 diff
  核对与全量 ui 测试回归，无 GUI 自动化断言。

### 2026-10-08：M8-05 relay 下发 TURN 的消费与可观测（阶段 3）

- 环境：Ubuntu 24.04 / GCC 13.3.0 / CMake 3.28.3 / debug + tsan 预设；
  heyaki 1b0447b。
- 实现：`RelayStatusView` +4 ICE 计数字段（relay_status() 投影）、
  AppState `RelayStatus` 同名字段（make_relay_status 透传）、设置页
  「TURN（中继下发）：N · 到期 HH:MM:SS」行 + TURN 高级区覆盖语义
  文案 + i18n；选路/续期零接线（上游 Node 合并 + 心跳换新，决策 11
  阶段 3 冻结条款引用 node.cpp 语义）。
- 测试（Independent-Verification-Agent）：`test_app_state` 扩展
  SetRelayStatus 用例（首推非零计数逐字段/同值幂等/换值整体替换恰发
  一次，41 断言含 4 条字段类型静态断言）；password e2e 新增独立 ICE
  用例（**token 模式注册——不触发 Argon2id，tsan 档保留覆盖**；fixture
  扩展 turn_credentials_enabled + secret 文件 + advertised turn_udp，
  上游 m4_node_turn_ice_test 同型）：login 下发
  `updates=1/servers_active=1/expires−now==600s==TTL` 精确、heartbeat
  换新 `updates=2` 计数不漂移、rejected=0、凭据材料无字段面（静态
  断言）、关闭序闭合（29 断言）。tsan 档实测执行通过（4 密码用例按
  既定 kTsanBuild 跳过，本用例不受影响）。
- 全量：`ctest --preset debug` **49/49**（条目数持平——两处改动均在
  既有二进制内）。
- 残余未验证（如实登记）：make_relay_status 透传为 .cpp 内部函数，经
  代码检视确认（两端已测，低风险）；设置页展示行无 GUI 测试面
  （GUI 目视复核并入 M8-01/04 的补跑项）；静态覆盖 + 下发的实际选路
  合并属上游行为（上游 m4 测试覆盖，Aki 零接线）。

### 2026-10-08：M8-04 密码模式注册主路径（阶段 2）

- 环境：Ubuntu 24.04 / x86_64 / GCC 13.3.0 / CMake 3.28.3 / debug
  preset；heyaki 1b0447b（密码准入/enrollment 凭据/借用 Runtime/回传
  指纹）。
- 实现：`heyaki/session/relay_enrollment.hpp`（凭据二选一 + 密码 256B
  上限 + 空租户归一 `"default"`、统一 `credential_exchange`、TOFU 首连
  `tls_verify_peer=false`、`RelayEnrollRuntime` 借用句柄、双凭据全退出
  路径原位擦除、TOFU 指针 UPSERT 回写）；`HostRuntime` 共享注册管线
  `launch_relay_enroll` + `enroll_relay_with_password` 入口 + 关闭序句柄
  销毁；`UiActions::enroll_relay_password` + `main.cpp` 绑定 +
  `settings_page` 密码主路径（令牌草稿非空走高级路径）+ i18n 5 词条；
  决策 11 阶段 2 冻结回填、UI 规范 2026-10-08 落地记录、台账
  HEY-20261007-001 与 HEY-20261006-002/003 收口。
- 测试（Independent-Verification-Agent 三轮，渐进发现并修复三处产品
  缺陷后全绿）：
  - **缺陷①（TOFU pin 不落库）**：上游记录写入只取 `config.relay_pin`
    （relay_enrollment_client.cpp:382），返回值 `relay_certificate_sha256`
    不持久化——密码模式「能注册、不能连接」。修复：Aki 层读回记录
    UPSERT 同代回写（IVA 指出任务简报与 pinned 源码不符，已核实）。
  - **缺陷②（worker 内 Runtime 生命周期）**：在 executor worker 内
    创建/析构借用 Runtime 使 teardown 自等待，后续 `Node::shutdown`
    确定性超时（IVA 对照实验隔离归因）。修复：`RelayEnrollRuntime`
    句柄主线程构造/关闭序销毁（任务 future 消费后、executor 回收前）。
  - **缺陷③（worker 名冲突）**：句柄缺省 worker 名与 NodeSession 同为
    `"heyaki-asio"` → `asio_worker_start_failed` → 两注册入口全拒绝。
    修复：显式 `worker_name="aki-relay-enroll"`。
  - 新增/扩展测试：`test_relay_integration`（validate 二选一/256B/归一
    6 SECTION + 擦除镜像 7 SECTION，18 用例/210 断言）、新
    `test_relay_password_e2e_loopback`（owned 成功 TOFU pin 对拍/借用
    成功/错密码 `enrollment_password_rejected` 零落库/句柄+NodeSession
    共存且自动登录收敛 ready 后干净关闭；4 用例/84 断言）、
    `test_host_runtime`（密码入口静态拒绝 + 异步失败 <2s + 注册后
    `node_stopped`；199 断言）、`test_host_relay_config`（token 入路
    关闭断言补强；36 断言）。
  - 全量 `ctest --preset debug` **49/49**；生命周期敏感二进制复跑
    （host_relay_config ×2 / host_runtime ×2 / password e2e ×1）无抖动。
  - **在途关闭注入（HEY-20261006-002 补跑条件兑现，第 4 轮追加）**：
    第 5 e2e 用例（HangingTcpListener 悬置 handshake，上游 borrow 测试
    同型）——enroll 在途时宿主关闭有界收敛（实测 10s ≤ 15s，
    shutdown Completed / 生命周期 Stopped），交换终态
    `wss_connect_wait_timeout` 可见、凭据擦除、零落库、句柄销毁有界
    （2.0s）、二轮注册毫秒级干净失败、无进程残留；password e2e 达
    5 用例/107 断言，全量维持 **49/49**。「worker 回收提前打断」路径
    未观察到（需上游可中断等待面），台账 002 残余声明如实记录。
    本注入用例的悬置 listener 为 POSIX 实现，**Windows 编译面跳过**
    （MSVC 无 arpa/inet.h；CI Windows 档不含本用例）——Windows 等价
    （Winsock 悬置 listener）为补跑项，负责人 Linductor。
- CI 时序处置（2026-10-08 第二轮 run 37732041555）：①Windows 档
  `pw_elapsed < 2s` 断言余量不足（实测 2027ms），放宽至 `< 5s`（语义
  边界为「远低于 transport 12s 上界」，非精确 2s）；②tsan 档密码
  交换用例失败——上游 `PasswordSecurityPolicy` 将 Argon2id 参数下限
  钉死（≥2 ops / ≥64MiB，`validate_security_policy` 强制），TSAN 放大
  下双端派生/校验超注册交换接收超时（服务端 `enroll_ok=1` 而客户端
  `wss_receive_timeout`）——**密码交换 4 用例在 TSAN 构建下编译期
  跳过**（`__SANITIZE_THREAD__` 守卫），覆盖由 debug/asan/ubsan 与
  Windows debug 档承担；注入用例（不触达密码交换）TSAN 下保留。
  tsan 档的密码交换覆盖为已登记限制，补跑需上游放宽策略下限或提供
  测试注入面。
- 残余未验证（如实登记）：pin 回写失败分支（读回缺失/UPSERT 失败）仅
  静态走查；GUI 目视复核未执行（补跑条件见 M8-04 工作项注）。

### 2026-10-08：M8-07 依赖升级（Heyaki 1b0447b + Executor v0.6.0 kairo）

- 环境：Ubuntu 24.04 / x86_64 / GCC 13.3.0 / CMake 3.28.3；debug 与 tsan
  预设；heyaki `1b0447b`（v1.2.0-13）、executor `d9602ea`（v0.6.0，
  更名 kairo）、heyaki 内部依赖经 `scripts/fetch_third_party.sh` 刷新
  （executor d9602ea、vendored openssl 3.5.9——后者不进 Aki 构建图，
  configure 输出核实系统 OpenSSL 3.0.13 仍被 find_package 使用）。
- 迁移：第一方 42 文件 `executor::`→`kairo::`/`<executor/…>`→`<kairo/…>`
  （残留 grep 零命中）；`submit_periodic_with_handle`→`submit_periodic`
  （产品 4 处 + 测试 6 处）、`initialize_ex`→`initialize`、
  `wait_for_completion_ex`→`wait_for_completion`（executor_owner + 3 测试
  文件）；CMake `executor::executor`→`kairo::kairo`（app/persistence）、
  SYSTEM include 目标 `executor`→`kairo`。configure 三方一致校验通过。
- 测试（Independent-Verification-Agent，两轮）：
  - 第一轮回归：debug 全量 47/48——发现 **上游 176db92（heyaki #15）
    有意变更与 Aki 既有断言冲突**：会话 parked 传输的取消从「拒绝
    （peer_session_missing）」改为「成功并产生一次 cancelled 终态」；
    `test_basic_communication_loopback` 旧断言（两路径均拒绝取消）约
    50% 竞态触发失败（失败签名 terminal=0 paused=1）。IVA 给出决定性
    归因链（pin 区间提交考古 + 新旧行为对照）。处置：断言按收敛路径
    拆分——parked 路径 `REQUIRE(cancel)` + 等待 cancelled 终态（与 Aki
    UI 契约 DEC-013⑥「Paused 行 Cancel = 直接终态入口」一致）；expired
    路径保持拒绝。
  - 第二轮复验：debug 直跑 10/10 + ctest 通过（原失败率 ~50% 下 10 连过
    概率 ~0.1%，统计上确认两路径覆盖）；tsan 全量（本机内核高熵 ASLR
    需 `setarch -R` 运行，per-process 关闭，不改系统）**48/48 零
    ThreadSanitizer 报告**；HEY-20261006-003 原报出点 test_host_runtime
    专项 3 次复跑干净（两条抑制移除后）。tsan `test_database_worker`
    首轮为 0 字节中断产物（全量并行构建期链接被中断），重建后 8 用例
    /102 断言通过零报告。
- 未验证（如实降级）：CI 七项门禁以本 MR 的 run 为准（链接回填下方）；
  本地 tsan 经 setarch 关 ASLR 运行，CI runner 环境不同但插桩语义等价。

### 2026-10-07：M8-01~M8-03 阶段 1 实现与验证

- 环境：Ubuntu 24.04 / x86_64 / GCC 13.3.0 / CMake 3.28.3 / Catch2 v3.9.1
  （pinned），debug preset（`build/debug`），分支
  `feat/ui-relay-advanced-fold`（基线 master `6afe62a`）。
- 实现：`ui/components/fold.hpp`（新增折叠开关组件，chevron
  `f077`/`f054`）、`ui/pages/settings_page.cpp`（中继未注册态主视图收拢 +
  折叠区 + 校验失败自动展开）、`ui/pages/main_window.hpp`
  （`settings_relay_advanced_open`）、`ui/i18n.hpp`（+1 词条）、
  `heyaki/session/relay_enrollment.hpp`（`enroll_relay_profile` 非 const
  引用 + `bootstrap_token` 全退出路径原位擦除）；
  `app/lifecycle/host_runtime.cpp` 调用点适配（lvalue 传递 + 自有 lambda
  副本擦除保持）。chevron 码点以最小 cmap 解析器对 pinned 捆绑 FA7
  Solid 字体实证存在（映射码位 2929），登记于 UI 规范 §2.6。
- 测试（Independent-Verification-Agent 编写并执行）：
  `tests/unit/test_relay_integration.cpp` 新增 "enroll_relay_profile
  scrubs the bootstrap token on every failure path"（5 SECTION × 5 断言
  = 25 断言：URL 非 wss/租户空/CA 缺失/非 PEM/不可达 relay 各退出路径
  token 原位为空 + nullopt + error 非空 + profile 零落库）；
  `tests/integration/test_relay_e2e_loopback.cpp` 成功路径 +2 断言
  （双端注册成功后调用方持有 token 为空，既有 99 断言闭环不变）。
  **变异验证**：临时置空擦除逻辑后新断言单测 5 failed / e2e 1 failed，
  字节级还原（md5 对拍）后复验全绿——断言真实绑定行为。
- 全量回归：`ctest --preset debug` **48/48 通过**（unit 33 + integration
  14 + smoke 1，202.28s；test_relay_integration 17 用例/156 断言、
  test_relay_e2e_loopback 1 用例/101 断言）。
- 未验证（环境限制，如实降级）：①GUI 视觉复核（折叠开合/校验失败自动
  展开/深浅两档）——本机无截图与输入注入工具，且 GUI 宿主无 argv 注入
  使用真实数据根，不冒充已验证；补跑条件：本机图形会话运行
  `build/debug/aki` → Settings → 中继区目视复核（折叠默认收起、必填
  校验失败自动展开、令牌掩码、深浅两档），负责人 Linductor。
  ②ASAN/UBSAN/TSAN 与 Windows/打包档：[PR #72 CI run 37627726846]
  (https://github.com/Linductor-alkaid/Aki/actions/runs/37627726846)
  七项全绿（Linux debug/asan/ubsan/tsan、Windows debug、deb/setup 打包）。
- 同步：DEC-028（决策 11 + 修订行 + 关联）、UI 规范（§2.6 码点登记/
  §4 映射/2026-10-07 落地记录）、总计划（当前状态/里程碑索引 M8）、
  HEY 台账（HEY-20261007-001/002）、本里程碑文档。
