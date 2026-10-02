FROM callisto-base:latest

USER 1000

ADD cmake-build-release/callisto_cli /callisto_cli
ADD LICENSE /LICENSE
ADD THIRD_PARTY_NOTICES.md /THIRD_PARTY_NOTICES.md

WORKDIR /workspaces
