FROM callisto-base:latest

USER 1000

ADD cmake-build-release/callisto /callisto
ADD LICENSE /LICENSE
ADD THIRD_PARTY_NOTICES.md /THIRD_PARTY_NOTICES.md

WORKDIR /workspaces
