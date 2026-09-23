// prepared-statement LRU 缓存（RULE-09：容量上限，行为明确并测试；M2-04）。
//
// 满时行为：LRU 逐出最久未使用条目（逐出即析构 Statement → 确定性 finalize），
// 不静默吞掉也不无限增长。get() 命中时对语句 reset + clear bindings 后复用。
//
// 线程契约：非线程安全——仅限 Database 的当前使用上下文（启动恢复主线程 /
// M2-05 DatabaseWorker 串行上下文）调用，与 M2-03 封装一致（RULE-07）。
#pragma once

#include "persistence/database/database.hpp"

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace aki::persistence {

class StatementCache {
public:
    explicit StatementCache(Database& database, std::size_t capacity = 16);

    StatementCache(const StatementCache&) = delete;
    StatementCache& operator=(const StatementCache&) = delete;
    // 可移动（持有 Database* 与可移动容器；移动后源为空壳，仅可析构）。
    StatementCache(StatementCache&&) noexcept = default;
    StatementCache& operator=(StatementCache&&) noexcept = default;

    // 命中：reset + clear bindings 后返回复用；未命中：prepare 并入缓存
    // （满时先 LRU 逐出）。返回的引用在下一次 get() 前有效。
    [[nodiscard]] Statement& get(const std::string& sql);

    // 逐出指定语句（确定性 finalize）。错误缓解：SQLite 3.53.4 在 reset 一个
    // 曾因约束失败（FK/CHECK）的语句时会异常终止（官方 DLL/MSVC/GCC 三路
    // 复现，M2-04 验证记录）——因此语句失败后必须逐出，下次调用方重新
    // prepare（fresh 语句行为正确）。
    void invalidate(const std::string& sql) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    using Entry = std::pair<std::string, Statement>;

    void evict_lru();

    Database* database_;
    std::size_t capacity_;
    std::list<Entry> entries_;  // 前端 = 最近使用
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
};

}  // namespace aki::persistence
