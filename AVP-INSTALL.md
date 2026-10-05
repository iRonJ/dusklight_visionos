# Installing Dusklight visionOS on Apple Vision Pro

This fork of [Dusklight](https://github.com/TwilitRealm/dusklight), the reverse-engineered reimplementation of Twilight Princess, adds a native Apple Vision Pro port for visionOS 26.5. It renders the game as a head-tracked stereoscopic diorama: two independent scene draws, one per eye, shown on a world-anchored screen in an immersive space, through Compositor Services and Metal.

## What you need

- Apple Vision Pro on visionOS 26.5
- An Apple Silicon Mac with:
  - Xcode with the visionOS 26.5 SDK, and its command-line tools selected
  - CMake and Ninja
  - Rust nightly with the visionOS target: `rustup target add --toolchain nightly aarch64-apple-visionos`
- An Apple Development certificate, and a development provisioning profile that includes your headset and covers `dev.twilitrealm.dusk`
- Your own dump of Twilight Princess for GameCube, USA or EUR release (`.iso` or `.rvz`)

## Your game files

Dusklight does not provide any copyrighted assets. You supply your own copy of the original game.

1. Dump your own disc. The Dolphin wiki's [Ripping Games](https://wiki.dolphin-emu.org/index.php?title=Ripping_Games) article explains how. To save space, you can convert the `.iso` to `.rvz` with [Dolphin](https://dolphin-emu.org/) or [nodtool](https://github.com/encounter/nod/releases). Only the GameCube USA and EUR releases are supported.
2. On the headset, open Dusklight, choose **Choose Disc Image (ISO / GCM / RVZ)…** and pick your dump. The app copies it into its own storage.
3. Press **Play in 3D Diorama**.

If the document picker can't access the disc image directly, copy it into the app's sandbox from your Mac (`VISIONOS_DEVICE_ID` is set in the install step below):

```sh
xcrun devicectl device copy to \
  --device "$VISIONOS_DEVICE_ID" \
  --domain-type appDataContainer \
  --domain-identifier dev.twilitrealm.dusk \
  --source "/path/to/game.rvz" \
  --destination "Documents/game.rvz"
```

## Build from source

There's no prebuilt visionOS app. Start from a checkout of this repository with its submodules (the README clones it with `git clone --recursive`), then configure and build the device target:

```sh
cmake --preset visionos-default \
  -DCMAKE_OBJC_FLAGS="-Wno-error=return-mismatch" \
  -DCMAKE_OBJCXX_FLAGS="-Wno-error=return-mismatch"
cmake --build build/visionos-default --target dusklight
```

## Install on Apple Vision Pro

Find your headset's identifier with `xcrun devicectl list devices`, then give the deployment script your own signing values. Set all four: the script falls back to built-in defaults for any that are missing.

```sh
export VISIONOS_DEVICE_ID="<paired-headset-identifier>"
export VISIONOS_SIGNING_IDENTITY="Apple Development: Your Name (XXXXXXXXXX)"
export VISIONOS_TEAM_ID="<10-character-team-id>"
export VISIONOS_PROVISIONING_PROFILE="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles/<profile>.mobileprovision"

./scripts/deploy_visionos.sh
```

The script rebuilds, embeds the provisioning profile, signs `build/visionos-default/Dusklight.app` and installs it with `devicectl`. Launch Dusklight from the headset's Home View; launching it through `devicectl` isn't reliable for an immersive app.

## Notes

- The companion window sets the diorama's aspect ratio, width and distance, and the game world's scene-plane depth, and recenters the diorama in front of you. Direct pinch manipulation of the diorama isn't supported yet.
- Dusklight pauses rendering when it becomes inactive. Use **Resume** in the companion window to reopen the immersive space without restarting the game session.
- [visionOS stereoscopic rendering](docs/visionos-rendering.md) covers the renderer, and its known effect and lifecycle caveats.
