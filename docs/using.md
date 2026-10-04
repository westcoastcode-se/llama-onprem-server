# Use

Start the server, then point Codex CLI at it. The default address is `127.0.0.1:8080`. There is no authentication.

## Server

`-c` is the context length. `-ngl 99` puts the layers on the GPU when the server is built with CUDA.

```bash
./cmake-build-release/callisto_server \
  -m Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99
```

The same values can live in JSON. Short flags use names such as `model`, `context`, `batch`, `gpu-layers`, and `temperature`. Longer options keep their names, 
such as `top-p` and `port`. `reasoning` is a boolean. `session-cache-size` is an integer or a string such as `"8G"`. 
A later file overrides the keys it sets. Flags on the command line override the file.

```bash
./cmake-build-release/callisto_server --config-file callisto-server.json -p 8081
```

```json
{
  "model": "Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf",
  "context": 32768,
  "gpu-layers": 99,
  "host": "127.0.0.1"
}
```

| Flag | Default | Meaning |
|---|---|---|
| `-m PATH` | required | GGUF file |
| `-c N` | 4096 | Context length |
| `-b N` | 2048 | Batch size |
| `-ngl N` | 99 | GPU layers |
| `-t F` | 1.0 | Temperature |
| `--top-p F` | 0.95 | Nucleus sampling |
| `--top-k N` | 20 | Top-k. `0` turns it off |
| `--min-p F` | 0 | Min-p. `0` turns it off |
| `--presence-penalty F` | 0 | Presence penalty |
| `--frequency-penalty F` | 0 | Frequency penalty |
| `--repetition-penalty F` | 1.0 | Repetition penalty. `1.0` turns it off |
| `--penalty-last-n N` | 64 | Penalty window |
| `--seed N` | random | Sampler seed |
| `--max-tokens N` | -1 | Cap per reply. `-1` fills the context |
| `--threads N` | 0 | Threads while generating. `0` leaves llama.cpp's choice |
| `--threads-batch N` | 0 | Threads during prefill |
| `--flash-attn` | auto | `auto`, `on`, or `off` |
| `--cache-type-k TYPE` | f16 | KV type for K |
| `--cache-type-v TYPE` | f16 | KV type for V |
| `--chat-template PATH` | the template in the GGUF | Your own Jinja file |
| `--reasoning` / `--no-reasoning` | on | `enable_thinking` |
| `--session-dir PATH` | `~/.local/state/callisto/sessions` and `~/.cache/callisto/sessions` | Conversation and KV. `PATH` stores both in one directory |
| `--session-cache-size SIZE` | 0 | Cap in bytes. `0` means no cap. Suffixes `K`, `M`, `G`, `T` |
| `--kv-sessions N` | unused | Accepted and unused. Parked KV is one file per session |
| `--config-file PATH` | | JSON settings. A later file overrides the keys it sets. Flags override the file |
| `--host HOST` | 127.0.0.1 | Bind address |
| `-p`, `--port N` | 8080 | Port |

## Codex CLI

Codex sends one stateless `POST /v1/responses` per turn, with the whole history in `input`. The server generates. Codex runs the tools and sends the results back as `function_call_output`.

### Install

Install [Codex CLI](https://learn.chatgpt.com/docs/codex/cli) with one of these. The same command updates an existing install. Homebrew uses `brew upgrade --cask codex` instead.

macOS or Linux, without Node.js:

```bash
curl -fsSL https://chatgpt.com/codex/install.sh | sh
```

Windows, from PowerShell:

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://chatgpt.com/codex/install.ps1 | iex"
```

npm. The package name is `@openai/codex`:

```bash
npm install -g @openai/codex
```

Homebrew:

```bash
brew install --cask codex
```

Run `codex` in a project directory. The first run asks you to sign in with ChatGPT or another method. Then point it at this server.

Put this in `~/.codex/config.toml`. Provider keys in a project `.codex/config.toml` are ignored.

```toml
model = "local"
model_provider = "callisto"

[model_providers.callisto]
name = "Callisto"
base_url = "http://127.0.0.1:8080/v1"
wire_api = "responses"
```

`base_url` must end in `/v1`. Codex appends `/responses`. `wire_api` must be `"responses"`. Any `model` string is accepted and echoed. Do not set `env_key`. The server does not check a bearer token.

`POST /responses` is the same handler when the base URL has no `/v1`.

The reply is server-sent events when `stream` is true, which is what Codex sends. The final event is `response.completed`, `response.incomplete` with reason `interrupted` when the turn is cancelled, or `response.failed`. A request the server will not run returns `400` with `{"error":{"message","type":"invalid_request_error"}}`.

`prompt_cache_key` is the KV slot. Letters, digits, `.`, `_`, and `-` are kept. Anything else becomes `_`. The name is at most 120 characters. With no key, the slot is `codex`. One engine sequence is live. Another key parks the current sequence in `<id>.kv` and loads that key's file.

`instructions` is the system text. `developer` messages are treated as system text and folded into that first message. A reasoning item whose content and summary are empty is skipped. `previous_response_id` is ignored, because Codex resends the input. Tools other than `function` are skipped. Image, audio, and file inputs are rejected.

`GET /v1/models` and `GET /models` return the loaded GGUF file name, without the directory or the `.gguf` suffix. `owned_by` is `callisto`.

## Sessions

Codex does not call the session API. It remains for a caller that keeps its own transcript and posts tool results itself. The shapes are in [src/api/openapi.yaml](../src/api/openapi.yaml).

`POST /v1/sessions` with an `id` resumes that session and refreshes its timestamp. A missing or zero `id` creates a session. An unknown id is 404. `GET /v1/sessions/{id}` is the header. The transcript is `GET /v1/sessions/{id}/messages`. A turn that contains tool calls pauses until `POST /v1/sessions/{id}/tools`.

The conversation file is `<id>.json` under `$XDG_STATE_HOME/callisto/sessions` (default `~/.local/state/callisto/sessions`). KV files are `<id>.kv` under `$XDG_CACHE_HOME/callisto/sessions` (default `~/.cache/callisto/sessions`). `--session-dir PATH` stores both in that directory. With no home directory and no XDG variable, both are under `/tmp/callisto/sessions`. `--session-cache-size` caps the combined size. `0` means no cap. A session that is generating, or waiting for tools or an answer, is kept. Idle sessions are removed after 10 minutes. At most 256 sessions are kept.
