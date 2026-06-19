# Local AI Studio llama.cpp Fork

This fork adds backend-only short-term and medium-term memory helper endpoints on top of llama.cpp's existing slot save/restore support.
It also removes the built-in llama.cpp Web UI routes so `llama-server` always runs as an API-only process for Local AI Studio.

Start `llama-server` with:

```bash
llama-server ... --slot-save-path ./llama-slots
```

The standard `--ui` / `--webui` flags are intentionally ignored by this fork's HTTP route setup. Local AI Studio has its own frontend, so `/` and embedded static UI assets are not served.

Memory model:

- Short-term memory is the active llama.cpp slot/KV cache up to the configured context size, such as 65,536 tokens.
- Medium-term memory is a local SQLite index in `local-ai-memory.db` under `--slot-save-path`.
- Long-term memory remains in the Local AI Studio backend database. llama.cpp stores only medium-term indexes and backend memory pointers.

KV cache endpoints:

- `GET /local-ai/kv-cache/{slotId}?filename={conversationId}.bin`
  - Saves the slot through the existing slot save task.
  - Returns the saved KV cache bytes as `application/octet-stream`.
- `GET /local-ai/kv-cache/{slotId}?filename={conversationId}.bin&store=1`
  - Saves the slot through the existing slot save task.
  - Stores the cache under `--slot-save-path`.
  - Records metadata in `local-ai-memory.db`.
  - Returns JSON metadata instead of transferring cache bytes.
- `GET /local-ai/kv-cache/{slotId}?filename={conversationId}.bin&metadata=1`
  - Returns metadata for the saved cache file and DB path without saving the slot.
- `POST /local-ai/kv-cache/{slotId}?filename={conversationId}.bin`
  - Accepts `application/octet-stream`.
  - Writes the bytes to `--slot-save-path`.
  - Restores the slot through the existing slot restore task.
- `POST /local-ai/kv-cache/{slotId}?filename={conversationId}.bin&restore=1`
  - Restores an already stored cache file without transferring bytes through the backend.
- `DELETE /local-ai/kv-cache/{slotId}?filename={conversationId}.bin`
  - Erases the slot through the existing slot erase task.
  - Removes the stored cache file when `filename` is provided.
  - Records the deletion event in `local-ai-memory.db`.

Medium memory endpoints:

- `POST /local-ai/memory/medium`
  - Upserts a medium-term index item with `id`, `owner_id`, `conversation_id`, `text`, `index_text`, `embedding`, `metadata`, and `importance`.
  - `metadata.longMemoryRef` may point back to the backend long-term memory row.
- `POST /local-ai/memory/medium/search`
  - Searches active medium-term indexes by cosine similarity over the stored embedding.
- `GET /local-ai/memory/medium?owner_id={ownerId}&limit=20`
  - Lists recent active medium-term indexes.
- `DELETE /local-ai/memory/medium/{id}`
  - Archives a medium-term index without deleting backend long-term memory.

Security note: keep llama.cpp bound to `127.0.0.1`. These endpoints are for the Local AI Studio backend only.
