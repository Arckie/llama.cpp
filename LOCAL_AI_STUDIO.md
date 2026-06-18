# Local AI Studio llama.cpp Fork

This fork adds backend-only KV cache helper endpoints on top of llama.cpp's existing slot save/restore support.

Start `llama-server` with:

```bash
llama-server ... --slot-save-path ./llama-slots
```

Endpoints:

- `GET /local-ai/kv-cache/{slotId}?filename={conversationId}.bin`
  - Saves the slot through the existing slot save task.
  - Returns the saved KV cache bytes as `application/octet-stream`.
- `POST /local-ai/kv-cache/{slotId}?filename={conversationId}.bin`
  - Accepts `application/octet-stream`.
  - Writes the bytes to `--slot-save-path`.
  - Restores the slot through the existing slot restore task.
- `DELETE /local-ai/kv-cache/{slotId}`
  - Erases the slot through the existing slot erase task.

Security note: keep llama.cpp bound to `127.0.0.1`. These endpoints are for the Local AI Studio backend only.
