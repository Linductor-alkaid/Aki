// 文件本体存储布局与生命周期（DEC-004；设计第 11.1 节 ④/②；M2-06）。
//
// 布局（数据根下并置，构造时创建）：
//   <root>/files/tmp/<transfer_id>.part   写入期（RULE-05：文件数据与消息分离）
//   <root>/files/<transfer_id>/<净化名>    Completed 原子改名后
//   DB 仅存 POSIX 相对路径（files/<id>/<净化名>）、hash、size；
//   远端原始文件名只存 DB 供展示，禁止拼入磁盘路径。
//
// TransferId 字符集约束（DEC-004）：[A-Za-z0-9_-]{1,64}，入口
// `valid_transfer_id` 固化并拒绝（路径穿越载荷在入口被拒，RULE-09）。
// 磁盘名经 `sanitize_disk_name` 净化：取末段路径分量（/ 与 \ 之后）、
// 非 [A-Za-z0-9._-] 字符替换为 '_'、空/全点回退 "file"、截断 128 字符。
//
// 线程契约：FileStore 方法为常量态目录操作（无内部可变状态），同
// DatabaseWorker 串行上下文或启动恢复主线程调用（与 M2-03/05 一致）。
#pragma once

#include "persistence/repository/repositories.hpp"
#include "persistence/storage/sha256.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace aki::persistence {

class FileStore {
public:
    // 创建 <root>/files 与 <root>/files/tmp（create_directories；
    // 失败抛 std::runtime_error，不静默）。
    explicit FileStore(std::string data_root);

    [[nodiscard]] const std::string& data_root() const noexcept {
        return root_;
    }

    // TransferId 字符集约束（DEC-004）：[A-Za-z0-9_-]{1,64}。
    [[nodiscard]] static bool valid_transfer_id(
        const std::string& transfer_id) noexcept;

    // 远端原始文件名 → 磁盘净化名（策略见头注）。
    [[nodiscard]] static std::string sanitize_disk_name(
        const std::string& remote_name);

    // files/tmp/<transfer_id>.part；非法 transfer_id 抛 std::invalid_argument。
    [[nodiscard]] std::string part_path(
        const std::string& transfer_id) const;
    // files/<transfer_id>/<disk_name>。
    [[nodiscard]] std::string final_path(const std::string& transfer_id,
        const std::string& disk_name) const;

    // .part 写入：append=false 覆盖、true 追加（调用方分块驱动，有界缓冲——
    // 每次仅写入给定块，无内部累积）。非法 id 或 I/O 失败抛出（RULE-09）。
    void write_part(const std::string& transfer_id,
        std::span<const std::byte> data, bool append = false) const;

    [[nodiscard]] bool part_exists(const std::string& transfer_id) const;

    // Completed 终态作业组（设计第 11.1 节 ④；经 DatabaseWorker 串行执行）：
    //   - 行缺失 → runtime_error；
    //   - 行已 Completed 且目标文件存在 → 幂等跳过（重复终态宣告计成功）；
    //   - .part 缺失（且无法从既有最终文件恢复）→ 明确失败（RULE-09）；
    //   - 流式 SHA-256 + 复制 + 原子改名到 files/<id>/<净化名> →
    //     TRANSFER 终态更新 + 回写位（relative_path/sha256/size）。
    // 崩溃恢复：改名已发生但回写未完成时，从最终文件补算哈希回写收敛。
    void complete_transfer(TransferRepository& transfers,
        const std::string& transfer_id) const;

    // Failed / Cancelled：.part 幂等删除（缺失即 no-op）。
    void discard_part(const std::string& transfer_id) const noexcept;

    // 启动清扫（设计第 11.1 节 ②）：files/tmp 下无活动 Transfer 行对应的
    // .part 残留删除（非法文件名/终态行/无行均视为无活动），返回删除数。
    // active_transfers 为恢复段加载的全部 Transfer 行。
    [[nodiscard]] std::size_t sweep_tmp_orphans(
        const std::vector<aki::transfer::Transfer>& active_transfers) const;

private:
    std::string root_;
    std::string files_dir_;
    std::string tmp_dir_;
};

}  // namespace aki::persistence
