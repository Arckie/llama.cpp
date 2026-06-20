#include "local-ai-memory.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(LOCAL_AI_STUDIO_SQLITE)
#if defined(LOCAL_AI_STUDIO_WINSQLITE)
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif
#endif

namespace local_ai_memory {
namespace {

int64_t now_unix() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::string join_path(const std::string & root, const std::string & filename) {
    if (root.empty()) {
        return filename;
    }
    const char tail = root[root.size() - 1];
    if (tail == '/' || tail == '\\') {
        return root + filename;
    }
    return root + "/" + filename;
}

#if defined(LOCAL_AI_STUDIO_SQLITE)

struct sqlite_db {
    sqlite3 * handle = nullptr;

    explicit sqlite_db(const std::string & path) {
        if (sqlite3_open(path.c_str(), &handle) != SQLITE_OK) {
            std::string error = handle ? sqlite3_errmsg(handle) : "unknown sqlite open error";
            if (handle) {
                sqlite3_close(handle);
                handle = nullptr;
            }
            throw std::runtime_error(error);
        }
    }

    ~sqlite_db() {
        if (handle) {
            sqlite3_close(handle);
        }
    }

    sqlite_db(const sqlite_db &) = delete;
    sqlite_db & operator=(const sqlite_db &) = delete;

    sqlite_db(sqlite_db && other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }

