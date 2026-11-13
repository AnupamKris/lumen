#!/bin/sh
# Build the .deb from any machine with Docker (e.g. a Mac):
#   packaging/docker-build.sh           # amd64 (default)
#   packaging/docker-build.sh arm64
set -e
ARCH=${1:-amd64}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec docker run --rm --platform "linux/$ARCH" -v "$ROOT:/src" -w /src \
  -e OUT=/src/dist debian:trixie bash /src/packaging/build-deb.sh
