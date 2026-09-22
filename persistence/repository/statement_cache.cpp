// prepared-statement LRU 缓存实现（M2-04）。
#include "persistence/repository/statement_cache.hpp"

#include <stdexcept>

namespace aki::persistence {

StatementCache::StatementCache(Database& database, std::size_t capacity)
    : database_(&database), capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument(
            "StatementCache capacity must be greater than zero");
    }
}

Statement& StatementCache::get(const std::string& sql) {
    const auto found = index_.find(sql);
    if (found != index_.end()) {
        entries_.splice(entries_.begin(), entries_, found->second);  // 提到最近使用
        Statement& statement = found->second->second;
        statement.reset();  // reset + clear bindings，复用前清理
        return statement;
    }

    if (entries_.size() >= capacity_) {
        evict_lru();  // 明确语义：LRU 逐出（RULE-09：有界、可测试）
    }

    entries_.emplace_front(sql, database_->prepare(sql));
    index_.emplace(entries_.front().first, entries_.begin());
    return entries_.front().second;
}

void StatementCache::invalidate(const std::string& sql) noexcept {
    const auto found = index_.find(sql);
    if (found == index_.end()) {
        return;
    }
    entries_.erase(found->second);  // Statement 析构 → finalize
    index_.erase(found);
}

void StatementCache::evict_lru() {
    const auto& victim = entries_.back();
    index_.erase(victim.first);
    entries_.pop_back();  // Statement 析构 → 确定性 finalize
}

}  // namespace aki::persistence
