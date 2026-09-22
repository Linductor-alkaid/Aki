// v1 正式 schema SQL（M2-04）。单步原子迁移：四表 + CHECK + 外键。
#include "persistence/migration/schema_v1.hpp"

namespace aki::persistence {
namespace {

const char* kSchemaV1Sql = R"SQL(
CREATE TABLE device (
    device_id    TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    device_class INTEGER NOT NULL CHECK (device_class BETWEEN 0 AND 5),
    os_name      TEXT NOT NULL DEFAULT '',
    public_key   BLOB,
    capabilities INTEGER NOT NULL,
    trust_state  INTEGER NOT NULL CHECK (trust_state IN (0, 1, 2, 3, 4))
);

CREATE TABLE conversation (
    conversation_id TEXT PRIMARY KEY,
    local_device    TEXT NOT NULL REFERENCES device (device_id),
    remote_device   TEXT NOT NULL REFERENCES device (device_id),
    state           INTEGER NOT NULL CHECK (state IN (0, 1, 2))
);

CREATE TABLE message (
    message_id        TEXT PRIMARY KEY,
    conversation_id   TEXT NOT NULL REFERENCES conversation (conversation_id),
    sender            TEXT NOT NULL,
    receiver          TEXT NOT NULL,
    timestamp_ns      INTEGER NOT NULL,
    type              INTEGER NOT NULL CHECK (type IN (0, 1, 2, 3, 4)),
    body_text         TEXT,
    media_name        TEXT,
    media_size        INTEGER,
    media_mime        TEXT,
    media_transfer_id TEXT,
    delivery_state    INTEGER NOT NULL CHECK (delivery_state IN (0, 1, 2, 3, 4))
);

CREATE TABLE transfer (
    transfer_id          TEXT PRIMARY KEY,
    message_id           TEXT REFERENCES message (message_id),
    sender               TEXT NOT NULL,
    receiver             TEXT NOT NULL,
    file_name            TEXT NOT NULL,
    file_size            INTEGER NOT NULL,
    file_mime            TEXT NOT NULL DEFAULT '',
    transferred          INTEGER NOT NULL DEFAULT 0,
    total                INTEGER NOT NULL DEFAULT 0,
    state                INTEGER NOT NULL CHECK (state IN (0, 1, 2, 3, 4, 5, 6)),
    stored_relative_path TEXT,
    stored_sha256        TEXT,
    stored_size_bytes    INTEGER
);
)SQL";

}  // namespace

std::vector<MigrationStep> schema_v1_steps() {
    return {MigrationStep{1, "er-v1-core", kSchemaV1Sql}};
}

}  // namespace aki::persistence