    sqlite_db & operator=(sqlite_db && other) noexcept {
        if (this != &other) {
            if (handle) {
                sqlite3_close(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
};

void exec(sqlite3 * db, const char * sql) {
    char * error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "sqlite exec failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

sqlite_db open_db(const std::string & slot_save_path) {
    if (slot_save_path.empty()) {
        throw std::runtime_error("slot_save_path is empty");
    }
    std::filesystem::create_directories(slot_save_path);
    sqlite_db db(db_path(slot_save_path));
    exec(db.handle, "PRAGMA journal_mode=WAL;");
    exec(db.handle, "PRAGMA busy_timeout=5000;");
    exec(db.handle, R"sql(
CREATE TABLE IF NOT EXISTS kv_cache_snapshots (
  filename TEXT PRIMARY KEY,
  id_slot INTEGER NOT NULL,
  last_event TEXT NOT NULL,
  size_bytes INTEGER NOT NULL DEFAULT 0,
  stored_path TEXT NOT NULL,
  updated_at_unix INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS kv_cache_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  event TEXT NOT NULL,
  id_slot INTEGER NOT NULL,
  filename TEXT NOT NULL DEFAULT '',
  size_bytes INTEGER NOT NULL DEFAULT 0,
  updated_at_unix INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS medium_memories (
  id TEXT PRIMARY KEY,
  owner_id TEXT NOT NULL DEFAULT '',
  conversation_id TEXT NOT NULL DEFAULT '',
  scope TEXT NOT NULL DEFAULT 'conversation',
  text TEXT NOT NULL DEFAULT '',
  index_text TEXT NOT NULL DEFAULT '',
  embedding_json TEXT NOT NULL DEFAULT '[]',
  metadata_json TEXT NOT NULL DEFAULT '{}',
  state TEXT NOT NULL DEFAULT 'active',
  importance REAL NOT NULL DEFAULT 0,
  last_accessed_at_unix INTEGER,
  created_at_unix INTEGER NOT NULL,
  updated_at_unix INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_kv_cache_events_slot_updated ON kv_cache_events(id_slot, updated_at_unix);
CREATE INDEX IF NOT EXISTS idx_medium_memories_owner_state ON medium_memories(owner_id, state, updated_at_unix);
CREATE INDEX IF NOT EXISTS idx_medium_memories_owner_conversation_scope_state ON medium_memories(owner_id, conversation_id, scope, state);
)sql");
    return db;
}

void bind_text(sqlite3_stmt * stmt, int index, const std::string & value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt * stmt, int index) {
    const unsigned char * value = sqlite3_column_text(stmt, index);
    return value ? reinterpret_cast<const char *>(value) : "";
}

std::vector<double> parse_vector(const json & value) {
    std::vector<double> vector;
    if (!value.is_array()) {
        return vector;
    }
    vector.reserve(value.size());
    for (const auto & item : value) {
        if (item.is_number()) {
            vector.push_back(item.get<double>());
        }
    }
    return vector;
}

double cosine_similarity(const std::vector<double> & a, const std::vector<double> & b) {
    if (a.empty() || a.size() != b.size()) {
        return 0.0;
    }
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a <= 0.0 || norm_b <= 0.0) {
        return 0.0;
    }
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

json row_to_medium_memory(sqlite3_stmt * stmt, bool include_embedding) {
    json item = {
        {"id", column_text(stmt, 0)},
        {"owner_id", column_text(stmt, 1)},
        {"conversation_id", column_text(stmt, 2)},
        {"scope", column_text(stmt, 3)},
        {"text", column_text(stmt, 4)},
        {"index_text", column_text(stmt, 5)},
        {"metadata", json::parse(column_text(stmt, 7), nullptr, false)},
        {"state", column_text(stmt, 8)},
        {"importance", sqlite3_column_double(stmt, 9)},
        {"last_accessed_at_unix", sqlite3_column_type(stmt, 10) == SQLITE_NULL ? json(nullptr) : json(sqlite3_column_int64(stmt, 10))},
        {"created_at_unix", sqlite3_column_int64(stmt, 11)},
        {"updated_at_unix", sqlite3_column_int64(stmt, 12)},
    };
    if (item["metadata"].is_discarded()) {
        item["metadata"] = json::object();
    }
    if (include_embedding) {
        json embedding = json::parse(column_text(stmt, 6), nullptr, false);
        item["embedding"] = embedding.is_discarded() ? json::array() : embedding;
    }
    return item;
}

json sqlite_error(const std::exception & exc) {
    return {
        {"ok", false},
        {"error", "LOCAL_AI_SQLITE_ERROR"},
        {"detail", exc.what()},
    };
}

#endif

json sqlite_disabled_error() {
    return {
        {"ok", false},
        {"error", "LOCAL_AI_SQLITE_DISABLED"},
        {"detail", "Build llama.cpp with -DLOCAL_AI_STUDIO_SQLITE=ON and SQLite3 development files."},
    };
}

}

std::string db_path(const std::string & slot_save_path) {
    return join_path(slot_save_path, "local-ai-memory.db");
}

json kv_cache_metadata(const std::string & slot_save_path, int id_slot, const std::string & filename) {
    std::string filepath = join_path(slot_save_path, filename);
    bool exists = std::filesystem::exists(filepath);
    uintmax_t size_bytes = exists ? std::filesystem::file_size(filepath) : 0;
    return {
        {"ok", true},
        {"id_slot", id_slot},
        {"filename", filename},
        {"exists", exists},
        {"size_bytes", size_bytes},
        {"stored_path", filepath},
        {"db_path", db_path(slot_save_path)},
        {"updated_at_unix", now_unix()},
    };
}

json record_kv_event(const std::string & slot_save_path, const std::string & event, int id_slot, const std::string & filename, size_t size_bytes) {
#if defined(LOCAL_AI_STUDIO_SQLITE)
    try {
        auto db = open_db(slot_save_path);
        sqlite3_stmt * stmt = nullptr;
        const char * event_sql = "INSERT INTO kv_cache_events(event, id_slot, filename, size_bytes, updated_at_unix) VALUES(?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db.handle, event_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db.handle));
        }
        bind_text(stmt, 1, event);
        sqlite3_bind_int(stmt, 2, id_slot);
        bind_text(stmt, 3, filename);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(size_bytes));
        sqlite3_bind_int64(stmt, 5, now_unix());
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string error = sqlite3_errmsg(db.handle);
            sqlite3_finalize(stmt);
            throw std::runtime_error(error);
        }
        sqlite3_finalize(stmt);

        if (!filename.empty()) {
            const char * snapshot_sql = R"sql(
INSERT INTO kv_cache_snapshots(filename, id_slot, last_event, size_bytes, stored_path, updated_at_unix)
VALUES(?, ?, ?, ?, ?, ?)
ON CONFLICT(filename) DO UPDATE SET
  id_slot = excluded.id_slot,
  last_event = excluded.last_event,
  size_bytes = excluded.size_bytes,
  stored_path = excluded.stored_path,
  updated_at_unix = excluded.updated_at_unix
)sql";
            if (sqlite3_prepare_v2(db.handle, snapshot_sql, -1, &stmt, nullptr) != SQLITE_OK) {
                throw std::runtime_error(sqlite3_errmsg(db.handle));
            }
            bind_text(stmt, 1, filename);
            sqlite3_bind_int(stmt, 2, id_slot);
            bind_text(stmt, 3, event);
            sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(size_bytes));
            bind_text(stmt, 5, join_path(slot_save_path, filename));
            sqlite3_bind_int64(stmt, 6, now_unix());
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                std::string error = sqlite3_errmsg(db.handle);
                sqlite3_finalize(stmt);
                throw std::runtime_error(error);
            }
            sqlite3_finalize(stmt);
        }

        json result = kv_cache_metadata(slot_save_path, id_slot, filename);
        result["event"] = event;
        return result;
    } catch (const std::exception & exc) {
        return sqlite_error(exc);
    }
#else
    GGML_UNUSED(slot_save_path);
    GGML_UNUSED(event);
    GGML_UNUSED(id_slot);
    GGML_UNUSED(filename);
    GGML_UNUSED(size_bytes);
    return sqlite_disabled_error();
#endif
}

json medium_memory_upsert(const std::string & slot_save_path, const json & body) {
#if defined(LOCAL_AI_STUDIO_SQLITE)
    try {
        auto db = open_db(slot_save_path);
        const std::string id = json_value(body, "id", std::string());
        if (id.empty()) {
            return {{"ok", false}, {"error", "MISSING_ID"}};
        }
        const std::string owner_id = json_value(body, "owner_id", std::string());
        const std::string conversation_id = json_value(body, "conversation_id", std::string());
        const std::string scope = json_value(body, "scope", std::string("conversation"));
        const std::string text = json_value(body, "text", std::string());
        const std::string index_text = json_value(body, "index_text", text);
        const std::string state = json_value(body, "state", std::string("active"));
        const double importance = json_value(body, "importance", 0.0);
        const json embedding = body.contains("embedding") && body["embedding"].is_array() ? body["embedding"] : json::array();
        const json metadata = body.contains("metadata") && body["metadata"].is_object() ? body["metadata"] : json::object();

        sqlite3_stmt * stmt = nullptr;
        const char * sql = R"sql(
INSERT INTO medium_memories(id, owner_id, conversation_id, scope, text, index_text, embedding_json, metadata_json, state, importance, last_accessed_at_unix, created_at_unix, updated_at_unix)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, ?, ?)
ON CONFLICT(id) DO UPDATE SET
  owner_id = excluded.owner_id,
  conversation_id = excluded.conversation_id,
  scope = excluded.scope,
  text = excluded.text,
  index_text = excluded.index_text,
  embedding_json = excluded.embedding_json,
  metadata_json = excluded.metadata_json,
  state = excluded.state,
  importance = excluded.importance,
  updated_at_unix = excluded.updated_at_unix
)sql";
        if (sqlite3_prepare_v2(db.handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db.handle));
        }
        const int64_t ts = now_unix();
        bind_text(stmt, 1, id);
        bind_text(stmt, 2, owner_id);
        bind_text(stmt, 3, conversation_id);
        bind_text(stmt, 4, scope);
        bind_text(stmt, 5, text);
        bind_text(stmt, 6, index_text);
        bind_text(stmt, 7, embedding.dump());
        bind_text(stmt, 8, metadata.dump());
        bind_text(stmt, 9, state);
        sqlite3_bind_double(stmt, 10, importance);
        sqlite3_bind_int64(stmt, 11, ts);
        sqlite3_bind_int64(stmt, 12, ts);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string error = sqlite3_errmsg(db.handle);
            sqlite3_finalize(stmt);
            throw std::runtime_error(error);
        }
        sqlite3_finalize(stmt);
        return {{"ok", true}, {"id", id}, {"db_path", db_path(slot_save_path)}, {"updated_at_unix", ts}};
    } catch (const std::exception & exc) {
        return sqlite_error(exc);
    }
