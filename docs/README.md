# Callisto

Callisto is a local model server. `callisto_server` loads one GGUF and speaks the OpenAI Responses API, so Codex CLI can use it. The server generates text and returns `function_call` items. Codex runs the tools.

llama.cpp is vendored under `vendors/llama.cpp`. You do not start a separate `llama-server`.

| Guide | Contents |
|---|---|
| [Build](build.md) | Dependencies, Debug, Release, CUDA, and tests |
| [Use](using.md) | Server flags, installing Codex CLI, and the session API |
| [Models](models.md) | Compatible Models |
