# Build

Run the commands from the repository root. `vendors/llama.cpp` and `vendors/ftxui` are git submodules. Check them out before configuring:

```bash
git submodule update --init
```

## Dependencies

You need CMake 4.1 or newer, a C++23 compiler, libcurl, and git. Ninja is optional. CUDA is needed only when the model should run on a GPU.

Arch:

```bash
sudo pacman -S --needed base-devel git cmake ninja curl
```

Debian or Ubuntu: `build-essential`, `cmake`, `ninja-build`, `libcurl4-openssl-dev`, and `git`. Add the CUDA toolkit when you want to offload layers to the GPU.

## Debug

The Debug build is the one to use while changing the code.

```bash
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug -j$(nproc)
```

That produces:

- `cmake-build-debug/callisto_server`
- `cmake-build-debug/callisto_cli`
- `cmake-build-debug/tests`

## Release

Release is the build to run a model with.

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target callisto_server callisto_cli -j$(nproc)
```

## GPU

Turn CUDA on and set the architecture. `nvidia-smi --query-gpu=name,compute_cap --format=csv` prints the number. An RTX 40-series card is `89` (8.9).

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build cmake-build-release --target callisto_server callisto_cli -j$(nproc)
```

## Tests

The tests need no model weights.

```bash
cmake --build cmake-build-debug --target tests -j$(nproc)
ctest --test-dir cmake-build-debug --output-on-failure
```

`ctest` runs one entry, the `tests` binary.

## Docker

The `Dockerfile` packages a client that is already built. Build Release first. The image contains `callisto_cli`, the license, and `THIRD_PARTY_NOTICES.md`. It has no CUDA and no server.

```bash
cmake --build cmake-build-release --target callisto_cli -j$(nproc)
docker build . -t local_ai:latest
```

`docker compose up -d` starts SearXNG and Valkey. That is the search engine behind the `web_search` tool, on port 4488. Those services are not part of the Callisto binaries.
