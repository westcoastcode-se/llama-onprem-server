**DISCLAIMER: Use this project and any code it runs at your own risk.**

# Callisto

Callisto is a local coding agent. Two programs share the work:

1. **`callisto_server`** keeps chat sessions, generates text and exposes it over HTTP REST. It does not run tools.
2. **`callisto_cli`** is the agent. It talks to the server over HTTP and runs tools locally.

llama.cpp is vendored under `vendors/llama.cpp`. You do not clone or start `llama-server` yourself.

Build and day-to-day use are in [docs/](docs/README.md).

## The client

With no subcommand, `callisto_cli` opens a fullscreen session in the current directory. Thinking stays on one line until you open it. A tool call is one line, and opens while you answer the approval question. The assistant reply sits in a box. The context meter is in the upper right.

![Callisto client](example.gif)

`exec` runs one task as plain text and then exits, for scripts. `health`, `session`, `send`, `job`, and `tools` talk to the HTTP API directly.

# Compile

You need CMake 4.1 or newer, a C++23 compiler, libcurl, and git. Ninja is optional. CUDA is optional and only needed for GPU inference.

Arch:

```bash
sudo pacman -S --needed base-devel git cmake ninja curl
```

Debian or Ubuntu: install the same packages with `apt` (`build-essential`, `cmake`, `ninja-build`, `libcurl4-openssl-dev`, `git`). Add the CUDA toolkit when you want GPU layers.

From the repository root:

```bash
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug -j$(nproc)
```

That produces:

- `cmake-build-debug/callisto_server`
- `cmake-build-debug/callisto_cli`
- `cmake-build-debug/tests`

A Release build is the one to run a model with:

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target callisto_server callisto_cli -j$(nproc)
```

GPU layers need CUDA turned on. `nvidia-smi --query-gpu=name,compute_cap --format=csv` prints the architecture number. An RTX 40-series card is `89`.

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build cmake-build-release --target callisto_server callisto_cli -j$(nproc)
```

The devcontainer image has no CUDA. `docker build . -t local_ai:latest` packages the binaries already built in `cmake-build-debug`. Build those first.

# Run

Download a GGUF model. This tree ships chat templates for Qwen3.8-27B and Ternary-Bonsai-2-27B under `src/templates/`. Pass `--chat-template` when the file inside the GGUF is not the one you want.

```bash
pip install huggingface_hub
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(repo_id="unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF", allow_patterns=["*Devstral-Small-2-24B-Instruct-2512-UD-Q3_K_XL.gguf"], local_dir="Devstral-Small-2-24B-Instruct-2512-GGUF")
EOF
```

Start the server. `-c` is the context length. `-ngl 99` offloads layers to the GPU. The default bind address is `127.0.0.1:8080`.

```bash
./cmake-build-release/callisto_server \
  -m Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99
```

A GGUF whose file name contains `devstral` is parsed as Devstral. Run it on another port and list it after Qwen in the client config. Temperature `0.15` and `--min-p 0.01` match that model. Details are in [docs/using.md](docs/using.md).

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

Each session is stored under `/tmp/.callisto/sessions`, or the directory given with `--session-dir`. `<id>.json` is the conversation: the system prompt, messages, tools, and a pending tool call or question. It is written when the session is created and after each turn, and a restarted server loads those files back into the session list. `<id>.kv` is that session's KV cache and token ids, written by `llama_state_seq_save_file` when another session takes the context and again on a clean shutdown. Both files are removed when the session is deleted or after 10 minutes idle. `--session-cache-size` caps the directory (for example `8G`; `0` means no cap). When the files no longer fit, the oldest sessions are removed. Opening a transcript, with `/resume` or `GET /v1/sessions/{id}/messages`, refreshes that session's updated time so a newer resume is kept. `POST /v1/sessions` with an `id` resumes that session, returns its conversation, and refreshes the same time. The other fields in that body are ignored. A missing or zero `id` creates a session. An unknown id is 404. `GET /v1/sessions/{id}` returns the session header. The transcript is `GET /v1/sessions/{id}/messages`.

Start the client in the project you want it to edit:

```bash
./cmake-build-release/callisto_cli --host 127.0.0.1 -p 8080
```

The same client settings can live in a JSON file and be passed with `--config-file`. `server` is one `[model=]host:port` string, or an array of them. `show-think`, `questions`, `compress-tools`, `resume`, `json`, and `verbose` are booleans. A later file overrides the keys it sets. Flags on the command line override the file.

```bash
./cmake-build-release/callisto_cli --config-file callisto.json
```

