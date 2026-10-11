# F-Zero X VR on Steam Frame

This branch adds a native VR mode to [G-Diffuser](https://github.com/Zorkats/G-Diffuser), the F-Zero X PC port by Zorkats and its contributors. It runs on Steam Frame as a Linux ARM64 app with OpenXR and OpenGL through Zink. The VR path is optional; the flat-screen game remains available.

**EXPERIMENTAL.** I made this for personal use, and it's still a work in progress. Bring your own game data.

Race in stereo from a chase view or from inside your machine. Cockpit options include a cabin or roof seat, dashboard readouts, and a rear-view mirror. The flat menus stay on a panel in front of you, with controller navigation and a laser pointer for selection.

## Frame controls

Right B switches between cockpit and chase views. Hold **Menu + View** for half a second to open the VR settings menu. The left stick and D-pad navigate menus; use the right-hand laser and trigger to point and click. Hold **View** for one second to recenter direction and position while keeping the height anchor; hold it for three seconds to recenter and set height again.

The VR menu includes world scale, camera, HUD, haptics and display settings. The default resolution scale is **1.25×** the runtime's recommended eye size; with the Frame's 1728×1728 recommendation this produces 2160×2160 eye images. The app requests 120 Hz when available, while SteamVR's per-app setting determines the actual display rate. Set the title's display rate in SteamVR's per-app settings.

## Game data

Playing the documented Frame build needs your own US revision 0 F-Zero X ROM, the translated 64DD Expansion Kit disk image, and a supported 64DD IPL ROM. The default build refuses Japanese-region game data. The game extracts its runtime data locally. This repository does not include Nintendo ROMs, disk images, IPL files or generated game assets. See the [saved G-Diffuser README](docs/README_UPSTREAM.md) for the original project's data policy and general features.

## Build and install

The [Steam Frame build and install guide](docs/STEAM-FRAME-BUILD-INSTALL.md) covers ARM64 Docker builds from Linux, macOS, or Windows through WSL2, the ROM/disk/firmware inputs, package output, first installation and Steam launch. It distinguishes the standalone Linux ARM64 Frame package from desktop and Windows builds.

The application builds without game data. A fresh ARM64 OpenXR build, all 293 VR checks, ABI checks and ROM-free package assembly passed. See [PR0 build details](docs/PR0_READINESS.md) and the [changelog](CHANGELOG.md).

See [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md) for the current VR implementation and controls. Sections 1–3 retain earlier M0 design and device notes; section 4 and the current-behavior sections near the end describe the implemented path.

G-Diffuser is maintained by Zorkats and contributors and builds on [inspectredc/fzerox](https://github.com/inspectredc/fzerox) and [Kenix3/libultraship](https://github.com/Kenix3/libultraship). The original README, including general platform/build information and the game's license policy, is preserved at [docs/README_UPSTREAM.md](docs/README_UPSTREAM.md).

## Credits and licenses

The [MIT license](LICENSE) covers original G-Diffuser code and documentation. Components retain their own terms in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). These licenses don't grant rights to Nintendo's game, ROM, disk, IPL or generated assets.
