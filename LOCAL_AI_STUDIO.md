# Local AI Studio llama.cpp Fork

This fork adds backend-only KV cache helper endpoints on top of llama.cpp's existing slot save/restore support.
It also removes the built-in llama.cpp Web UI routes so `llama-server` always runs as an API-only process for Local AI Studio.

Start `llama-server` with:

```bash
llama-server ... --slot-save-path ./llama-slots
```

The standard `--ui` / `--webui` flags are intentionally ignored by this fork's HTTP route setup. Local AI Studio has its own frontend, so `/` and embedded static UI assets are not served.

Endpoints:

- `GET /local-ai/kv-cache/{slotId}?filename={conversationId}.bin`
  - Saves the slot through the existing slot save task.
  - Returns the saved KV cache bytes as `application/octet-stream`.
- `GET /local-ai/kv-cache/{slotId}?filename={conversationId}.bin&store=1`
  - Saves the slot through the existing slot save task.
  - Stores the cache under `--slot-save-path`.
  - Appends a row to `local-ai-kv-cache.db.jsonl`.
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
  - Appends a deletion row to `local-ai-kv-cache.db.jsonl`.

Security note: keep llama.cpp bound to `127.0.0.1`. These endpoints are for the Local AI Studio backend only.
