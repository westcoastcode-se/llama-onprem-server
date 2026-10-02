# Use

Start the server first, then the client in the project the agent should read and change. The default address is `127.0.0.1:8080`.

## Model

Download a GGUF. The server uses the Jinja template stored in that file. `--chat-template PATH` replaces it.

```bash
pip install huggingface_hub
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(
    repo_id="unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF",
    allow_patterns=["*Devstral-Small-2-24B-Instruct-2512-UD-Q5_K_XL.gguf"],
    local_dir="Devstral-Small-2-24B-Instruct-2512-GGUF",
)
EOF
```

## Server

`-c` is the context length. `-ngl 99` puts the layers on the GPU when the server is built with CUDA.

```bash
./cmake-build-release/callisto_server \
  -m Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99
```

The same values can live in JSON. Short flags use names such as `model`, `context`, `batch`, `gpu-layers`, and `temperature`. Longer options keep their names, such as `top-p` and `port`. `reasoning` is a boolean. `session-cache-size` is an integer or a string such as `"8G"`. A later file overrides the keys it sets. Flags on the command line override the file.

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
| `--host HOST` | 127.0.0.1 | Bind address |
| `-p`, `--port N` | 8080 | Port |

`--kv-sessions` is accepted and unused. Parked KV is one file per session.

### Devstral

A GGUF whose file name contains `devstral` is the Devstral family. The same match applies to a `--chat-template` file name. Tool calls are `[TOOL_CALLS]name[ARGS]{"arg":"value"}`. Devstral does not open a `<think>` block, so the reply is the answer even when `--reasoning` is on. Use temperature `0.15`. `--min-p 0.01` is a good match for that model. Context of at least 16384 is the practical minimum.

```bash
pip install huggingface_hub
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(
    repo_id="unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF",
    allow_patterns=["*UD-Q4_K_XL.gguf"],
    local_dir="Devstral-Small-2-24B-Instruct-2512-GGUF",
)
EOF
./cmake-build-release/callisto_server \
  -m Devstral-Small-2-24B-Instruct-2512-GGUF/Devstral-Small-2-24B-Instruct-2512-UD-Q4_K_XL.gguf \
  -c 32768 -ngl 99 -t 0.15 --min-p 0.01 -p 8081
```

Point the client at Qwen first and Devstral second. The first server that answers is used. `/model` switches.

```json
{
  "server": ["qwen=127.0.0.1:8080", "devstral=127.0.0.1:8081"]
}
```

By default the conversation `<id>.json` is under `~/.local/state/callisto/sessions` and the KV cache `<id>.kv` is under `~/.cache/callisto/sessions`. `$XDG_STATE_HOME` and `$XDG_CACHE_HOME` replace those homes. `--session-dir PATH` stores both files in `PATH`. `<id>.json` holds the system prompt, messages, tools, and a pending tool call or question. It is written when the session is created and after each turn. A restarted server loads those files back. `<id>.kv` is written when another session takes the context and again on a clean shutdown. Both files are removed when the session is deleted, after 10 minutes idle, or when `--session-cache-size` is set and the oldest sessions no longer fit. The cap counts the conversation files and the KV files together. A session that is in use is not idle.

`callisto_server --help` prints the same list.

## Client

```bash
./cmake-build-release/callisto_cli --host 127.0.0.1 -p 8080
```

Those settings can also come from a JSON file. `--config-file` may be repeated. A later file overrides the keys it sets, and a flag overrides the file. `server` is one `[model=]host:port` string or an array of them.

```bash
./cmake-build-release/callisto_cli --config-file callisto.json --approval auto
```

```json
{
  "host": "127.0.0.1",
  "port": 8080,
  "server": ["qwen=127.0.0.1:8080"],
  "approval": "read-only",
  "theme": "nord",
  "show-think": true,
  "questions": true,
  "compress-tools": true
}
```

With no subcommand, a fullscreen session opens in the current directory. You can pass a first task as arguments.

`COLORTERM=truecolor` gives more colors. The theme comes from `--theme`, or from `$XDG_CONFIG_HOME/callisto/theme.json` (default `~/.config/callisto/theme.json`) when that file exists. Built-in names are `default`, `ink`, `nord`, `forest`, and `ember`. A path is a JSON file. An object can set `"theme"` to a built-in name and override colors with a palette name or `#rrggbb`.

Thinking stays on one line while it runs, the last three lines, then closes. A finished thinking row and a finished tool row show how long they took once that time reaches one second. A tool call is one line and opens while you answer the approval question. The reply sits in a box. The context meter is in the upper right. Click a thinking, tool, or system row, or press `Ctrl-O`, to open or close it. `Ctrl-C` cancels the current generation. A running command is aborted, and the server is told how long it ran. `Ctrl-D` or `/exit` leaves. `/quit` leaves as well.

### Several models

One model is one server. Put the list in `server` in the config file, or pass `--server`. The client uses the first server in that list when it answers, and otherwise the next one that answers. `/model` opens a dialog. `/model name` or `/model 1` switches directly. Switching starts a new session on that server and copies the conversation, so the next turn continues there.

```bash
./cmake-build-release/callisto_cli --config-file callisto.json
```

```json
{
  "server": ["qwen=127.0.0.1:8080", "devstral=127.0.0.1:8081"]
}
```

```bash
./cmake-build-release/callisto_cli \
  --server qwen=127.0.0.1:8080 \
  --server devstral=127.0.0.1:8081
```

`--server` replaces the `server` list from the config file. With neither, `--host` and `-p` are the single server. HTTP subcommands such as `health` and `session` always use `--host` and `-p`.

The fullscreen client still opens when no server answers. Status stays `offline`. The next message connects to the first server in the list that answers.

