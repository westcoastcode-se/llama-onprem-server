# Models

## Qwen 3.8 27B

[Qwen](https://qwen.ai) 3.8 27B is a large language model developed by Qwen. It is a Chinese language model that has been trained on a large corpus of text data. 
It is designed to generate human-like text and can be used for a variety of natural language processing tasks, such as text generation, 
text classification, and text summarization.

It's a bit slower than Devstral but has better quality.

The easiest way is to download a LLama.cpp compatible version of Qwen from [huggingface](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF).

You can also download it using llama.cpp's built-in python script using:

```bash
# Enter the llama.cpp directory
cd vendors/llama.cpp
# Enable python virtual environment
python -m venv venv
# Activate the virtual environment
source venv/bin/activate
# Install the necessary dependencies
pip install
# Run Llama.cpp huggingface download library
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(
    repo_id="unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF",
    allow_patterns=["*UD-Q4_K_XL.gguf"],
    local_dir="Devstral-Small-2-24B-Instruct-2512-GGUF",
)
EOF
```

Replace **Q4_K_XL.gguf** and **local_dir** with whatever version of Qwen that's compatible with your hardware and where to download it.

For example:

| Graphics Card                | Most Compatible Version | Context Size |
|------------------------------|-------------------------|--------------|
| NVIDIA RTX 3080 Ti 16GB VRAM | *UD-Q2_K_XL.gguf        | 60_000       |
| NVIDIA RTX 4090 24GB VRAM    | *UD-Q4_K_XL.gguf        | 100_000      |

The more compressed model you use the larger context size it can handle.

Good arguments to the server are:
* **temperature** = 1.0

## Devstral

[Devstral](https://mistral.ai/news/devstral/) is a model based on Mistral but for coding. Quality is decent but not the best. What makes this model great
is the speed. It's very, very fast, even on smaller hardware.

A GGUF whose file name contains `devstral` is the Devstral family. The same match applies to a `--chat-template` file name.
Tool calls are `[TOOL_CALLS]name[ARGS]{"arg":"value"}`. Devstral does not open a `<think>` block, so the reply is the answer even when `--reasoning` is on.

The easiest way is to download a LLama.cpp compatible version of Devstral from [huggingface](https://huggingface.co/unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF).

You can also download it using llama.cpp's built-in python script using:

```bash
# Enter the llama.cpp directory
cd vendors/llama.cpp
# Enable python virtual environment
python -m venv venv
# Activate the virtual environment
source venv/bin/activate
# Install the necessary dependencies
pip install
# Run Llama.cpp huggingface download library
python <<EOF
from huggingface_hub import snapshot_download
snapshot_download(
    repo_id="unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF",
    allow_patterns=["*Q5_K_XL.gguf"],
    local_dir="Devstral-Small-2-24B-Instruct-2512-GGUF",
)
EOF
```

Replace **Q5_K_XL.gguf** and **local_dir** with whatever version of Devstral that's compatible with your hardware and where to download it.

For example:

| Graphics Card                | Most Compatible Version | Context Size |
|------------------------------|-------------------------|--------------|
| NVIDIA RTX 3080 Ti 16GB VRAM | *UD-Q2_K_XL.gguf        | 20_000       |
| NVIDIA RTX 4090 24GB VRAM    | *UD-Q4_K_XL.gguf        | 40_000       |

The more compressed model you use the larger context size it can handle. The minimum usable context size is 16_384.

Good arguments to the server are:
* **temperature** = 0.15
* **min_p** = 0.01

## Others

There's preliminary support for DeepSeek although not fully tested