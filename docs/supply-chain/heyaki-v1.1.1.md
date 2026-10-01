# Heyaki v1.1.1 升级审计

> 状态：In Progress；日期：2026-10-01；负责人：Linductor；工作项：M5-33。

## 固定来源与差异

从 e114508ab32d496d52e9db9bac26eb1cc88c4ae7 升级至官方 tag v1.1.1
的 1ceb42c7b244e950ebeeb10edc84b6d83e423626。
[发布说明](https://github.com/Linductor-alkaid/heyaki/releases/tag/v1.1.1)
列明 pair_peer 返回 Result<RequestId>、有界终态、已授权会话修复、
receiver approval 与显式 basic_communication。源 API 变化只在 Aki
Heyaki Adapter 消费；基础通信遵循 DEC-023。gateway 新能力保持关闭。

## 许可证和依赖闭包

Heyaki 仍为 MIT；官方 dependencies.lock、licenses.lock 与
transitive-dependencies.lock 相对旧 pin 无差异；Executor 仍为
74a94198fbe0f2a4081cd260658a26f969986870，保持单图三方一致性校验。
libdatachannel 仍为 v0.23.2，官方 fetch 脚本应用并验证已纳入 release 的
synchronized_stored_callback copy/move 同步补丁。Aki 不直接修改 vendor。
官方补丁 SHA256 为 3730b39b9203c273e7fcced398747929ad2ed7cb3d989f464dbbdf2a0d3c00a2。
本地官方 fetch --all 应用补丁后，--check --all 全部通过；configure
验证三方 Executor pin、Aki lock，打包登记 40 份许可证文本。
独立核实上游 release commit 的 CI run
[36831912768](https://github.com/Linductor-alkaid/heyaki/actions/runs/36831912768)：
12/12 jobs completed/success，包含三种 sanitizer、Windows Debug/Release、
Ubuntu 20.04、supply-chain、coturn。高级服务拒绝覆盖来自该 pin 的
M5BasicCommunicationTest.BasicSessionsKeepRpcEventsStreamsShellGatewayGrantOnly
等真实上游测试；不将其称为 Aki 控制 UI 或目标设备实测。

## 适配与验证

NodeSession 暴露独立 basic_communication/policy_scopes/authorized_scopes；
普通密码提交使用上游有界 admission，允许 restricted 或 authorized 会话。
单 Executor 双节点回归发现 worker_name 原来仅传入 NodeConfig，borrowed
Runtime 创建时未消费；现直接传入 Runtime::create_borrowed，确保两个
blocking worker 名互异，不引入新 owner 或自行管理线程。
NodeConfig 新成员默认值与上游一致，Aki Host 明确 opt-in 基础通信。
移除 HEY-20260930-004 的精确抑制，保留已有 usrsctp 上游抑制。

待执行：无 grant 双端消息/文件、单侧策略拒绝、密码反向授权/错误口令、
关闭回调 TSAN，七项最新 head CI，安装版 Linux/Windows 双机测试。
未执行项负责人 Linductor；条件为本地依赖和 CI runner 可用，两端设备
安装同版新包上线。不得将上游 issue 关闭替代这些证据。

## 已知后续问题

单侧 basic=false 的文件 push 可入队而无有界终态；Debug 可显式取消，
ASAN 观察到取消 admission 被拒。详见 HEY-20261001-001 / Heyaki #13。
该限制未被升级掩盖，M5-34 文件拒绝完成语义保持未完成；测试对这一路径
只声明无落盘/无授权、取消 admission 可见和 shutdown，不声明已修复。

2026-10-01：Debug 全量构建通过；首轮 ctest 43/44，旧“完成后重复应拒绝”
断言依据 v1.1.1 更新为续期结果后，目标 53 assertions 通过；基础通信
110 assertions 通过。ASAN 基础回环 + Adapter 2/2 通过（46.94s）。
本地 TSAN 编译通过但运行前 unexpected memory mapping，未验证；
最新 Aki CI 与安装版验证仍待执行。原有网络 skip 列表见 M5 记录。
