# Callisto

Callisto is a local coding agent. Two programs share the work:

- `callisto_server` keeps sessions, generates text, and exposes it over HTTP. The server does not run tools.
- `callisto_cli` is the agent. It talks to the server and runs tools in the directory where you start it.

llama.cpp is vendored under `vendors/llama.cpp`. You do not start a separate `llama-server`.

| Guide | Contents |
|---|---|
| [Build](build.md) | Dependencies, Debug, Release, CUDA, and tests |
| [Use](using.md) | Server, client, sessions, approval, and tools |
