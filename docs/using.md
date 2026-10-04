# Use

Start the server, then point Codex CLI or Copilot CLI at it. The default address is `127.0.0.1:8080`. The API is open unless you set an API key. `GET /health` never checks that key.

## Server

`-c` is the context length. `-ngl 99` puts the layers on the GPU when the server is built with CUDA.

```bash
./cmake-build-release/callisto_server \
  -m Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99
```

The same values can live in JSON. Short flags use names such as `model`, `context`, `batch`, `gpu-layers`, and `temperature`. Longer options keep their names, such as `top-p`, `port`, and `api-key`. `reasoning` is a boolean. A later file overrides the keys it sets. Flags on the command line override the file. `CALLISTO_API_KEY` applies only when the flag and the file leave `api-key` empty.

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
| `--session-dir PATH` | `$XDG_CACHE_HOME/callisto/sessions` | Parked KV files. `PATH` stores them in that directory |
| `--session-cache-size SIZE` | unused | Accepted and unused |
| `--kv-sessions N` | unused | Accepted and unused. Parked KV is one file per id |
| `--api-key KEY` | empty | Require `Authorization: Bearer KEY`. Empty leaves the API open |
| `--config-file PATH` | | JSON settings. A later file overrides the keys it sets. Flags override the file |
| `--host HOST` | 127.0.0.1 | Bind address |
| `-p`, `--port N` | 8080 | Port |

`--api-key` and the config key `api-key` override the environment. When both are empty, `CALLISTO_API_KEY` is the key. A wrong or missing bearer token on any route except `GET /health` returns `401` with `{"error":{"message":"invalid api key","type":"invalid_request_error","code":"invalid_api_key"}}`.

