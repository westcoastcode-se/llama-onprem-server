# Callisto

Callisto is a local model server. The `callisto` binary loads one GGUF and speaks the OpenAI Responses API, so Codex CLI and Copilot CLI can use it. The server generates text and returns `function_call` items. The client runs the tools.

llama.cpp is vendored under `vendors/llama.cpp`. You do not start a separate `llama-server`.

| Guide | Contents |
|---|---|
| [Build](build.md) | Dependencies, Debug, Release, CUDA, and tests |
| [Use](using.md) | Server flags, API key, Codex CLI, and Copilot CLI |
| [Models](models.md) | Qwen, Devstral, and the other tool-call families |
| [Recording](recording.md) | How `example.gif` was recorded |
