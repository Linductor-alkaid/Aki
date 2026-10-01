# Heyaki 文件协商终态修复升级审计

> 状态：Completed；日期：2026-10-02；负责人：Linductor；工作项 M5-40。

## 来源与差异

从 v1.1.1 @ 1ceb42c7b244e950ebeeb10edc84b6d83e423626 升级至官方 master
的 516815cbfb76f93f60acd4b58e5b6a7976e4417f，git describe 为
v1.1.1-2-g516815c；没有新发布 tag，不称为 v1.1.2。
[上游 #14](https://github.com/Linductor-alkaid/heyaki/pull/14) 已 squash 合入，
关闭 #13 / HEY-20261001-001。差异仅两个提交：release 文档及文件 offer 修复。

新增公开 NodeConfig::file_offer_timeout，0 使用 30s 默认协商窗口。未收到
FILE_ACCEPT 的 push 在上游维护 tick 到期时发布一次 failed（timeout /
offer_expired / deadline_exceeded）；已接收文件不应用该 offer deadline。
接收确认前不调度正文分块读取/发送；源文件元数据探测及校验不在此保证内。
没有 wire、grant、接收根或控制能力扩展。
Aki 仅在 NodeSession::Options 映射该 std::chrono 参数；Host 默认值不变。
不创建 Aki timer/worker/队列，不修改 Heyaki 源码。

## 供应链闭包

LICENSE、third_party/dependencies.lock、licenses.lock 与
transitive-dependencies.lock 相对旧 pin 无差异，MIT 与既有许可证集合保持。
Executor 仍为 74a94198，EUI-NEO 仍为 b9032a8；既有授权的 EUI 构建补丁保留。
官方 fetch --check --all 本地通过，configure 校验精确 pin 与单图 Executor。

上游 merge head 的 CI run 36889253791 尚非全绿：UBSAN 的 TUI service
harness 报 rpc echo 未回传；Windows Debug 的 network matrix 报 relay_direct
file / TURN authentication 失败。没有把它们表述为 UBSAN runtime 检出或
Aki 构建失败；Aki 的实际 gate 必须独立验证。该 CI 不能支持全拓扑兼容声明。

## 回归范围与已知限制

新增真实单 Executor / 双 borrowed Runtime / fresh profile 回环：单侧策略
拒绝或接收根缺失，一次失败、同 TransferId、无落盘/无 grant；超时后再次
取消 API 拒绝，超时前取消只产生一次 cancelled；已接受双向文件、图片信封、
连接重启、反向密码授权与关闭沿原回归继续覆盖。

首轮 Debug 全回环通过；ASAN 的消息先被拒绝、再 push 同一会话的序列未
得到 failed。定向诊断为 terminal=0、paused=1、offered=1、linked=0：该会话
丢失，传输按既有契约停为 Paused，不是第一次 offer 无界等待。上游 #14
已列出此后续限制：拒绝后复用缓存的关闭物理通道可能破坏整个会话，book
中的 paused transfer 取消也可能同步拒绝。回归明确要求该序列在预算内
failed 或 Paused+disconnected，保留这一限制，不把它算作会话保持已修复。
第一次独立 offer 的回归仍严格要求一次 failed，不接受 Paused 替代。

本地 Debug aki 构建通过；完整基础回环 Debug 1/1（47.99s）、UBSAN 1/1
（71.24s）通过。ASAN 首轮因终态假设失败，第二轮因暂停回调早于连接
快照而失败；改为等待可观察快照后完整回环 1/1（53.45s）通过。两次失败
记录保留，不以即时回调推断连接快照已更新。新独立 file_offer 定向
Debug 与 ASAN 各 86 assertions / 2 cases 通过。后续会话问题另登记 Heyaki #15。

Aki 精确 head `ae08acef46490d2a2d6e5f5c5cca0499b123dbbf` 的
[CI run 36895691566](https://github.com/Linductor-alkaid/Aki/actions/runs/36895691566)
七项 completed/success（Linux Debug/ASAN/UBSAN/TSAN、Windows Debug、deb、setup）。
本机 TSAN 的 runtime mapping 限制由 CI runner 完成此门禁；不扩大为 UI
运行期 TSAN 声明。PR #65 squash 合入 084c39c，M5-40 依赖接入 Completed。
两端安装版复验仍归
M5-34/37~39，负责人 Linductor/操作者，条件为新包与双端在线。
