**DISCLAIMER: Use this project and any code it runs at your own risk.**

# Callisto

Callisto is a local model server. `callisto` loads one GGUF and speaks the OpenAI Responses API, so [Codex CLI](https://github.com/openai/codex) can use it. The server generates text and returns `function_call` items. Codex runs the tools.

This is Callisto answering through Codex CLI. Speed and quality follow the model you load.

![Codex Example](example.gif)

llama.cpp is vendored under `vendors/llama.cpp`. You do not clone or start `llama-server` yourself.

Build and day-to-day use are in [docs/](docs/README.md).

# Compile

You need CMake 4.1 or newer, a C++23 compiler, libcurl, and git. Ninja is optional. CUDA is optional and only needed for GPU inference. libcurl is used by the unit tests.

Arch:

```bash
sudo pacman -S --needed base-devel git cmake ninja curl
```

Debian or Ubuntu: install the same packages with `apt` (`build-essential`, `cmake`, `ninja-build`, `libcurl4-openssl-dev`, `git`). Add the CUDA toolkit when you want GPU layers.

From the repository root. `vendors/llama.cpp` is a git submodule, so a fresh clone needs it checked out first:

```bash
git submodule update --init
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug -j$(nproc)
```

That produces:

- `cmake-build-debug/callisto`
- `cmake-build-debug/tests`

A Release build is the one to run a model with:

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target callisto -j$(nproc)
```

GPU layers need CUDA turned on. `nvidia-smi --query-gpu=name,compute_cap --format=csv` prints the architecture number. An RTX 40-series card is `89`.

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build cmake-build-release --target callisto -j$(nproc)
```

The devcontainer image has no CUDA. `docker build . -t callisto:latest` copies `cmake-build-release/callisto` to `/callisto`, plus `LICENSE` and `THIRD_PARTY_NOTICES.md`. Build the server first. The same steps are in [docs/build.md](docs/build.md).

# Run

Download a GGUF model. The server uses the Jinja template stored in that file. Pass `--chat-template` when you want a different one. The file name of that template, or else the GGUF, selects the tool-call format. [docs/using.md](docs/using.md) lists the server flags.

```bash
pip install huggingface_hub
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(repo_id="unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF", allow_patterns=["*Devstral-Small-2-24B-Instruct-2512-UD-Q4_K_XL.gguf"], local_dir="Devstral-Small-2-24B-Instruct-2512-GGUF")
EOF
```

Start the server. `-c` is the context length. `-ngl 99` offloads layers to the GPU. The default bind address is `127.0.0.1:8080`. The API is open unless `--api-key` or `CALLISTO_API_KEY` is set. `GET /health` never checks the key.

```bash
./cmake-build-release/callisto \
  -m Devstral-Small-2-24B-Instruct-2512-GGUF/Devstral-Small-2-24B-Instruct-2512-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99 -t 0.15 --min-p 0.01 --session-memory-mb 4GB
```

A GGUF whose file name contains `devstral` is parsed as Devstral. Temperature `0.15` and `--min-p 0.01` match that model. Other models and their starting flags are in [docs/models.md](docs/models.md).

The same arguments can be stored in a JSON file and passed with `--config-file`. Short flags use names such as `model`, `context`, `batch`, `gpu-layers`, and `temperature`. Longer options keep their names, such as `top-p`, `port`, and `api-key`. `reasoning` is a boolean. A later file overrides the keys it sets. Flags on the command line override the file. `CALLISTO_API_KEY` applies only when the flag and the file leave the key empty.

```bash
./cmake-build-release/callisto --config-file callisto.json -p 8081
```

```json
{
  "model": "Devstral-Small-2-24B-Instruct-2512-GGUF/Devstral-Small-2-24B-Instruct-2512-UD-Q4_K_XL.gguf",
  "context": 32768,
  "gpu-layers": 99,
  "host": "127.0.0.1"
}
```

# Codex CLI

Install [Codex CLI](https://learn.chatgpt.com/docs/codex/cli), then point it at the server. On macOS or Linux:

```bash
curl -fsSL https://chatgpt.com/codex/install.sh | sh
```

The other official installs are `npm install -g @openai/codex` and `brew install --cask codex`. On Windows, from PowerShell:

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://chatgpt.com/codex/install.ps1 | iex"
```

Run the same command again to update. Homebrew uses `brew upgrade --cask codex`. The npm package is `@openai/codex`. Run `codex` and sign in. [docs/using.md](docs/using.md) has the same steps next to the server flags.

Codex sends `POST /v1/responses` with the full conversation on every turn. Point it at the server from `~/.codex/config.toml`. A project `.codex/config.toml` does not override the provider.

```toml
model = "local"
model_provider = "callisto"

[model_providers.callisto]
name = "Callisto"
base_url = "http://127.0.0.1:8080/v1"
wire_api = "responses"
requires_openai_auth = false
```

`base_url` must end in `/v1`. Codex appends `/responses`. `wire_api` must be `"responses"`. The server accepts any `model` string and echoes it. `requires_openai_auth = false` skips the ChatGPT login. Leave out `env_key` when the server has no API key.

When the server requires a key, `env_key` is the name of an environment variable, not the secret. Add `env_key = "CALLISTO_API_KEY"` under `[model_providers.callisto]`. Interactive `codex` reads that variable from its background server, not from the terminal that launches it. Export `CALLISTO_API_KEY`, run `codex app-server daemon restart`, and then start `codex`. `codex --no-daemon` reads the variable from the current shell instead. The server reads that same variable when `--api-key` and the config-file key are both empty. Those two win when set, and the bearer token must match them. [docs/using.md](docs/using.md) has the full provider block.

`prompt_cache_key` from Codex is the KV slot for that thread. Letters, digits, `.`, `_`, and `-` are kept. Anything else becomes `_`, and the name is cut at 120 characters. With no key, the slot is `codex`. The file is `<id>.kv` under `$XDG_CACHE_HOME/callisto/sessions` (default `~/.cache/callisto/sessions`). `--session-dir PATH` stores those files in `PATH`. `--session-memory-mb SIZE` keeps parked sessions in RAM. A bare number is megabytes, and `MB` or `GB` sets the unit. `--session-disk-limit DAYS,SIZE` deletes files older than that many days when the directory is larger than that size. [docs/using.md](docs/using.md) has the details.

Image, audio, and file inputs are rejected. The server does not run tools. Namespace tools are flattened to `namespace.member` and returned with a `namespace` field. `custom` and `tool_search` are accepted. Hosted tools such as `web_search` are skipped.

`GET /health` returns `OK`. `GET /v1/models` lists the loaded GGUF file name, without the directory or the `.gguf` suffix. [src/api/openapi.yaml](src/api/openapi.yaml) describes the routes.

# Copilot CLI

Install [GitHub Copilot CLI](https://docs.github.com/en/copilot/how-tos/set-up/install-copilot-cli), start the server, then start Copilot in a project directory. On macOS or Linux with Homebrew:

```bash
brew install --cask copilot-cli
```

The other official installs are `npm install -g @github/copilot` (Node.js 22 or later) and `curl -fsSL https://gh.io/copilot-install | bash`. On Windows, from PowerShell: `winget install GitHub.Copilot`. [docs/using.md](docs/using.md) has the same steps next to the server flags.

The default wire is Chat Completions. This server implements `POST /v1/responses` only, so set `COPILOT_PROVIDER_WIRE_API=responses`. `COPILOT_PROVIDER_BASE_URL` must end in `/v1`. `COPILOT_MODEL` is any string and is echoed. `COPILOT_OFFLINE=true` skips GitHub login. Leave `COPILOT_PROVIDER_API_KEY` unset unless the server was given an API key.

```bash
export COPILOT_PROVIDER_BASE_URL=http://127.0.0.1:8080/v1
export COPILOT_PROVIDER_TYPE=openai
export COPILOT_PROVIDER_WIRE_API=responses
export COPILOT_MODEL=local
export COPILOT_OFFLINE=true
copilot
```

If `~/.copilot/providers.json` declares a provider or a model, it overrides `COPILOT_PROVIDER_*`.

# Credits

Callisto sits on other people's work. Thank you.

| Project | Role | License |
|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) and ggml | Model runtime in `callisto` | MIT, © 2023-2026 The ggml authors |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON | MIT, © 2013-2025 Niels Lohmann |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | HTTP | MIT, © Yuji Hirose |
| [subprocess.h](https://github.com/sheredom/subprocess.h) | Process helper inside llama.cpp | The Unlicense |
| [libcurl](https://curl.se) | HTTP in the unit tests | curl license, system library |

The license texts those projects require for a binary build are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Source copies keep their own `LICENSE` files.
