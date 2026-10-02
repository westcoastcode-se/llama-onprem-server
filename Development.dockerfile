FROM ubuntu:26.10

ENV DEBIAN_FRONTEND=noninteractive

# Development Tools
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates git xz-utils zstd cmake build-essential gcc-16 g++-16 clang libcurl4-openssl-dev \
        python3 python3-pip python3-venv python3-dev \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 10 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 10

# Add agents skills folder
RUN mkdir -p /home/ubuntu/.agents/skills
ADD .agents/skills /home/ubuntu/.agents/skills
RUN chown -R ubuntu:ubuntu /home/ubuntu/.agents

USER 1000
