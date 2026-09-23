// 数据根目录解析实现（M2-06）。平台分支以条件编译隔离（设计第 11.1 节 ④）。
#include "persistence/storage/data_root.hpp"

#include <cstdlib>
#include <string>

namespace aki::persistence {
namespace {

// getenv 的 MSVC C4996 为部署建议而非缺陷（只读标准环境变量，无 CRT 写入
// 风险面）；作用域内豁免，不放松全局（DEC-004 影响节同策略）。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string join_path(const std::string& base, const std::string& leaf) {
    if (base.empty()) {
        return leaf;
    }
    if (base.back() == '/' || base.back() == '\\') {
        return base + leaf;
    }
    return base + "/" + leaf;
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace

std::string resolve_data_root() {
#if defined(_WIN32)
    // Windows：%APPDATA%\aki（DEC-004）。
    const std::string appdata = env_or_empty("APPDATA");
    if (!appdata.empty()) {
        return join_path(appdata, "aki");
    }
    return "aki-data";  // 显式回退：相对当前目录，不静默假装系统目录
#else
    // POSIX：$XDG_DATA_HOME/aki，缺失时 $HOME/.local/share/aki（XDG 规范）。
    const std::string xdg = env_or_empty("XDG_DATA_HOME");
    if (!xdg.empty()) {
        return join_path(xdg, "aki");
    }
    const std::string home = env_or_empty("HOME");
    if (!home.empty()) {
        return join_path(join_path(home, ".local/share"), "aki");
    }
    return "aki-data";
#endif
}

}  // namespace aki::persistence
