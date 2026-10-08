# Heyaki 1b0447b 升级审计（relay 密码准入 / 运行期更新 / ICE 下发）

> 日期：2026-10-08（M8-07）
> 变更：Heyaki `7e9758a` → `1b0447b`（v1.1.1-4-g7e9758a → v1.2.0-13-g1b0447b）；
> Executor（更名 kairo）`e2362736` → `d9602ea6`（v0.5.2-20 → v0.6.0）
> 双侧同步 pin。

## 升级动机

上游落地了 Aki 反馈台账六项记录对应的修复/能力（M8 阶段 2/3 前置）：

| 上游提交 | 内容 | Aki 台账 |
| --- | --- | --- |
| `3763dfd`（+`51338f6`/`82f10e6`/`1a2acdd`/`d869bb6`/`482ff8a`/`7dcf115` 收尾） | relay 密码准入模式（`enrollment_mode = token\|password\|closed`、Argon2id 挑战绑定证明、`EnrollmentResult` 回传 leaf 证书 SHA-256）与 `--init` 首跑引导 | HEY-20261007-001（[heyaki #22](https://github.com/Linductor-alkaid/heyaki/issues/22)） |
| `1b0447b` | 控制面下发短时效 TURN/ICE 凭据（relay_ice_config_v1；快照计数器；`merge_relay_ice_servers`） | HEY-20261007-002 |
| `82147d5` | `Node::update_relay_config`（运行期 relay enrollment 更新） | HEY-20261006-001（[heyaki #19](https://github.com/Linductor-alkaid/heyaki/issues/19)） |
| `d5be571` | enrollment WSS 客户端借用宿主 Runtime（`runtime_borrowed`） | HEY-20261006-002（[heyaki #20](https://github.com/Linductor-alkaid/heyaki/issues/20)） |
| `ab6418c` | PairingService 审计计数器跨线程同步 | HEY-20261006-003（[heyaki #21](https://github.com/Linductor-alkaid/heyaki/issues/21)；tsan 抑制随本升级移除） |
| `176db92` | 会话结束后 parked transfer 保持可见可取消 | HEY-20261002-001（[heyaki #15](https://github.com/Linductor-alkaid/heyaki/issues/15)） |

其余为 heyaki v1.2.0 既有内容（Android core-library port、kairo 0.6.0 迁移
窗口），在 Aki 构建面不新增编译单元。

## 版本差异与许可

- Heyaki：MIT，许可文件路径不变（`third_party/heyaki/LICENSE`）；上游
  v1.2.0 之后 13 个提交，许可证无变化。
- Executor → kairo：MIT，同仓库同许可（`third_party/executor/LICENSE`）；
  v0.6.0 为破坏性窗口（项目更名 + 兼容层清理，见其 `docs/MIGRATION.md`）。
- heyaki lock 新增 vendored `openssl` 3.5.9（`45e844f`）条目：属 heyaki
  独立构建/打包面；**Aki 构建图内 heyaki 仍经 find_package 使用系统
  OpenSSL（本机 3.0.13，满足其 ≥3.0 要求）**，vendored 副本未进入 Aki
  构建图，无双 OpenSSL 链接冲突（configure 输出核实）。

## Aki 侧迁移（kairo v0.6.0 破坏性变更）

- `#include <executor/…>` → `<kairo/…>`、`executor::` → `kairo::`：
  第一方 42 文件全量机械迁移（残留 grep 零命中）。
- 定时器 API：`submit_periodic_with_handle` → `submit_periodic`
  （返回 `TimerHandle` 语义不变），产品 4 处 + 测试 6 处。
- `_ex` 兼容层：`initialize_ex` → `initialize`、`wait_for_completion_ex` →
  `wait_for_completion`（Result 返回型接管主名；`executor_owner.hpp` 与
  3 个测试文件）。
- CMake：`executor::executor` 链接 → `kairo::kairo`（app/persistence）；
  SYSTEM include 属性目标 `executor` → `kairo`（根 CMakeLists，路径
  `third_party/heyaki/third_party/executor/include` 不变）。
- 未使用面（无迁移成本确认）：字符串任务 ID/`cancel_task`、4 参
  `submit_auto`、`push_task`、`is_lock_free`、`memory_locked`（Aki 零命中）。
- 新增 Scheduling Runtime（deadline/QoS/affinity/resources）为非破坏性
  增量，Aki 暂不启用，按 EXEC 条目需要再评估。

## 验证

- 上游门禁：heyaki master `1b0447b` ci/android 工作流全绿（ci run
  37710735237/android run 37710735202，2026-10-08 核实）。
- Aki 回归：本机 debug 全量 ctest（Independent-Verification-Agent 执行，
  计数见 M8-07 验证记录）+ tsan 预设复跑（HEY-20261006-003 抑制移除后）；
  CI 七项门禁以本升级 MR 的 run 为准（链接回填 M8-07 验证记录）。
- 三方一致性：configure 校验（Aki lock / heyaki lock / heyaki checkout
  executor 同 `d9602ea6`）通过。

## 风险与后续

- 第一方语义无变化声明以全量回归 + 七项 CI 背书；kairo 0.6.0 行为变化中
  与 Aki 相关者（定时器事件驱动、periodic 网格锚定）见其 CHANGELOG
  0.5.3/0.6.0 节，均非 Aki 依赖的非契约观察。
- 新能力（密码准入、运行期更新、ICE 下发、借用 Runtime）的消费为 M8
  阶段 2/3 与热生效专项，不随本升级落地；台账各条在 Aki 接入复验前保持
  未关闭。
- `AGENTS.md`/工程规范中「Executor」术语与 skill 路径随上游更名出现
  词汇漂移（`third_party/executor` 路径未变，仓库名未改，仅为品牌更名）：
  是否同步措辞留待用户决定，不在本 MR 范围。
