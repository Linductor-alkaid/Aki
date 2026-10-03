# Heyaki 文件取消重入与 Executor 同步升级审计

> 状态：Completed；日期：2026-10-03；负责人：Linductor；工作项 M5-43。

## 来源与差异

Heyaki 516815cbfb76f93f60acd4b58e5b6a7976e4417f →
7e9758a370d047db1e511b50627c7f3b3edc78e2（v1.1.1-4-g7e9758a）。
官方 [PR #17](https://github.com/Linductor-alkaid/heyaki/pull/17) 修复
[HEY-20261002-002](../heyaki_feedback/ledger.md)：发送 abort/frame 可同步
退役会话，原 sender 裸指针和 FileService 所有权因此失效。修复在可重入
发送前快照/退役状态，发送后按稳定 TransferId 重查，Node 调用保留 service
强引用。上游新增三项确定性回归覆盖 sender cancel、receiver failure、prune；
run 37024892121 十二项 CI 全绿。公开 include/heyaki 无差异，无 wire 变更。

上游同时升级 Executor，2026-10-03 用户明确授权 Aki 同步：
74a94198fbe0f2a4081cd260658a26f969986870 →
e2362736c697cb215e914b3f1cdfeedb0c1544d6（v0.5.2-20-ge236273）。
采用此精确上游要求，不追随之后的 master/tag。变化包括 dependency-driven
调度、parked 超时/关闭结算、生命周期/串行上下文/通信与构建修复，以及
事件驱动 timer 唤醒和固定网格周期；公开 API 迁移指南说明现有调用无需修改。
Aki 使用公开 facade，周期任务仍属软调度，取消不能清除已排队/执行的回调。
不新增并发路径，不宣称性能或实时保证。

## 供应链闭包

Heyaki LICENSE、licenses.lock、transitive-dependencies.lock 无差异，
third_party/dependencies.lock 唯一变更为 Executor pin。Executor MIT LICENSE
无差异；EUI-NEO/SQLite pin 与既有授权 X11 构建补丁保持。
Aki 侧 submodule 和 JSON lock 同步，configure 验证 Aki lock、Heyaki lock、
Heyaki Executor checkout 三方一致、构建图只编译一份 Executor。

Executor 能力反馈台账无未关闭条目；审计使用新版 API、迁移指南、集成
指南的 by-api router / scheduling card，未修改或读取 Executor 实现源码。

## 验证与限制

真实 Aki file_offer 回环保持原断言：取消早于期限、同一 cancelled 终态、
重复取消拒绝、零 grant、无落盘、Node 和唯一 Executor orderly shutdown。
Debug 全量、ASAN/UBSAN 与最新 head 七项 CI 待执行并追加准确结果。
本机 TSAN 曾受 unexpected memory mapping 限制，不能标通过；由 CI runner
补跑，负责人 Linductor。安装版 GUI 与 Windows 双端验收由操作者在同版
新包和 LAN 双端在线后执行，归 M5-34/41/42，不能以上游 CI 代替。

Heyaki #15 / HEY-20261002-001 仍 Open，拒绝业务后后续 push 的断连/Paused
边界保持，未借本次修复关闭。

2026-10-03 本地验证（Ubuntu 24.04 / x86_64 / GCC 13.3 / CMake 3.28.3）：
`bash third_party/heyaki/scripts/fetch_third_party.sh --all` 及 `--check --all`
通过。Debug 首轮因本机缺 xkbcommon/xkbcommon.h 构建失败；沿既有本机
限制，三档分别执行 `cmake --preset <debug|asan|ubsan> -DGLFW_BUILD_WAYLAND=OFF`
和 `cmake --build --preset <preset> -j 4`，全目标含 aki GUI 均构建通过。
CI 不关闭 Wayland、不改变项目预设。

`ctest --preset debug --output-on-failure --timeout 180`：45/45 返回成功
（192.16s）；五个旧网络用例有显式 skip，不能算作其链路已验收。
真实 basic communication 回环无 skip，250 assertions / 6 cases（47.51s）。
ASAN/UBSAN 分别执行：

```bash
ctest --preset <preset> -R 'test_basic_communication_loopback|test_app_state$|test_app_managers$|test_reconnect_loop$|test_database_worker$|test_host_runtime$|test_peer_sessions_pipeline$' --output-on-failure --timeout 180
```

均 7/7 返回成功（64.99s / 76.50s）。
两档 basic 回环均 250 assertions / 6 cases（51.38s / 61.26s），ASAN 无内存报告。

UBSAN 仍打印 M3-07 已登记的 libdatachannel/usrsctp 非对齐访问，
sctptransport.cpp:732-735 的 byte buffer 不满足 sctp_reset_streams 的 4 字节
对齐；同一第三方 pin 未变。以 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
build/ubsan/tests/test_basic_communication_loopback 'Cancelling an unanswered offer*'`
复核，exit 1 于该第三方对齐错误，尚未执行全部取消断言。保留默认 recover
与严格运行两份证据，不新增抑制、不宣称 UBSAN 零诊断或该缺陷已修复。
此限制沿 [M3-07 既有登记](../plans/m3-heyaki-integration.md) 跟踪，负责人
Linductor/Heyaki 上游；补跑条件为第三方正式对齐修复被独立消费后重跑严格 UBSAN。
与 Heyaki #16 的 sender/service UAF 分开；取消修复的 ASAN 验证已通过。

日志位于本机 /tmp/aki-m5-43-*（临时保留至本轮结束，非持久附件）；准确
命令、计数和限制在此留档。文档链接 191 项无断链，git diff --check 通过。
最新 Aki head 七项 CI 尚待验证，M5-43 维持 In Progress。

2026-10-03 依赖接入闭环：精确 Aki head
6eaf5ea0bce71c739e9e15389f95d1304c5d7600 的 [CI run 37124555173](https://github.com/Linductor-alkaid/Aki/actions/runs/37124555173)
七项 completed/success（Linux Debug/ASAN/UBSAN/TSAN、Windows Debug、
Ubuntu 20.04 deb、Windows setup）。[PR #69](https://github.com/Linductor-alkaid/Aki/pull/69)
Squash 合入 84b8c3cd2b259cb5f474b18516c93e84675afecd；远程/本地依赖分支
已删除，主目录 master 已 fast-forward 同步且干净。M5-43 Completed，
HEY-20261002-002 Resolved；已有第三方 UBSAN 对齐限制和 Heyaki #15 保留。
该结论只关闭取消重入修复的依赖接入，不关闭 M5-34/41/42 的桌面双端验收。
