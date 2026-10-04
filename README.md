**DISCLAIMER: Use this project and any code it runs at your own risk.**

# Callisto

Callisto is a local model server. `callisto_server` loads one GGUF and speaks the OpenAI Responses API, so [Codex CLI](https://github.com/openai/codex) can use it. The server generates text and returns `function_call` items. Codex runs the tools.

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

- `cmake-build-debug/callisto_server`
- `cmake-build-debug/tests`

A Release build is the one to run a model with:

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target callisto_server -j$(nproc)
```

GPU layers need CUDA turned on. `nvidia-smi --query-gpu=name,compute_cap --format=csv` prints the architecture number. An RTX 40-series card is `89`.

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build cmake-build-release --target callisto_server -j$(nproc)
```

The devcontainer image has no CUDA. `docker build . -t local_ai:latest` packages `callisto_server` already built in `cmake-build-release`, plus `LICENSE` and `THIRD_PARTY_NOTICES.md`. Build the server first. The same steps are in [docs/build.md](docs/build.md).

# Run

Download a GGUF model. The server uses the Jinja template stored in that file. Pass `--chat-template` when you want a different one. The file name of that template, or else the GGUF, selects the tool-call format. [docs/using.md](docs/using.md) lists the server flags.

```bash
pip install huggingface_hub
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(repo_id="unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF", allow_patterns=["*Devstral-Small-2-24B-Instruct-2512-UD-Q4_K_XL.gguf"], local_dir="Devstral-Small-2-24B-Instruct-2512-GGUF")
EOF
```

Start the server. `-c` is the context length. `-ngl 99` offloads layers to the GPU. The default bind address is `127.0.0.1:8080`. There is no authentication.

```bash
./cmake-build-release/callisto_server \
  -m Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99
```

A GGUF whose file name contains `devstral` is parsed as Devstral. Temperature `0.15` and `--min-p 0.01` match that model.

```bash
./cmake-build-release/callisto_server \
  -m Devstral-Small-2-24B-Instruct-2512-GGUF/Devstral-Small-2-24B-Instruct-2512-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99 -t 0.15 --min-p 0.01 -p 8081
```

The same arguments can be stored in a JSON file and passed with `--config-file`. Short flags use names such as `model`, `context`, `batch`, `gpu-layers`, and `temperature`. Longer options keep their names, such as `top-p` and `port`. `reasoning` is a boolean. `session-cache-size` is a byte count or a string such as `"8G"`. A later file overrides the keys it sets. Flags on the command line override the file.

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
```

`base_url` must end in `/v1`. Codex appends `/responses`. `wire_api` must be `"responses"`. The server accepts any `model` string and echoes it. Leave out `env_key`: the server does not check a bearer token.

`prompt_cache_key` from Codex is the KV slot for that thread. Letters, digits, `.`, `_`, and `-` are kept. Anything else becomes `_`, and the name is cut at 120 characters. With no key, the slot is `codex`. The file is `<id>.kv` under `$XDG_CACHE_HOME/callisto/sessions` (default `~/.cache/callisto/sessions`).

Image, audio, and file inputs are rejected. The server does not run tools.

`GET /health` returns `OK`. `GET /v1/models` lists the loaded GGUF file name, without the directory or the `.gguf` suffix.

The session HTTP API is still there for a caller that manages its own tools. Codex does not use it. [src/api/openapi.yaml](src/api/openapi.yaml) describes both.

Each session's conversation is `<id>.json` under `$XDG_STATE_HOME/callisto/sessions` (default `~/.local/state/callisto/sessions`). `--session-dir PATH` stores the conversation files and the KV files in that directory. `--session-cache-size` caps their combined size (for example `8G`; `0` means no cap). With no home directory and no XDG variable, both are under `/tmp/callisto/sessions`.

# Credits

Callisto sits on other people's work. Thank you.

| Project | Role | License |
|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) and ggml | Model runtime in `callisto_server` | MIT, © 2023-2026 The ggml authors |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON | MIT, © 2013-2025 Niels Lohmann |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | HTTP | MIT, © Yuji Hirose |
| [subprocess.h](https://github.com/sheredom/subprocess.h) | Process helper inside llama.cpp | The Unlicense |
| [libcurl](https://curl.se) | HTTP in the unit tests | curl license, system library |

The license texts those projects require for a binary build are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Source copies keep their own `LICENSE` files.
