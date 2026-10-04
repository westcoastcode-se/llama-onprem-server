#!/bin/bash

export DOCKER_BUILDKIT=1
echo "Building docker image for using codex with callisto..."
docker buildx build . -f Codex.dockerfile -t callisto-codex:latest
echo "Run docker run -it --rm --env CALLISTO_API_KEY=<API_KEY> --net=host -v \$(pwd):/workspaces  callisto-codex:latest codex"