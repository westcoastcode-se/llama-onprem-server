FROM ubuntu:26.10

ENV DEBIAN_FRONTEND=noninteractive

ARG CODEX_VERSION=0.160.0

# Development Tools
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates git xz-utils zstd cmake build-essential gcc-16 g++-16 clang libcurl4-openssl-dev \
        python3 python3-pip python3-venv python3-dev nodejs npm \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 10 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 10 \
    && npm install -g "@openai/codex@${CODEX_VERSION}" \
    && codex --version

# Add agents skills folder
RUN mkdir -p /home/ubuntu/.agents/skills
ADD .agents/skills /home/ubuntu/.agents/skills
RUN chown -R ubuntu:ubuntu /home/ubuntu/.agents

# Add codex config file for accessing callisto
RUN mkdir -p /home/ubuntu/.codex /workspace \
    && chown -R ubuntu:ubuntu /home/ubuntu /workspace
ADD docker/codex.toml /home/ubuntu/.codex/config.toml

USER 1000
WORKDIR /workspace

# Plain `codex` opens the TUI. Override the command for `codex exec` / `codex login`.
ENTRYPOINT ["codex"]
CMD []
