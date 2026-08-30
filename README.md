**DISCLAIMER: Use this project and executing any code from this project at your own risk.**

Most of the code in this project is vibe-coded. Keep that in mind when considering things like security flaws. The 
server itself is only running the actual AI. It won't run any tasks outside running the AI model itself.

The client, however, has support for lots of tasks that can be considered insecure. Examples are: running arbitrary
commands on the computer.

You can increase the security somewhat by running the client itself in a virtual machine or container using devcontainers.

# Introduction

This project is basically a way for me to improve my understanding on how AI models and agents work. Most of the code is
vibe-coded.

Complex tasks are delegated to subagents as a way to lower the complexity of the main agent context.

Experiment yourself how much context you allow the server to give the client. If you have a Nvidia 4090 GTX with 64GB 
RAM, you can use at least `-c 100000`. You can also use `Qwen3.8-27B-UD-Q4_K_XL.gguf` as a model, which is really 
good for that kind of hardware.

# Setup Dev

Install the necessary tools needed for the project to work — C++ development tools with CMake and Python.
Optionally with CUDA as well. Replace `pacman` with `apt` if you are building on a Debian system.

```bash
sudo pacman -S --needed base-devel git cmake ninja python curl cuda
```

Models can be found on https://huggingface.co, for example: https://huggingface.co/unsloth/Qwen3.8-27B-GGUF

## Checking out and building llama.cpp outside this project

Here is an example on how to build llama.cpp from scratch. You don't need to do this beforehand if you don't want to.

```bash
# Clone llama.cpp
git clone https://github.com/ggml-org/llama.cpp.git
cd llama.cpp

# Add cuda to PATH
export PATH=/opt/cuda/bin:$PATH
export LD_LIBRARY_PATH=/opt/cuda/lib64:$LD_LIBRARY_PATH

# Compile llama.cpp with CUDA support
cmake -B build \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES="89" \
  -DCMAKE_BUILD_TYPE=Release \
  -G Ninja
cmake --build build --config Release -j$(nproc)

# Setup Python tools
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# Download Qwen3.8-27B-UD-Q4_K_XL
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(repo_id="unsloth/Qwen3.8-27B-GGUF", allow_patterns=["*Qwen3.8-27B-UD-Q4_K_XL.gguf"], local_dir="Qwen3.8-27B-GGUF")
EOF
```

After building the project, you can start llama.cpp locally by:

```bash
# Start llama.cpp server
./build/bin/llama-server -m Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf -ngl 99 --host 0.0.0.0 --port 8080

# Or run one of the example applications with 32k tokens
./build/bin/llama-simple-chat -m Qwen3.8-27B-GGUF/Qwen3.8-27B-UD-Q4_K_XL.gguf -c 32768 -ngl 99
```

## Building this project

Check out this project:

```bash
git clone <this-repo> --recurse-submodules
```

Open the project and build it with your favorite IDE (e.g. CLion) or directly via CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES="89"
cmake --build build -j$(nproc)
```

## Build this project inside devcontainer

The devcontainer doesn't have access to CUDA. So build without it

## Download Qwen3.8

This project uses Qwen3.8 by default. Support for other models can also be used:

```bash
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(repo_id="unsloth/Qwen3.8-27B-GGUF", allow_patterns=["*Qwen3.8-27B-UD-Q4_K_XL.gguf"], local_dir="Qwen3.8-27B-GGUF")
EOF
```

Replace `Qwen3.8-27B-UD-Q4_K_XL.gguf` with the model you want to use. Check Hugging Face for the system requirements for each model.

## Download other models

Use the same script as for Qwen3.8. The application requires GGUF format, so you might need to convert the model with:

```bash
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(repo_id="microsoft/Phi-3-mini-128k-instruct", local_dir="Phi-3-mini-128k-instruct")
EOF

python llama.cpp/convert_hf_to_gguf.py ./Phi-3-mini-128k-instruct --outfile Phi-3-mini-128k-instruct.gguf --outtype q8_0
```

## Building Devcontainer

You can build a devcontainer where the local AI client is available. Both the client and the fat_client will be part of the docker image:

```bash
docker build . -t local_ai:latest
```

## Before Running

If you want to have support for searching the internet for information, then start the attached docker-compose.yml file
to start SearXNG. Makes the AI's web-integration much more powerful:

```bash
docker compose up -d
```

# Autonomous AI Agent & Architecture

The application is modularly split into standalone components:

1. **`server`** — Dedicated LLM inference server listening on TCP (default port `8080`). Loads GGUF models via `llama.cpp` and serves token generation and chat completion requests over TCP.
2. **`client`** — Lightweight agent client that connects to the `server` over TCP. It manages the interactive agent CLI, runs the ReAct reasoning loop, and executes tools locally on the client host.
3. **`fat_client`** — Standalone all-in-one agent binary embedding both the local `llama.cpp` inference engine and tool execution in a single process.
4. **`web`** (Standalone Web UI) — Standalone web interface and bridge that connects web browsers to the `server` over TCP, offering real-time SSE streaming, live context meter visualization, chat management, and temperature/system prompt configuration.

### Running the Server and Client (Client-Server Mode)

Start the TCP server:
```bash
./build/server -m <path-to-model.gguf> -c 32768 -ngl 99 --host 0.0.0.0 -p 8080
```

Start the client (connecting to the server over TCP, optionally enabling sub-agents and auto-approval):
```bash
./build/client --host 127.0.0.1 -p 8080 --sub-agents
```

#### Running Single Commands / Prompts directly (stdout output):

You can execute a command or prompt directly from the terminal and have the response printed to stdout without entering interactive mode:

```bash
# Run a single prompt and print the response to stdout
./build/client -c "Summarize the contents of README.md"

