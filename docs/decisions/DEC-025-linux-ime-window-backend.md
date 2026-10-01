# DEC-025：Linux 中文输入的窗口后端兼容

> 状态：Accepted；日期：2026-10-01；负责人：Linductor；工作项 M5-36。

当前 EUI-NEO v0.6.0 的 Linux IME bridge 是空实现，pinned GLFW Wayland
未实现 text-input/input-method 协议；X11 后端提供 XIM 提交字符。本机
GNOME Wayland 会话同时有 DISPLAY 与 XMODIFIERS=@im=ibus。

Aki 在唯一 Platform Adapter 中、框架 glfwInit 前，通过公开 glfwInitHint
选择可用的 X11 后端，并在单线程启动期设置 LC_CTYPE 为用户 locale。
窗口生命周期仍归 EUI-NEO；不增加线程、不改变渲染后端，保留 aki 的
X11 class/instance 与图标身份。无 DISPLAY 时不强制无法连接的后端。
这是有 XWayland 的兼容策略，不宣称原生 Wayland 输入法已实现。

上游提供原生 Wayland 输入法并完成 pinned 升级与双端验收后移除此兼容。
反馈 EUI-20261001-001；需要实测中文候选、退格、发送和 Dock 分组。
