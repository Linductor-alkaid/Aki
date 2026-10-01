# EUI-NEO 缺陷与缺口反馈台账

> 状态：Active
> 更新日期：2026-10-01

本台账记录 Aki 对 pinned `third_party/EUI-NEO` 的缺陷与能力缺口反馈（对齐
Executor 反馈台账的纪律：先核对 pinned 版本的公开头文件与实现，只写"不支持"
不构成有效记录）。每条记录包含：可复现证据、影响范围、期望语义、根因定位、
建议的最小修复、Aki 侧临时方案（如有，限定在单一 Adapter/boundary 内）与
上游 issue 链接。

## 台账

| 编号 | 日期 | 等级 | 状态 | 摘要 |
| --- | --- | --- | --- | --- |
| EUI-20260929-001 | 2026-09-29 | Medium | [Reported #76](https://github.com/sudoevolve/EUI-NEO/issues/76) | Linux 文件对话框把 stderr 合并进选取结果，调用方无法区分诊断行与所选路径 |
| EUI-20260929-002 | 2026-09-29 | Medium | [Reported #77](https://github.com/sudoevolve/EUI-NEO/issues/77) | 窗口创建缺 app_id/WM_CLASS 设置，Wayland 会话任务栏图标丢失 |

| EUI-20261001-001 | 2026-10-01 | Medium | Open | pinned 原生 Wayland 缺中文输入协议，临时使用 X11 |
| EUI-20261001-002 | 2026-10-01 | High | [Reported #78](https://github.com/sudoevolve/EUI-NEO/issues/78) | XIM 消费的退格仍被转发至正文 |

## EUI-20260929-001：文件对话框输出与 stderr 不可区分

- **pinned 版本**：v0.6.0（b9032a8）。
- **可复现证据**：Ubuntu 24.04（zenity 4.x）选择文件后，`core/platform/
  platform.cpp` 的 `openFileDialog`（zenity 分支 `:454-467`、kdialog 分支
  `:494`，命令以 `2>&1` 结尾）返回的 `selectedFiles` 首项为 GTK 诊断行
  （如 `Gtk-Message: Failed to load module ...`）而非所选路径；Aki 侧
  `std::filesystem::file_size` 报 ENOENT（"读取文件信息失败：no such file
  or directory"）。22.04（zenity/GTK3 静默）不复现。
- **根因**：`runCommand` 用 `popen` 捕获合并输出；`resultFromCommand`
  （`:428-442`）把每个非空行当路径，stderr 诊断行与结果不可区分。取消
  识别（`commandWasCancelled`）依赖 stderr 留在输出中，故不能简单删除
  `2>&1`。
- **期望语义**：成功路径只返回真实选取结果；诊断信息进 `error`/失败通道。
- **建议最小修复**：`resultFromCommand` 成功分支按 `std::filesystem::exists`
  过滤 `splitLines` 结果，全滤空则走 `failedFileDialog(output)` 透传诊断。
- **Aki 侧临时方案**：`ui/pages/conversations_page.cpp` 对选取结果按
  存在性过滤（单 boundary，绕不开上游对取消语义的破坏面）。
- **上游 issue**：[sudoevolve/EUI-NEO#76](https://github.com/sudoevolve/EUI-NEO/issues/76)。

## EUI-20260929-002：窗口 app_id/WM_CLASS 未设置，Wayland 图标丢失

- **pinned 版本**：v0.6.0（b9032a8）。
- **可复现证据**：Ubuntu 24.04（GNOME Wayland）安装 deb 后任务栏/窗口
  图标不显示；22.04（X11）正常。`DslAppConfig.iconPath` → `core::window::
  setWindowIcon` → `glfwSetWindowIcon` 链路完好（assets 256x256 PNG 验证
  有效），但 GLFW Wayland 后端 `glfwSetWindowIcon` 为空实现
  (`wl_window.c:2227-2231`，仅报 `GLFW_FEATURE_UNAVAILABLE`)；且全仓库
  无 `glfwWindowHintString(GLFW_WAYLAND_APP_ID/…)` 调用，窗口 app_id 为
  空，GNOME Shell 无法把窗口匹配到 `aki.desktop`（`StartupWMClass` 仅
  覆盖 X11 WM_CLASS）。
- **影响范围**：所有 Wayland 会话下的 Linux 桌面集成（任务栏图标、
  窗口分组）；X11 会话不受影响（`_NET_WM_ICON` 兜底）。
- **期望语义**：应用可经 `DslAppConfig` 声明 app_id/WM_CLASS（如
  `.appId("aki")`），窗口创建前设置三类 hint：`GLFW_WAYLAND_APP_ID`、
  `GLFW_X11_INSTANCE_NAME`、`GLFW_X11_CLASS_NAME`。
- **建议最小修复**：`DslAppConfig` 新增 `appIdValue` 字段 +
  `dsl_app_impl.h` 窗口创建前（`glfwCreateWindow` 附近）设置上述 hint；
  值与 desktop 文件名一致即可匹配。
- **Aki 侧临时方案**：`packaging/linux/aki.desktop` 增加
  `StartupWMClass=aki`（仅 X11 生效）；Wayland 治本依赖上游修复，
  Aki 不修改 pinned 依赖。
  2026-09-30 用户再次复现，按
  [DEC-024](../decisions/DEC-024-linux-desktop-window-identity.md)
  在 `ui/platform/application_identity.hpp` 使用公开 GLFW API 设置三个
  创建 hint；唯一调用点为 GLFW 初始化后、窗口创建前的配置首次构造。
  该适配不创建线程/资源、不改后端或依赖；依赖升级须复核启动次序，
  正式 app ID 配置接口上线并完成升级验收后移除。运行验证见 M5-24。
- **上游 issue**：[sudoevolve/EUI-NEO#77](https://github.com/sudoevolve/EUI-NEO/issues/77)。

## EUI-20261001-001：Linux 原生 Wayland 缺中文输入协议

- pinned EUI-NEO v0.6.0 b9032a8，GLFW 的 wl_window.c 不含 text-input /
  input-method；ime_bridge.c 的 Linux 分支所有方法为空，is_composing=0。
- 用户在 Ubuntu 24 安装版 9d0a7fe 的文本框无法切换中文。本机会话
  wayland，有 DISPLAY=:0、WAYLAND_DISPLAY=wayland-0、XMODIFIERS=@im=ibus。
  会话类型不单独作为窗口后端实测证据；适配后日志记录 glfwGetPlatform。
- 影响中文文本输入/组合串/候选定位。期望上游正式支持原生 Wayland 的
  输入法协议及公开窗口后端选择，不需要应用初始化第二套 GUI 生命周期。
- Aki 临时方案 DEC-025：GLFW 初始化前选择 X11（仅有 DISPLAY 时），
  LC_CTYPE 用户 locale；不修改上游、不增加线程。移除条件为正式能力
  上线并升级，真实中文与 Dock 回归通过。M5-36 尚待 GUI 复验。
- 上游 issue 尚未创建；本记录保留公开 API 与 pinned 证据，后续单独反馈。

## EUI-20261001-002：XIM 消费按键仍改动正文

- 状态 Reported #78，负责人 Linductor，2026-10-01。关联 M5-36。
  用户明确授权 Aki 临时补丁并发上游 issue。补丁应用于构建目录中的源码
  副本，原 pinned checkout 不变；原件/补丁/产物 SHA256 纳入 lock。
- 本机修复版实测：中文输入正常、Dock 正常、历史缩略图正常；组合串
  退格同时删除正文。pinned GLFW X11 在 XFilterEvent 返回 filtered 后
  仍触发 KeyPress 回调，只有字符路径检查 `!filtered`。Linux ime_bridge
  不报告 composing；公开 GLFW 回调不暴露过滤结果/XIC，不能正确加 guard。
- 提案：[issue 草稿](EUI-20261001-002-issue.md)和
  [最小补丁](patches/EUI-20261001-002-filtered-x11-keys.patch)。
  已生成并编译补丁副本，尚未完成 patched GUI 验收。需复验候选退格/Enter/Escape/
  方向键、确认后的正文退格、英文、重复/修饰键、焦点与关闭；原始按键
  消费者可能受影响，应由 EUI 上游确认接口语义。

- 上游 issue：[EUI-NEO#78](https://github.com/sudoevolve/EUI-NEO/issues/78)。
  临时方案移除条件：升级至正式修复版本、撤销构建覆盖并通过相同 GUI 验收。
