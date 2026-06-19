#pragma once

#include "server-common.h"

#include <string>

namespace local_ai_memory {

std::string db_path(const std::string & slot_save_path);
json kv_cache_metadata(const std::string & slot_save_path, int id_slot, const std::string & filename);
json record_kv_event(const std::string & slot_save_path, const std::string & event, int id_slot, const std::string & filename, size_t size_bytes);

json medium_memory_upsert(const std::string & slot_save_path, const json & body);
json medium_memory_list(const std::string & slot_save_path, const std::string & owner_id, int limit, bool include_embedding);
json medium_memory_search(const std::string & slot_save_path, const json & body);
json medium_memory_delete(const std::string & slot_save_path, const std::string & id);

}
