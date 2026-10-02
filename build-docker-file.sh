#!/bin/bash

export DOCKER_BUILDKIT=1

docker buildx build . -f Development.dockerfile -t callisto-base:latest
docker buildx build . -f Dockerfile -t callisto:latest