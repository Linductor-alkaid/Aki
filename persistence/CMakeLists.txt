# persistence 层（设计第 11/14 节）：M0 骨架 + M2-02 起 vendored SQLite 接入
# （DEC-004）；sqlite3* 封死在本层（RULE-10，M2-03 起以薄 RAII 封装消费）。
aki_add_skeleton_library(aki_persistence)
target_link_libraries(aki_persistence INTERFACE sqlite3)
