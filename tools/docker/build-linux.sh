#!/usr/bin/env bash
# Build G-Diffuser for Linux inside the pinned environment from tools/docker/linux.Dockerfile.
# Set DOCKER_PLATFORM=linux/arm64 to target the Steam Frame from an x86-64 Docker host with ARM64
# emulation enabled.
#
#   tools/docker/build-linux.sh [build-dir] [extra cmake args...]
# Set CMAKE_BUILD_PARALLEL_LEVEL to cap the build jobs passed to CMake.
#
# Extra arguments go to the CMake configure step, e.g. -DGDX_OPENXR=ON.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
docker_platform="${DOCKER_PLATFORM:-}"
target_arch="$(docker info --format '{{.Architecture}}')"
if [[ "$docker_platform" == linux/arm64 ]]; then target_arch=aarch64; fi
build_dir="${1:-build/linux-$target_arch}"
shift || true

docker_args=()
[[ -z "$docker_platform" ]] || docker_args+=(--platform "$docker_platform")
if [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
    docker_args+=(--env "CMAKE_BUILD_PARALLEL_LEVEL=$CMAKE_BUILD_PARALLEL_LEVEL")
fi

docker run --rm ${docker_args[@]+"${docker_args[@]}"} -v "$repo":/src -w /src gdx-linux bash -c "
    set -euo pipefail
    cmake -S . -B '$build_dir' -GNinja -DCMAKE_BUILD_TYPE=Release $*
    cmake --build '$build_dir' --target G-Diffuser
"
