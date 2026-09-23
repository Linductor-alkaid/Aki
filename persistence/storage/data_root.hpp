// 数据根目录解析（设计第 11.1 节 ④ 最小落点；DEC-004；M2-06）。
//
// 平台条件编译单元提供解析（Windows %APPDATA% / Linux XDG），公开面仅
// std::string 路径（RULE-10，平台类型不出现在头文件——实现见
// data_root.cpp 的 #ifdef 分支）。组合根解析一次后经构造参数注入
// （FileStore/Database 路径），Core/persistence 其余部分不见平台环境。
//
// 解析规则：
//   Windows：%APPDATA%\aki（环境变量缺失时回退当前目录，失败可见由调用方
//   处理——解析不做 I/O，目录创建由 FileStore/Database 承担）。
//   POSIX：$XDG_DATA_HOME\aki，缺失时 $HOME/.local/share\aki（XDG 规范）。
//   两平台环境变量均缺失的最终回退为 "./aki-data"（显式可见，不静默假装
//   系统目录存在）。
//
// 跨平台注记：本机（Windows）验证 Windows 分支；Linux 分支随 CI Linux
// 编译执行（M2-06 验证记录），本机不宣称已验证 POSIX 分支行为。
#pragma once

#include <string>

namespace aki::persistence {

// 返回数据根目录（不含尾部分隔符）。仅字符串拼接与 getenv，不做 I/O。
[[nodiscard]] std::string resolve_data_root();

}  // namespace aki::persistence
