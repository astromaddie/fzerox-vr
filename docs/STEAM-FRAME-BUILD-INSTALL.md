# Steam Frame build and install

This guide builds the standalone Steam Frame target: a Linux ARM64 package. The build host can be Linux, macOS, or Windows running a Linux shell under WSL2; it does not need to be ARM64. A Windows or desktop VR executable is a different target and is not the Frame package.

## Build host

The build host needs Bash, Git, and Docker with Buildx and ARM64 emulation on x86 hosts. The build image supplies its compiler and Python. On Windows, use Docker Desktop with WSL2 integration, clone and build inside the Linux distribution's filesystem, and run these commands from Bash. See Microsoft's [install WSL](https://learn.microsoft.com/windows/wsl/install) instructions and Docker's [WSL2 integration](https://docs.docker.com/desktop/features/wsl/use-wsl/) and [multi-platform builds with emulation](https://docs.docker.com/build/building/multi-platform/) guides. `docker buildx inspect --bootstrap` should list `linux/arm64` under supported platforms. ARM64 emulation can make compiler and asset-extractor builds slow.

On an ARM64 Linux Docker host, the default Docker platform is already the target. On x86-64 Linux, macOS, or WSL2, set `DOCKER_PLATFORM=linux/arm64`. Buildx runs this Dockerfile and subsequent build container for ARM64; the package produced is not the host's desktop executable. The repository does not set up Docker on SteamOS: use the native Frame Desktop route below when Docker is already available there, otherwise build on an external host and copy the finished package to the Frame.

## Build and package

```sh
git clone --recurse-submodules --branch vr/openxr-m0 https://github.com/astromaddie/fzerox-vr.git
cd fzerox-vr
docker buildx build --platform linux/arm64 --load -t gdx-linux \
  -f tools/docker/linux.Dockerfile tools/docker
DOCKER_PLATFORM=linux/arm64 CMAKE_BUILD_PARALLEL_LEVEL=4 \
  tools/docker/build-linux.sh build/openxr-arm64 -DGDX_OPENXR=ON
docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src gdx-linux \
  tools/package_linux.sh build/openxr-arm64 1.0.0 dist linux-arm64
```

On an ARM64 Linux Docker host, omit `--platform linux/arm64` and the `DOCKER_PLATFORM` assignment. Replace `1.0.0` with the version label you want. The package directory is `dist/G-Diffuser-v1.0.0-linux-arm64/`; the command also writes a tarball. The package contains the native executable, extractor, recipes and required data files, but no game ROM, Expansion Kit disk, IPL image or generated game assets. The package script runs the ABI check before finishing.

If Frame Desktop already has Docker and an ARM64-capable Buildx builder available, a native local build is also supported. From a checkout under your writable home directory, use:

```sh
docker build -t gdx-linux -f tools/docker/linux.Dockerfile tools/docker
CMAKE_BUILD_PARALLEL_LEVEL=4 tools/docker/build-linux.sh build/openxr-arm64 -DGDX_OPENXR=ON
docker run --rm -v "$PWD":/src -w /src gdx-linux \
  tools/package_linux.sh build/openxr-arm64 1.0.0 dist linux-arm64
```

This omits the `--platform linux/arm64` options and leaves `DOCKER_PLATFORM` unset because Frame Desktop is the native ARM64 target. The repository does not provision Docker on SteamOS; when no working Docker setup is present, build on an external host and transfer the package.

## First install and game data

For a first install from another computer, follow Valve's [Steam Frame development setup](https://partner.steamgames.com/doc/steamhardware/steamframe/setup) to enable Developer Mode and pair the host. In SteamOS Devkit Client, connect to the Frame by IP, then upload the package directory. Set **Name** to `GDiffuser`, **Local Folder** to that package directory, **Start Command** to `G-Diffuser --vr`, and **Runtime** to **Steam Linux Runtime 3.0 ARM64 (Sniper)**. The [Valve upload guide](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames) describes those fields. Uploading creates the Steam library entry, and this command launches the VR mode from its first Steam start.

On the Frame, start `GDiffuser` from the Steam library. The uploaded Start Command already includes `--vr`, selecting VR mode on the first launch. The game's first-boot setup asks for your own supported files. The regular Frame build accepts the US revision 0 F-Zero X ROM `baserom.us.rev0.z64` (SHA-1 `5f658e88ffa9de23cba6986a8fd3d3a90d7b4340`), translated Expansion Kit disk `baserom.translated.ek.ndd` (SHA-1 `fde9fa6f29a52be0144bda74caf8583c036c20ce`), and a known 64DD IPL image named `N64DDIPLROM.n64`. The recognized IPL images have SHA-1 `bf861922dcb78c316360e3e742f4f70ff63c9bc3` (Japan retail) or `3c5b93ca231550c68693a14f03cea8d5dbd1be9e` (USA prototype). The default build refuses the retail Japanese Expansion Kit disk. The setup validates the chosen files, copies them beside the executable under those names, and extracts local game data. An Expansion Kit cartridge image is not a disk image.

To make the files selectable in the Frame's setup, transfer your own files to the Frame and place them in a folder there. For example, after putting the inputs under `~/incoming/`:

```sh
mkdir -p "$HOME/game-data"
cp "$HOME/incoming/your-us-rev0-image.z64" "$HOME/game-data/baserom.us.rev0.z64"
cp "$HOME/incoming/your-translated-ek-disk.ndd" "$HOME/game-data/baserom.translated.ek.ndd"
cp "$HOME/incoming/your-64dd-ipl-image.n64" "$HOME/game-data/N64DDIPLROM.n64"
```

If the package is already on the Frame, keep the unpacked package in a writable folder under your home directory, add `G-Diffuser` through Steam's **Add a Non-Steam Game** flow, set Launch Options to `--vr`, and start it from the Steam library. The project has a separate flat desktop target; a Windows build or Linux desktop build is not an ARM64 Frame build.
