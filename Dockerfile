FROM ubuntu:26.10

ENV DEBIAN_FRONTEND=noninteractive

# Base tools
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates git xz-utils zstd cmake build-essential gcc-16 g++-16 clang libcurl4-openssl-dev \
        python3 python3-pip python3-venv python3-dev \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 10 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 10

USER 1000

ADD cmake-build-debug/client /client
ADD cmake-build-debug/fat_client /fat_client

WORKDIR /workspaces