The process still creates `$XDG_STATE_HOME/callisto/sessions` (default `~/.local/state/callisto/sessions`). It does not write conversation files and it has no session HTTP API. KV files are `<id>.kv`. With no home directory and no XDG variable, KV files are under `/tmp/callisto/sessions`.

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
requires_openai_auth = false
```

`base_url` must end in `/v1`. Codex appends `/responses`. `wire_api` must be `"responses"`. Any `model` string is accepted and echoed. `requires_openai_auth = false` skips the ChatGPT login. Leave out `env_key` when the server has no API key. Codex then sends no `Authorization` header.

### API key

`env_key` is the name of an environment variable, not the secret. Writing that name in `config.toml` does not set the variable. Codex reads it from the process that sends the request. An empty value counts as missing.

Interactive `codex` uses a shared background server. That server keeps the environment it had when it started. Exporting `CALLISTO_API_KEY` in a terminal does not change a server that is already running, so Codex reports `Missing environment variable: CALLISTO_API_KEY`.

Export the same secret this server requires, restart the background server from that shell, and then start Codex:

```toml
requires_openai_auth = false
env_key = "CALLISTO_API_KEY"
```

```bash
export CALLISTO_API_KEY=the-secret
codex app-server daemon restart
codex
```

`codex --no-daemon` skips the background server and reads the variable from that shell. `codex exec` does too.

Codex sends `Authorization: Bearer` with that value. The server must require the same secret. Start it in a shell where `CALLISTO_API_KEY` is already set and pass neither `--api-key` nor `api-key` in the config file, and the server reads the variable itself. Restart the server after exporting it. `--api-key` and the config-file key win over the environment variable, so a server started that way still accepts this bearer token only when the values match. A missing or different token is `401` with `invalid_api_key`. `GET /health` does not check it.

`POST /responses` is the same handler when the base URL has no `/v1`.

The reply is server-sent events when `stream` is true, which is what Codex sends. The final event is `response.completed`, `response.incomplete` with reason `interrupted` when the turn is cancelled, or `response.failed`. A request the server will not run returns `400` with `{"error":{"message","type":"invalid_request_error"}}`.

`prompt_cache_key` is the KV slot. Letters, digits, `.`, `_`, and `-` are kept. Anything else becomes `_`. The name is at most 120 characters. With no key, the slot is `codex`. One engine sequence is live. Another key parks the current sequence in `<id>.kv` and loads that key's file.

`instructions` is the system text. `developer` messages are treated as system text and folded into that first message. A reasoning item whose content and summary are empty is skipped, unless `encrypted_content` is plain text rather than a ciphertext blob. `previous_response_id` is ignored, because the client resends the input. Image, audio, and file inputs are rejected.

Tools of type `function` and `custom` are sent to the model. A `namespace` tool is flattened: each member is named `namespace.member`, and a `function_call` for that tool is returned with the leaf `name` and the `namespace` field. `tool_search` is accepted as a function named `tool_search` when the request omits a name. Hosted tools such as `web_search` are skipped. A flat tool whose name contains `.` is left as one name unless that prefix was declared as a namespace.

`GET /v1/models` and `GET /models` return the loaded GGUF file name, without the directory or the `.gguf` suffix. `owned_by` is `callisto`. Codex does not fill its `/model` picker from that list.

## Copilot CLI

Copilot CLI uses the same `POST /v1/responses` route as Codex. Start the server first, then start Copilot in a project directory with the variables below. Copilot runs the tools. The server generates and returns `function_call` items.

### Install

Install [GitHub Copilot CLI](https://docs.github.com/en/copilot/how-tos/set-up/install-copilot-cli) with one of these. The same command updates an existing install.

macOS or Linux, with Homebrew:

```bash
brew install --cask copilot-cli
```

macOS or Linux, with the install script:

```bash
curl -fsSL https://gh.io/copilot-install | bash
```

npm, on any platform. The package name is `@github/copilot`. Node.js 22 or later is required:

```bash
npm install -g @github/copilot
```

Windows, from PowerShell:

```powershell
winget install GitHub.Copilot
```

### Start

The default provider wire is Chat Completions (`POST /v1/chat/completions`). This server does not implement that route. Set `COPILOT_PROVIDER_WIRE_API=responses` so Copilot posts to `{base_url}/responses`.

`COPILOT_PROVIDER_BASE_URL` must end in `/v1`. `COPILOT_PROVIDER_TYPE=openai` is the OpenAI-compatible provider. `COPILOT_MODEL` is required for a custom provider. Any string is accepted and echoed. The server does not switch GGUF files.

`COPILOT_OFFLINE=true` skips GitHub login, telemetry, and other calls to GitHub. Prompts still go to this server. Without it, Copilot may ask you to sign in with GitHub even though the model requests use the local provider.

In the project directory, with the server already listening:

```bash
export COPILOT_PROVIDER_BASE_URL=http://127.0.0.1:8080/v1
export COPILOT_PROVIDER_TYPE=openai
export COPILOT_PROVIDER_WIRE_API=responses
export COPILOT_MODEL=local
export COPILOT_OFFLINE=true
copilot
```

Leave `COPILOT_PROVIDER_API_KEY` unset when the server has no API key. An empty value can make the request fail. When the server was started with `--api-key`, or with `api-key` in the config file, or with `CALLISTO_API_KEY`, set the same secret before `copilot`:

```bash
export COPILOT_PROVIDER_API_KEY=the-same-key
```

Copilot sends it as `Authorization: Bearer`. `GET /health` does not check the key.

If `~/.copilot/providers.json` declares any provider or model, that file overrides `COPILOT_PROVIDER_*`. `COPILOT_PROVIDERS_CONFIG` selects another path. Remove those entries, or point the file at this server, before relying on the variables above.

Copilot requires a model that streams and can call tools. GitHub recommends a context of at least 128k tokens. `-c` on the server is that limit. Image, audio, and file inputs are rejected. Namespace tools are flattened to `namespace.member` and returned with a `namespace` field, as described above.

The request and event shapes are in [src/api/openapi.yaml](../src/api/openapi.yaml). Provider variables are documented by GitHub under [Using your own LLM models](https://docs.github.com/en/copilot/how-tos/copilot-cli/customize-copilot/use-byok-models).
