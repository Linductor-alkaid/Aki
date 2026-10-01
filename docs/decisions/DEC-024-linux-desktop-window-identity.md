# DEC-024：Linux 桌面入口与窗口身份一致

> 状态：Accepted
> 日期：2026-09-30
> 负责人：Linductor
> 冻结里程碑：M5-24
> 替代/被替代：补充 DEC-021 的 Linux 图标遗留项

## 背景与问题

用户在 Ubuntu 24.04 安装新版 deb 后再次发现运行中的 Dock 图标丢失。
安装包已有 `aki.desktop` 和 hicolor `aki.png`；仅设置 `iconPath` 和
`StartupWMClass` 无法给原生 Wayland 窗口提供 `app_id`。上游缺口继续由
[EUI-20260929-002 / EUI-NEO #77](../eui_neo_feedback/ledger.md) 跟踪。

## 决策

- Aki Linux 平台适配层使用 pinned GLFW 3.4 的公开
  `glfwWindowHintString` 设置 Wayland app ID、X11 class 和 instance，
  三者均为 `aki`，与桌面文件名 `aki.desktop` 一致。
- 唯一调用点是 `app::dslAppConfig()` 的首次配置构造。核对 pinned
  `glfw_app_main.cpp`：`glfwInit()` 成功后才查询 `initialWindowWidth()`，
  继而首次构造配置；`core::window::createWindow()` 不重置字符串 hint。
  此处是一项主线程、有界、仅设置创建参数的兼容适配例外，不装配 Host，
  不创建窗口、线程、任务或其他资源；之后的配置查询仍无副作用。
- 非 Linux 平台不执行此适配。不改 `third_party`，不强制 X11，继续使用
  框架负责的 GLFW 初始化、窗口创建与销毁生命周期。
- 上游提供正式 `DslAppConfig.appId` 接口并经独立依赖升级验收后移除此
  适配；每次升级 EUI-NEO 时须复核上述启动次序及 hint 保留契约。

## 备选方案

- 修改 pinned EUI-NEO：当前未授权依赖修复，继续报上游。
- 修改 desktop 的 Icon 或清图标缓存：包内图标完好，无法补齐窗口身份。
- 启动器强制 XWayland：改变后端、输入法和缩放行为，范围过大。

## 验证方式与影响

Debug 构建确认公开 GLFW API 链接；真实 Wayland 协议确认窗口发送
`set_app_id("aki")`，X11 查询 `WM_CLASS` 两项均为 `aki`。
deb 校验桌面入口与图标布局，Ubuntu 24.04 安装版由用户观察 Dock 图标
及重开后的分组。未执行的真机验收保持 M5-24 未完成。
无 Core、Heyaki 协议、数据、权限、Executor 或视觉令牌变更。
