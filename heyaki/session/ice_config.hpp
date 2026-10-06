// ICE/TURN 服务器配置文件（M7，DEC-028 决策 7）：数据根下 `ice-servers.txt`
// 的解析与序列化。每行一台服务器：`kind host port [username credential]`，
// kind ∈ {stun, turn_udp}（vendored libjuice 后端仅 TURN/UDP——TURN/TCP 需
// libnice、TURN/TLS 无后端，均为上游 validate_peer_path_policy 拒绝项，不
// 在文件格式里放宽）；`#` 起注释，空行忽略；字段不含空白（写入侧校验）。
//
// 解析容错：单行非法跳过并计数（invalid_lines 可观测），不整体失败——
// TURN 配置错误不应阻断应用启动（无 ICE 服务器时 LAN/直连路径不受影响）；
// 解析结果最终经 NodeSession::create 的 validate_peer_path_policy 强校验
//（失败装配可见）。运行中的 NodeSession 不感知文件变化（重启生效，与
// relay enrollment 同纪律，HEY-20261006-001）。
//
// RULE-10：公开面仅 aki/std 类型（::heyaki::NodeIceServer 的转换入口
// `to_heyaki_ice_servers` 仅供 heyaki/session 内部与组合根使用）。
#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aki::heyaki {

// 单台 ICE 服务器（aki 公开面）。
struct IceServerConfig {
    std::string kind;  // "stun" | "turn_udp"
    std::string hostname;
    std::uint16_t port = 0;
    std::string username;    // TURN 必填（stun 留空）
    std::string credential;  // TURN 必填（stun 留空）
};

// 解析结果：servers + 被跳过的非法行计数（RULE-09 可观测）。
struct IceServersFile {
    std::vector<IceServerConfig> servers;
    std::uint64_t invalid_lines = 0;
};

// 字段静态校验（无 IO）：kind 合法、hostname 非空且无空白、port 1-65535、
// TURN 须带 username/credential、全字段无空白（行格式分隔符约束）。
[[nodiscard]] inline std::optional<std::string> validate_ice_server(
    const IceServerConfig& server) {
    const bool is_turn = server.kind == "turn_udp";
    if (server.kind != "stun" && !is_turn) {
        return std::string("kind must be stun or turn_udp");
    }
    auto has_whitespace = [](std::string_view text) {
        for (const char c : text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                return true;
            }
        }
        return false;
    };
    if (server.hostname.empty() || has_whitespace(server.hostname)) {
        return std::string("hostname must be non-empty without whitespace");
    }
    if (server.port == 0) {
        return std::string("port must be 1-65535");
    }
    if (is_turn
        && (server.username.empty() || server.credential.empty())) {
        return std::string("turn_udp requires username and credential");
    }
    if (has_whitespace(server.username) || has_whitespace(server.credential)) {
        return std::string("fields must not contain whitespace");
    }
    return std::nullopt;
}

// 解析文件内容（纯函数；文件读取由调用方完成）。
[[nodiscard]] inline IceServersFile parse_ice_servers_content(
    std::string_view content) {
    IceServersFile result;
    std::istringstream stream{std::string{content}};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        std::istringstream fields{line};
        IceServerConfig server;
        std::string port_text;
        if (!(fields >> server.kind >> server.hostname >> port_text)) {
            if (line.find_first_not_of(" \t") == std::string::npos) {
                continue;  // 空行/纯空白行
            }
            ++result.invalid_lines;
            continue;
        }
        fields >> server.username >> server.credential;  // TURN 凭据可选读
        // 端口 token 须为纯数字（"3478abc"/"+5"/"0x10" 都是整行非法，
        // 不取 stoul 前缀——RULE-09：非法行跳过计数可见，不静默矫形）。
        if (port_text.empty()
            || port_text.find_first_not_of("0123456789")
                != std::string::npos) {
            ++result.invalid_lines;
            continue;
        }
        try {
            const unsigned long port = std::stoul(port_text);
            if (port == 0 || port > 65535) {
                ++result.invalid_lines;
                continue;
            }
            server.port = static_cast<std::uint16_t>(port);
        } catch (const std::exception&) {
            ++result.invalid_lines;
            continue;
        }
        if (validate_ice_server(server).has_value()) {
            ++result.invalid_lines;
            continue;
        }
        result.servers.push_back(std::move(server));
    }
    return result;
}

// 单文件读取包装（文件缺失 = 空配置，非错误——默认无 TURN 部署合法）。
[[nodiscard]] inline IceServersFile load_ice_servers_file(
    const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse_ice_servers_content(buffer.str());
}

// 序列化（整文件覆写形态：设置页保存单 TURN 服务器时由 HostRuntime 生成
// 全量行；字段已经 validate_ice_server 校验无空白）。
[[nodiscard]] inline std::string serialize_ice_servers(
    const std::vector<IceServerConfig>& servers) {
    std::ostringstream stream;
    for (const auto& server : servers) {
        stream << server.kind << ' ' << server.hostname << ' '
               << static_cast<unsigned>(server.port);
        if (!server.username.empty()) {
            stream << ' ' << server.username << ' ' << server.credential;
        }
        stream << '\n';
    }
    return stream.str();
}

}  // namespace aki::heyaki
