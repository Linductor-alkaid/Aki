// 传输归档 IO 承载面（设计 §7.1③/DEC-011；M4-04）。
//
// 发送侧 Aki 侧文件 IO（发送前流式 SHA-256 + 归档拷贝 source →
// files/tmp/<transfer_id>.part）的抽象承载接口：由组合根以专用 blocking
// worker（aki.transfer-io，DatabaseWorker 同款形态）实现——实现落
// persistence/storage/transfer_io_worker.hpp（FileStore + 流式 Sha256），
// 消费方为 app TransferManager（经泵上下文提交/续接，事件驱动会话状态机）。
//
// 本头文件属 Domain 层 transfer/storage（RULE-01/10）：仅 aki/std 类型，
// 不依赖 executor/persistence/heyaki——worker 细节全部封在实现侧。
//
// 使用契约（DEC-011 ①③）：
//   - start() 开会话并执行首个 hash 分块；此后每会话单飞（single-flight）：
//     advance() 续接下一分块（hash 相位完 → copy 相位），仅在收到上一分块
//     的完成事件后由泵提交；cancel() 清理 worker 侧会话状态并幂等删除
//     .part。作业为 offset 基础的无状态分块（对齐断点续传）。
//   - 事件经 set_event_sink 注入的投递面回到调用方（worker 线程上下文
//     调用——实现保证有界、不抛出；调用方在此上下文只做有界投递）。
//   - idle() == true 表示无在飞作业且无待发事件的回调——调用方析构前必须
//     等待（其事件回调不得晚于消费方终结，EXEC-07 形态，DEC-011 ③）。
//   - request_stop() 协作停止（worker 侧存量作业清理；join 归 owner
//     EXEC-01 步骤 2/3）。
#pragma once

#include "transfer/transfer/transfer_types.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>

namespace aki::transfer {

// IO 完成事件（worker 线程 → 调用方投递面；aki/std 类型）。
struct TransferIoEvent {
    enum class Phase {
        hash_progress,  // hash 相位分块完成（未终）
        hash_done,      // hash 相位完成（hash_hex 携带 64 字符小写 hex）
        copy_progress,  // 归档拷贝分块完成（未终）
        copy_done,      // 归档拷贝完成（.part 完整）
        failed,         // 会话 IO 失败（error 携带原因；会话状态已清理）
        cancelled,      // 会话已清理（cancel 作业完成；.part 已幂等删除）
        released,       // 会话状态已清理且 .part 保留（终态 Completed 路径：
                        // M2-06 作业组随消费 .part，不得提前删除）
    };
    TransferId transfer;
    Phase phase = Phase::hash_progress;
    std::uint64_t bytes_done = 0;  // 当前相位累计
    std::uint64_t bytes_total = 0;
    std::string hash_hex;
    std::string error;
};

class TransferIo {
public:
    virtual ~TransferIo() = default;

    TransferIo(const TransferIo&) = delete;
    TransferIo& operator=(const TransferIo&) = delete;

    // 开会话 + 首个 hash 分块。false = admission 拒绝（通道满/已停止/
    // transfer_id 非法——可见，RULE-09）。display_name 仅用于终态净化名
    // 来源记录（.part 路径只依赖 transfer_id）。
    [[nodiscard]] virtual bool start(const TransferId& transfer,
        std::filesystem::path source, std::string display_name,
        std::uint64_t total_bytes)
        = 0;

    // 续接下一分块（每会话单飞：仅在上一分块事件到达后提交）。
    [[nodiscard]] virtual bool advance(const TransferId& transfer) = 0;

    // 清理会话（worker 侧状态 + .part 幂等删除）；completed 事件以
    // TransferIoEvent::cancelled 回报。
    [[nodiscard]] virtual bool cancel(const TransferId& transfer) = 0;

    // 清理 worker 侧会话状态但**保留 .part**（终态 Completed 路径：M2-06
    // 终态作业组消费 .part 后由其负责改名/删除语义）；事件以
    // TransferIoEvent::released 回报。
    [[nodiscard]] virtual bool release(const TransferId& transfer) = 0;

    // 事件投递面（worker 线程上下文；实现保证不抛出）。必须在 worker 启动
    // 前设置（组合根装配序）。
    virtual void set_event_sink(
        std::function<void(const TransferIoEvent&)> sink)
        = 0;

    // 协作停止（非阻塞；存量作业清理，回调结算后 worker 退出）。
    virtual void request_stop() noexcept = 0;

    // 无在飞作业且无待回调（调用方析构前的等待条件）。
    [[nodiscard]] virtual bool idle() const noexcept = 0;

    // admission 拒绝计数（EXEC-06/RULE-09 可观测）。
    [[nodiscard]] virtual std::uint64_t rejected_submissions() const noexcept = 0;

protected:
    TransferIo() = default;
};

}  // namespace aki::transfer
