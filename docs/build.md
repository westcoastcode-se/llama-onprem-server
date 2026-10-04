# Build

Run the commands from the repository root. `vendors/llama.cpp` is a git submodule. Check it out before configuring:

```bash
git submodule update --init
```

## Dependencies

You need CMake 4.1 or newer, a C++23 compiler, libcurl, and git. Ninja is optional. CUDA is needed only when the model should run on a GPU.

Arch:

```bash
sudo pacman -S --needed base-devel git cmake ninja curl cuda
```

Debian or Ubuntu: `build-essential`, `cmake`, `ninja-build`, `libcurl4-openssl-dev`, and `git`. Add the CUDA toolkit when you want to offload layers to the GPU.

## Debug Without CUDA

The Debug build is the one to use while changing the code

```bash
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug -j$(nproc)
```

That produces:

- `cmake-build-debug/callisto`
- `cmake-build-debug/tests`

## Debug With Cuda

Replace `CMAKE_CUDA_ARCHITECTURES=86` with whatever CUDA version you have on your computer. You can figure it out by
running: `nvidia-smi --query-gpu=name,compute_cap --format=csv`. An RTX 40-series card is `89` (will show as 8.9).

```bash
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=86 \
  -DCUDAToolkit_ROOT=/opt/cuda \
  -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc
cmake --build cmake-build-debug -j$(nproc)
```

## Release Without CUDA

Release is the build to run a model with.

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target callisto -j$(nproc)
```

## Release With CUDA

Replace `CMAKE_CUDA_ARCHITECTURES=86` with whatever CUDA version you have on your computer. You can figure it out by
running: `nvidia-smi --query-gpu=name,compute_cap --format=csv`. An RTX 40-series card is `89` (will show as 8.9).

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=86 \
  -DCUDAToolkit_ROOT=/opt/cuda \
  -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc
cmake --build cmake-build-release -j$(nproc)
```

## Tests

The tests need no model weights.

```bash
cmake --build cmake-build-debug --target tests -j$(nproc)
ctest --test-dir cmake-build-debug --output-on-failure
```

`ctest` runs one entry, the `tests` binary.

## Docker

Do you want to run Codex inside a dockerized virtual environment? There are still a lot of security bugs related to
Linux-based containers, but at least you isolate the agent from accidentally erasing your entire computer.

After **building** a release-version of Callisto, you can build a docker image that has codex installed in it by:

```bash
./build-docker-file.sh
```

Script requires docker buildx plugin. Install it with `sudo pacman -S docker-buildx`.
