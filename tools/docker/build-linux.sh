#!/usr/bin/env bash
# Build G-Diffuser for Linux inside the pinned environment from tools/docker/linux.Dockerfile.
# The target architecture is the Docker host's (arm64 on Apple Silicon, x86-64 elsewhere).
#
#   tools/docker/build-linux.sh [build-dir] [extra cmake args...]
#
# Extra arguments go to the CMake configure step, e.g. -DGDX_OPENXR=ON.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${1:-build/linux-$(docker info --format '{{.Architecture}}')}"
shift || true

docker run --rm -v "$repo":/src -w /src gdx-linux bash -c "
    set -euo pipefail
    cmake -S . -B '$build_dir' -GNinja -DCMAKE_BUILD_TYPE=Release $*
    cmake --build '$build_dir' --target G-Diffuser
"