# Run with --command or --exec flag
./build/client --command "List files in the working directory and explain their purpose" -y

# Quiet mode (-q / --quiet / --silent): suppresses reasoning steps, tool statuses, and prints only the final answer
./build/client -q -c "Fetch and summarize https://example.com" --allow-tool web_fetch

# Allow specific tools automatically via --allow-tool or --allow-tools (comma-separated list)
./build/client -q -c "What is on https://example.com?" --allow-tools web_fetch,read_file

# Execute a direct shell command via /exec and print the output to stdout
./build/client -c "/exec ls -la src/"
```

### Running the Web Interface

Start the standalone web interface (default port `3000`, connects to AI server on `127.0.0.1:8080`):
```bash
python3 web/server.py --port 3000 --server-host 127.0.0.1 --server-port 8080
```
Then open your browser at `http://localhost:3000`.

Features:
- Chat interface with Markdown rendering and syntax-highlighted code blocks (with copy buttons).
- Real-time token streaming via Server-Sent Events (SSE).
- Real-time visualization of server context usage.
- Server status, context resetting (`/reset`), custom system prompt, and temperature adjustments.
- Configuration modal to dynamically change connected server address/port.

### Running the Fat Client (Standalone Mode)

```bash
./build/fat_client -m <path-to-model.gguf> -c 4096 -ngl 99 --sub-agents
```

### Custom Project Instructions (`AI_INSTRUCTIONS.md`)

If a file named `AI_INSTRUCTIONS.md` (or alternatives such as `copilot_instructions.md` / `.github/copilot-instructions.md`) is found in the project root/working directory, it is automatically loaded and appended to the agent's system prompt under `## Project Instructions (AI_INSTRUCTIONS.md):`.
This allows project-specific rules, coding conventions, and architectural guidelines to be consistently followed by the agent.

### Tool Approval & Security Control

By default, the user must approve each tool execution before commands or file modifications are performed.
When the agent requests to invoke a tool, the user is prompted to choose:
- **`yes`** (`y`, `ja`, `j`) — Approve and execute the requested tool this time.
- **`no`** (`n`, `nej`) — Deny the tool execution (the agent is informed of the rejection).
- **`always`** (`a`, `alltid`, `always yes`) — Auto-approve this and all subsequent tool executions during the session.

You can also start the client in auto-approval mode:
- CLI flags: `-y`, `--yes`, `--auto-approve` (or `--require-approval` to enforce prompts).
- Pre-approve specific tools without confirmation: `--allow-tool <tool>` or `--allow-tools <tool1,tool2>` (e.g. `--allow-tool web_fetch`).
- Slash command: `/approval` to toggle between confirmation mode and automatic approval during an interactive session.

### Available Agent Tools

When solving tasks, the agent iteratively reasons, invokes tools via `<tool_call>` blocks, receives observations via `<tool_response>`, and continues reasoning until the task is complete.

```xml
<tool_call>
{
  "name": "read_file",
  "arguments": {"path": "src/client.cpp", "offset": 1, "limit": 50}
}
</tool_call>
```

- **`execute_command`** — Executes bash/shell commands on the local system, returning stdout/stderr and exit codes.
- **`read_file`** — Reads local file contents with line numbering, offset, and limit support.
- **`write_file`** — Writes/creates files (with automatic directory creation).
- **`list_directory`** — Lists files and directories with sizes and file types.
- **`file_search`** — Recursively searches for files/directories matching patterns.
- **`search_text`** — Recursively searches for text or regular expressions across project files.
- **`web_fetch`** — Downloads and parses readable text from HTTP(S) URLs via `libcurl`.
- **`web_search`** — Searches the web via local SearXNG instance.
- **`sub_agent`** — Delegates a sub-task or complex task to an isolated sub-agent. When sub-agents are enabled, the agent breaks down complex problems into modular tasks during the planning phase (Thought/Plan) and runs each task sequentially via sub-agents. The sub-agent runs with its own context and tools, keeping the main conversation context compact and avoiding context pollution, returning only its final result. Each sub-agent's response can also directly trigger follow-up tool executions (such as file operations, commands, or further sub-agent tasks). (Toggleable via `--sub-agents` / `/subagents`).

### Interactive Slash Commands

- `/help` — Display list of commands.
- `/exec <cmd>`, `/sh <cmd>` — Run a shell command directly and print output to stdout.
- `/tools` — Display all registered tools and their argument schemas.
- `/subagents` — Toggle sub-agent tool delegation (enable/disable) dynamically.
- `/approval` — Toggle tool approval mode (Require approval / Auto-approve).
- `/context` — Display current context usage and memory statistics.
- `/compact` — Manually compact conversation context history.
- `/clear`, `/reset` — Clear conversation history and reset context memory.
- `/system` — View the active agent system prompt.
- `/exit`, `/quit` — Exit the program.