```json
{
  "host": "127.0.0.1",
  "port": 8080,
  "server": ["qwen=127.0.0.1:8080", "devstral=127.0.0.1:8081"],
  "approval": "read-only",
  "theme": "nord"
}
```

Several models means several servers. Pass them with `--server`, or set `server` in the config file. The client uses the first server in that list when it answers, and otherwise the next one that answers. `/model` opens a dialog to switch. Switching starts a new session on that server and copies the conversation so far, so the next turn continues there.

```bash
export COLORTERM=truecolor ./cmake-build-release/callisto_cli \
  --server qwen=127.0.0.1:8080 \
  --server devstral=127.0.0.1:8081
```

Setting `COLORTERM=truecolor` is optional but gives you the best color experience.

`--server` replaces the `server` list from the config file. With neither, `--host` and `-p` are the single server. HTTP subcommands such as `health` and `session` still use `--host` and `-p`.

The fullscreen client still opens when every server is down. Status stays `offline`, and a chat message reports that none are reachable. The next message connects to the first server that answers.

One task from a script:

```bash
./cmake-build-release/callisto_cli exec "Summarize the README"
```

`--approval` is `read-only` by default. `auto` also allows writes inside the working directory. `full` asks for nothing. `--resume` continues the session saved in `~/.agents/last-session` for this directory and server, when this computer created it, and shows its transcript. `--session ID` does the same for an id listed in `~/.agents/sessions`. `/resume` lists only sessions this computer created that the server still holds, including ones restored after a restart. A session id from another computer is refused. A session idle for 10 minutes is collected. `--hide-think` hides the thinking line. `--debug` leaves tool-call XML in the assistant text.

`web_search` calls a local SearXNG on port 4488. Start it with:

```bash
docker compose up -d
```

# Using the session

Reads run without a prompt. A write, a shell command, or a network tool asks first. The list is:

- **Yes, this once** (`y`)
- **No** (`n`)
- **Always this tool** (`a`)
- **Full access** (`f`)

Click a thinking line or a tool line to open it. `Ctrl-O` toggles the latest one. `Ctrl-C` cancels the current generation. `Ctrl-D` or `/exit` leaves.

| Command | What it does |
|---|---|
| `/help` | Show the commands |
| `/model [name]` | Open the server dialog, or switch by name |
| `/approval [mode]` | Show or set `read-only`, `auto`, or `full` |
| `/status` | Session id, approval mode, and server |
| `/diff` | `git diff --stat` for the working directory |
| `/compact` | Summarize the chat into a new session and stop |
| `/clear` | Start a new session |
| `/resume` | Continue a session created on this computer |
| `/exit` | Leave |

`AGENTS.md` in the project root is added to the system prompt when it has text. The prompt tells the model to look at the project root and determine what kind of project it is. When the question needs more than that listing, the model can call `sub_agent`. The sub-agent's result should describe what the question needs: the kind of project, how it is built and tested, and the paths that matter. A skill is `.agents/skills/<name>/SKILL.md`. The prompt lists each skill's name and one line. The model reads the file only when the task needs that procedure. Wide exploration belongs in `sub_agent`, which returns a summary and leaves the file contents out of the parent session.

Tools the client can run: `read_file`, `edit_file`, `list_directory`, `file_search`, `search_text`, `execute_command`, `web_fetch`, `web_search`, and `sub_agent`. `edit_file` applies a unified diff. A patch of only added lines creates the file. A change is matched by its context lines. The result is the diff that landed.

# Credits

Callisto sits on other people's work. Thank you.

| Project | Role | License |
|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) and ggml | Model runtime in `callisto_server` | MIT, © 2023-2026 The ggml authors |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON, both programs | MIT, © 2013-2025 Niels Lohmann |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | HTTP, both programs | MIT, © Yuji Hirose |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Fullscreen client | MIT, © 2019 Arthur Sonzogni |
| [CLI11](https://github.com/CLIUtils/CLI11) | Command-line parsing | BSD-3-Clause, © 2017-2025 University of Cincinnati, Henry Schreiner |
| [subprocess.h](https://github.com/sheredom/subprocess.h) | Process helper inside llama.cpp | The Unlicense |
| [libcurl](https://curl.se) | HTTP fetch in the client | curl license, system library |

The license texts those projects require for a binary build are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Source copies keep their own `LICENSE` files. CLI11's BSD-3-Clause terms forbid using the University of Cincinnati or the contributors' names to endorse a product. The thanks above is attribution, not an endorsement.

SearXNG (AGPL-3.0) and Valkey (BSD-3-Clause) are optional separate services started by `docker-compose.yml`. They are not part of the Callisto binaries.
