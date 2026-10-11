# PR0 source build readiness

**EXPERIMENTAL — source release.** A fresh anonymous checkout, ARM64 OpenXR build, all 293 VR checks, ABI checks and ROM-free package assembly passed. The README describes the current features and controls.

## Tested source and dependencies

The fresh validation checkout used superproject revision `e6cc30b2c83b8fd8b590f30ee891c66f65ec52f2` with the PR0 `.gitmodules` URLs. The dependency gitlinks were unchanged:

| Path | Pinned commit |
| --- | --- |
| `decomp` | `3b307ffe4fe8b15ab5abff56e5eb6da9f61995cc` |
| `fzerox-expansion-kit` | `6cd71e6fff1714fbfbda22d61dc8bce190d9632d` |
| `libultraship` | `c8348cc8294969a6df4192b1747d2f2e08ce7628` |
| `torch` | `8bdae395876b31f70c599d607c2babcb941abbd3` |

Recursive initialization succeeded with an empty `HOME`, system and global Git configuration disabled, and terminal prompting disabled. All nested decompilation and Expansion Kit tooling submodules also resolved to their pinned commits. The two project-owned dependency forks used by this branch are public:
[fzerox-frame-source](https://github.com/astromaddie/fzerox-frame-source) and [libultraship-fzerox-frame](https://github.com/astromaddie/libultraship-fzerox-frame).

## Verification

The ARM64 build used the existing `gdx-linux` image (`sha256:ec30b042be5580daf5d886ca5234ccd18d8474287032ea72d837ed48c0209472`, `arm64`) and four build jobs. CMake configured with `-DGDX_OPENXR=ON`; the full `G-Diffuser` target compiled and linked successfully without ROM, disk, or IPL inputs. `tools/docker/build-linux.sh` now forwards `CMAKE_BUILD_PARALLEL_LEVEL` into the container so the documented job cap controls the actual Ninja build.

The following production test targets passed:

| Target | Result |
| --- | ---: |
| `gdx_vr_scheduler_tests` | 51 checks, 0 failed |
| `gdx_vr_math_tests` | 74 checks, 0 failed |
| `gdx_vr_system_tests` | 134 checks, 0 failed |
| `gdx_vr_settings_tests` | 34 checks, 0 failed |

The Linux ABI checker passed for `G-Diffuser`, `gdx-extract`, and the bundled SDL2 and zlib libraries. The ROM-free package contains the executable, extractor, recipes, fonts, shaders, licenses, and required runtime libraries. No ROM, disk image, or IPL file was present in the archive.

Package: `G-Diffuser-vpr0-linux-arm64.tar.gz`

SHA-256: `ab5cbd818cd1dd0e30a244143d6af538e490451f9e27d12691fbcb9463e31b70`

The metadata below records the source, toolchain and artifact identity. The package was built for validation; no binary release has been uploaded.