#else
    GGML_UNUSED(slot_save_path);
    GGML_UNUSED(body);
    return sqlite_disabled_error();
#endif
}

json medium_memory_list(const std::string & slot_save_path, const std::string & owner_id, int limit, bool include_embedding) {
#if defined(LOCAL_AI_STUDIO_SQLITE)
    try {
        auto db = open_db(slot_save_path);
        sqlite3_stmt * stmt = nullptr;
        limit = std::max(1, std::min(limit, 100));
        std::string sql = "SELECT id, owner_id, conversation_id, scope, text, index_text, embedding_json, metadata_json, state, importance, last_accessed_at_unix, created_at_unix, updated_at_unix FROM medium_memories WHERE state = 'active'";
        if (!owner_id.empty()) {
            sql += " AND owner_id = ?";
        }
        sql += " ORDER BY updated_at_unix DESC LIMIT ?";
        if (sqlite3_prepare_v2(db.handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db.handle));
        }
        int bind_index = 1;
        if (!owner_id.empty()) {
            bind_text(stmt, bind_index++, owner_id);
        }
        sqlite3_bind_int(stmt, bind_index, limit);
        json items = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            items.push_back(row_to_medium_memory(stmt, include_embedding));
        }
        sqlite3_finalize(stmt);
        return {{"ok", true}, {"db_path", db_path(slot_save_path)}, {"items", items}};
    } catch (const std::exception & exc) {
        return sqlite_error(exc);
    }