### Flags

| Flag | Default | Meaning |
|---|---|---|
| `--config-file PATH` | | JSON settings. Repeat to layer files. Flags override the file |
| `--host` | 127.0.0.1 | Server |
| `-p`, `--port` | 8080 | Port |
| `--server SPEC` | | `[model=]host:port`. Repeat it, or separate entries with commas |
| `--approval` | read-only | `read-only`, `auto`, or `full` |
| `--resume` | off | Continue the session in `~/.local/state/callisto/last-session` for this directory and server |
| `--session ID` | | Continue an id created on this computer |
| `--show-think` / `--hide-think` | shown | The thinking line |
| `--debug` | off | Leave tool-call XML in the assistant text |
| `--questions` / `--no-questions` | on | Let the model pause and ask a question |
| `--compress-tools` / `--no-compress-tools` | on | Applies to a new session. Finished tool results shrink when the next message is saved |
| `--theme` | `theme.json` when that file exists | Name or path |
| `--json` | off | Print the API object as JSON |
| `-v`, `--verbose` | off | HTTP log |

`callisto_cli --help` and `--help-all` list the subcommands.

## Sessions

The client resumes only sessions created on this computer. The list is `~/.local/state/callisto/known-sessions` (`$XDG_STATE_HOME/callisto/known-sessions`). A session from another computer is refused. `/resume` lists your sessions that the server still holds, newest first, including ones restored after a restart. `--resume` takes the latest session for this directory and this server. `--session ID` takes an id from that list.

`POST /v1/sessions` with an `id` resumes that session, leaves the conversation unchanged, and refreshes its timestamp. The other fields in that body are ignored. A missing or zero `id` creates a session. An unknown id is 404.

`GET /v1/sessions/{id}` is the header. The transcript is `GET /v1/sessions/{id}/messages`. Opening a transcript refreshes the timestamp, so a newer resume is kept.

The system prompt is fixed when the session is created. A changed prompt shows up in a new session on a server built after the change.

When context is strictly over 80% and the server is waiting for tools or for an answer, you can choose `Continue`, `Compact and continue`, or `Compact and stop`. Exactly 80% does not offer the choice. Esc cancels without compacting. `exec` without a terminal, and sub-agents, are not prompted. `/compact` summarizes the chat into a new session and stops at the prompt.

## Approval

`--approval read-only` is the default. Reads run immediately. A write, a shell command, or a network tool asks first.

- `y` this once
- `n` no
- `a` always this tool
- `f` full access for the rest of the session

`auto` also allows writes whose path is inside the working directory. Shell and network tools still ask. `full` asks for nothing. In `exec` without a terminal, a call that would have asked is denied.

`/approval` shows the mode. `/approval read-only`, `/approval auto`, and `/approval full` change it. `suggest` is the same mode as `read-only`.

## Commands in the session

| Command | What it does |
|---|---|
| `/help` | Show the commands |
| `/model [name]` | Open the server dialog, or switch by name or number |
| `/approval [mode]` | Show or set `read-only`, `auto`, or `full` |
| `/status` | Session, approval, server, and directory |
| `/diff` | `git diff --stat` in the working directory |
| `/compact` | Summarize the chat into a new session and stop |
| `/clear` | Start a new session |
| `/resume` | Continue a session created on this computer |
| `/exit` | Leave |

## Project files

`AGENTS.md` in the project root is added to the system prompt when the file has text. The prompt tells the model to look at the root files and determine what kind of project this is.

A skill is `.agents/skills/<name>/SKILL.md` in the project, or `~/.agents/skills/<name>/SKILL.md` for the user. A project skill with the same name is the one listed. The prompt lists the name, one line, and the path. The model reads the file when the task needs that procedure.

Wide exploration belongs in `sub_agent`. The sub-agent runs, then is summarized, and the parent receives only the last assistant message. A sub-agent does not start its own sub-agents. `exec` does not register `sub_agent`.

## Tools

| Tool | Kind | Approval in `read-only` |
|---|---|---|
| `read_file` | read | runs |
| `list_directory` | read | runs |
| `file_search` | read | runs |
| `search_text` | read | runs |
| `edit_file` | write | asks |
| `write_file` | write | asks |
| `execute_command` | shell | asks |
| `web_fetch` | network | asks |
| `web_search` | network | asks |
| `sub_agent` | other | asks |

`edit_file` applies a unified diff. A patch of only added lines creates the file. A change is matched by its context lines. The result is the diff that landed. `web_search` calls SearXNG at `http://localhost:4488`. Start it with `docker compose up -d`.

With `--compress-tools`, tool results stay in full while calls are in progress. They shrink to one line when the next user message is saved. Small results, at most 240 characters and 3 lines, and sub-agent results stay whole.

## One task from a script

```bash
./cmake-build-release/callisto_cli exec "Summarize the README"
```

`exec` prints plain text and then exits. The same session and approval flags apply.

## HTTP from the terminal

These subcommands talk to the API. They do not run local tools.

```bash
./cmake-build-release/callisto_cli health
./cmake-build-release/callisto_cli session list
./cmake-build-release/callisto_cli session create --json
./cmake-build-release/callisto_cli session get ID
./cmake-build-release/callisto_cli session delete ID
./cmake-build-release/callisto_cli send ID "the text"
./cmake-build-release/callisto_cli job get ID JOB
./cmake-build-release/callisto_cli job cancel ID JOB
```

`session create --id ID` resumes a session created on this computer. `session snapshot ID` clones a session. `send` with no text reads stdin. `tools ID` posts a tool result and streams the reply. `--file` is a JSON array, or an object with `tool_results`. `--help-all` shows the remaining flags.
