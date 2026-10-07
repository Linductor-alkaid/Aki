# M8：中继接入简化（地址 + 密码）

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M7 主体能力（enrollment/发现/建链/ICE 已交付；M7-08/09 收口并行）、
> [DEC-028](../decisions/DEC-028-relay-cross-subnet.md)（2026-10-07 修订：
> 决策 11 分阶段简化）
> 建议发布点：v0.8.0
> 更新日期：2026-10-07（创建）

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
- [ ] `M8-04` 密码模式注册主路径（阶段 2，Blocked：HEY-20261007-001 上游
  未落地）：主视图 URL + 密码 secureInput；租户不出现在 UI（Adapter 层
  落默认）；TOFU pin 锚定注册交换所见 leaf 证书；TOFU 首连窗口文案如实
  披露；密码擦除断言测试；错误码对齐（密码错误/限速/未启用密码模式）。
- [ ] `M8-05` TURN 自动化（阶段 3，Blocked：HEY-20261007-002 上游未落地）：
  relay 凭据自动参与选路；静态 `ice-servers.txt` 降级为高级覆盖（合并/
  优先级跟随上游语义）；设置页展示生效 ICE 配置与来源；短时效凭据续期/
  过期语义接入与测试。
- [ ] `M8-06` 阶段 1 回归验证：全量 ctest 零回归 + CI 七项全绿 + GUI
  视觉复核（折叠开合/校验自动展开/深浅两档）；阶段 2/3 各自开工时另立
  回归项。

## 风险与阻塞

- **阶段 2/3 上游前置**（HEY-20261007-001/002）：上游语义（默认租户、
  错误码、ICE 合并优先级、凭据续期）未定型，决策 11 对应条款为暂定
  默认值；开工前按实测冻结，避免预实现上游想象。
- **折叠区默认收起的可发现性**（阶段 1 过渡期租户/令牌仍必填）：缓解 =
  折叠标签明示内容清单 + 必填校验失败自动展开；阶段 2 主路径不再依赖
  折叠区必填项。
- **heyaki 侧 issue 互链未建**：HEY-20261007-001/002 的上游 issue 需用户
  授权后在 heyaki 仓库提交后回填链接（独立管理上游纪律）。

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