#else
    GGML_UNUSED(slot_save_path);
    GGML_UNUSED(owner_id);
    GGML_UNUSED(limit);
    GGML_UNUSED(include_embedding);
    return sqlite_disabled_error();
#endif
}

json medium_memory_search(const std::string & slot_save_path, const json & body) {
#if defined(LOCAL_AI_STUDIO_SQLITE)
    try {
        const std::vector<double> query = parse_vector(body.value("embedding", json::array()));
        if (query.empty()) {
            return {{"ok", false}, {"error", "MISSING_EMBEDDING"}};
        }
        const std::string owner_id = json_value(body, "owner_id", std::string());
        const int limit = std::max(1, std::min(json_value(body, "limit", 8), 50));
        auto db = open_db(slot_save_path);
        sqlite3_stmt * stmt = nullptr;
        std::string sql = "SELECT id, owner_id, conversation_id, scope, text, index_text, embedding_json, metadata_json, state, importance, last_accessed_at_unix, created_at_unix, updated_at_unix FROM medium_memories WHERE state = 'active'";
        if (!owner_id.empty()) {
            sql += " AND owner_id = ?";
        }
        if (sqlite3_prepare_v2(db.handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db.handle));
        }
        if (!owner_id.empty()) {
            bind_text(stmt, 1, owner_id);
        }
        json scored = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json item = row_to_medium_memory(stmt, false);
            json embedding_json = json::parse(column_text(stmt, 6), nullptr, false);
            const double score = cosine_similarity(query, parse_vector(embedding_json));
            item["score"] = score;
            scored.push_back(item);
        }
        sqlite3_finalize(stmt);
        std::sort(scored.begin(), scored.end(), [](const json & a, const json & b) {
            return a.value("score", 0.0) > b.value("score", 0.0);
        });
        if (static_cast<int>(scored.size()) > limit) {
            scored.erase(scored.begin() + limit, scored.end());
        }
        const int64_t accessed_at = now_unix();
        for (const auto & item : scored) {
            sqlite3_stmt * update = nullptr;
            if (sqlite3_prepare_v2(db.handle, "UPDATE medium_memories SET last_accessed_at_unix = ? WHERE id = ?", -1, &update, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(update, 1, accessed_at);
                bind_text(update, 2, item.value("id", std::string()));
                sqlite3_step(update);
            }
            sqlite3_finalize(update);
        }
        return {{"ok", true}, {"db_path", db_path(slot_save_path)}, {"items", scored}};
    } catch (const std::exception & exc) {
        return sqlite_error(exc);
    }
#else
    GGML_UNUSED(slot_save_path);
    GGML_UNUSED(body);
    return sqlite_disabled_error();
#endif
}

json medium_memory_delete(const std::string & slot_save_path, const std::string & id) {
#if defined(LOCAL_AI_STUDIO_SQLITE)
    try {
        auto db = open_db(slot_save_path);
        sqlite3_stmt * stmt = nullptr;
        if (sqlite3_prepare_v2(db.handle, "UPDATE medium_memories SET state = 'archived', updated_at_unix = ? WHERE id = ?", -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db.handle));
        }
        sqlite3_bind_int64(stmt, 1, now_unix());
        bind_text(stmt, 2, id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string error = sqlite3_errmsg(db.handle);
            sqlite3_finalize(stmt);
            throw std::runtime_error(error);
        }
        const int changed = sqlite3_changes(db.handle);
        sqlite3_finalize(stmt);
        return {{"ok", true}, {"id", id}, {"archived", changed > 0}, {"db_path", db_path(slot_save_path)}};
    } catch (const std::exception & exc) {
        return sqlite_error(exc);
    }
#else
    GGML_UNUSED(slot_save_path);
    GGML_UNUSED(id);
    return sqlite_disabled_error();
#endif
}

}
