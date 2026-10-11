# Local mirror of the pinned build environment in .github/workflows/release-linux.yml. Select the
# Docker target platform explicitly when it differs from the Docker engine's platform. Keep the
# versions below in sync with that workflow.
#
#   docker build -t gdx-linux -f tools/docker/linux.Dockerfile tools/docker
#   tools/docker/build-linux.sh
ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive \
    CC=gcc-12 \
    CXX=g++-12

ARG CMAKE_VERSION=3.31.6
ARG SDL2_VERSION=2.30.9
ARG TINYXML2_VERSION=10.0.0
ARG SPDLOG_VERSION=1.14.1
ARG LIBZIP_VERSION=1.10.1

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-12 g++-12 ninja-build pkg-config \
        git ca-certificates wget \
        python3 python3-yaml python3-pil \
        zlib1g-dev libbz2-dev nlohmann-json3-dev libpng-dev \
        libgl1-mesa-dev libopengl-dev libegl1-mesa-dev libglew-dev \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
        libxinerama-dev libxss-dev libxkbcommon-dev \
        libwayland-dev wayland-protocols libdrm-dev libgbm-dev \
    && rm -rf /var/lib/apt/lists/* \
    && git config --global --add safe.directory '*'

RUN set -eux; \
    arch="$(uname -m)"; \
    wget -q "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"; \
    mkdir -p /opt/cmake; \
    tar -xzf "cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" -C /opt/cmake --strip-components=1; \
    rm "cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"
ENV PATH=/opt/cmake/bin:$PATH

RUN set -eux; \
    cd /tmp; \
    wget -q "https://www.libsdl.org/release/SDL2-${SDL2_VERSION}.tar.gz"; \
    tar -xzf "SDL2-${SDL2_VERSION}.tar.gz"; \
    cmake -S "SDL2-${SDL2_VERSION}" -B build-sdl2 -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF; \
    cmake --build build-sdl2 --target install; \
    wget -q -O tinyxml2.tar.gz "https://github.com/leethomason/tinyxml2/archive/refs/tags/${TINYXML2_VERSION}.tar.gz"; \
    tar -xzf tinyxml2.tar.gz; \
    cmake -S "tinyxml2-${TINYXML2_VERSION}" -B build-tinyxml2 -GNinja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -Dtinyxml2_BUILD_TESTING=OFF; \
    cmake --build build-tinyxml2 --target install; \
    wget -q -O spdlog.tar.gz "https://github.com/gabime/spdlog/archive/refs/tags/v${SPDLOG_VERSION}.tar.gz"; \
    tar -xzf spdlog.tar.gz; \
    cmake -S "spdlog-${SPDLOG_VERSION}" -B build-spdlog -GNinja \
        -DCMAKE_BUILD_TYPE=Release -DSPDLOG_BUILD_SHARED=OFF \
        -DSPDLOG_FMT_EXTERNAL=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF; \
    cmake --build build-spdlog --target install; \
    wget -q -O libzip.tar.gz "https://github.com/nih-at/libzip/archive/refs/tags/v${LIBZIP_VERSION}.tar.gz"; \
    tar -xzf libzip.tar.gz; \
    cmake -S "libzip-${LIBZIP_VERSION}" -B build-libzip -GNinja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DENABLE_OPENSSL=OFF \
        -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF; \
    cmake --build build-libzip --target install; \
    ldconfig; \
    rm -rf /tmp/*

WORKDIR /src
