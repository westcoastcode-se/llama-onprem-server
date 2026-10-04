# Codex CLI in a container.
# Pin the CLI so rebuilds stay reproducible. Bump CODEX_VERSION when you want an upgrade.
FROM node:26.10

ARG CODEX_VERSION=0.160.0
ARG UID=1000
ARG GID=1000

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git \
        ca-certificates \
        curl \
        ripgrep \
        openssh-client \
        python3 \
        build-essential \
        xz-utils cmake build-essential gcc-16 g++-16 clang python3 python3-pip python3-venv python3-dev \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 10 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 10 \
    && npm install -g "@openai/codex@${CODEX_VERSION}" \
    && codex --version

# Stock "node" user is uid 1000. Remap if the host user differs so bind mounts stay writable.
RUN if [ "${GID}" != "1000" ]; then groupmod -g "${GID}" node; fi \
    && if [ "${UID}" != "1000" ]; then usermod -u "${UID}" -g "${GID}" node; fi \
    && mkdir -p /home/node/.codex /workspace \
    && chown -R node:node /home/node /workspace

# Add agents skills folder
RUN mkdir -p /home/node/.agents/skills
ADD .agents/skills /home/node/.agents/skills
RUN chown -R node:node /home/node/.agents

# Add codex config file for accessing callisto
ADD docker/codex.toml /home/node/.codex/config.toml

USER node
ENV CODEX_HOME=/home/node/.codex
ENV HOME=/home/node
WORKDIR /workspace

# Plain `codex` opens the TUI. Override the command for `codex exec` / `codex login`.
ENTRYPOINT ["codex"]
CMD []
