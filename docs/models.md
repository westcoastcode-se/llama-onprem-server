# Models

The GGUF file name selects how tool calls are parsed. `--chat-template` wins when that file name matches a family. A name containing `deepseek`, `devstral`, `bonsai`, `ternary`, or `qwen` selects that family. Any other name uses the Qwen format.

## Qwen 3.8 27B

[Qwen](https://qwen.ai) 3.8 27B is slower than Devstral and usually answers with higher quality.

Download a GGUF from [Unsloth on Hugging Face](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF):

```bash
pip install huggingface_hub
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(
    repo_id="unsloth/Qwen3.8-27B-GGUF",
    allow_patterns=["*UD-Q4_K_XL.gguf"],
    local_dir="Qwen3.8-27B-GGUF",
)
EOF
```

Change `allow_patterns` and `local_dir` for another quantization or directory. A more compressed file leaves more room for context. Examples:

| Graphics card | Quantization | Context |
|---|---|---|
| NVIDIA RTX 3080 Ti 16GB | UD-Q2_K_XL | 60000 |
| NVIDIA RTX 4090 24GB | UD-Q4_K_XL | 100000 |

Start this model with `-t 1.0`. Tool calls use `<tool_call><function=name><parameter=arg>value</parameter></function></tool_call>`.

## Devstral

[Devstral](https://mistral.ai/news/devstral/) is a Mistral coding model. Quality is decent. It is very fast, including on smaller hardware.

A GGUF whose file name contains `devstral` is the Devstral family. The same match applies to a `--chat-template` file name. Tool calls are `[TOOL_CALLS]name[ARGS]{"arg":"value"}`. Devstral does not open a `<think>` block, so the reply is the answer even when `--reasoning` is on.

Download a GGUF from [Unsloth on Hugging Face](https://huggingface.co/unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF):

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
```

Change `allow_patterns` and `local_dir` for another quantization or directory. A more compressed file leaves more room for context. The smallest context that still works well is 16384. Examples:

| Graphics card | Quantization | Context |
|---|---|---|
| NVIDIA RTX 3080 Ti 16GB | UD-Q2_K_XL | 20000 |
| NVIDIA RTX 4090 24GB | UD-Q4_K_XL | 40000 |

Start this model with `-t 0.15 --min-p 0.01`.

## Others

A file name that contains `deepseek` parses tool calls between `<｜tool▁call▁begin｜>` and `<｜tool▁call▁end｜>`. The function name follows `<｜tool▁sep｜>`, and the arguments are a JSON object.

A file name that contains `bonsai` or `ternary` uses the same `<tool_call>` format as Qwen.
