<div align="center">
  <img src="res/logo.png" alt="Logo" width="640">

  <p align="center">
    <a href="https://twilitrealm.dev">Official Website</a>
    •
    <a href="https://discord.gg/6NpMhefCK9">Discord</a>
  </p>
</div>

# Overview

Dusklight is a reverse-engineered reimplementation of Twilight Princess.

It aims to be as accurate as possible to the original while also providing new options, enhancements, and tools to customize your experience.

> [!NOTE]
> This fork includes a native Apple Vision Pro port for visionOS 26.5. It renders the game as a head-tracked stereoscopic diorama using Compositor Services, Metal, and independent left/right game-engine scene draws.

# Setup

> [!IMPORTANT]
> Dusklight does *not* provide any copyrighted assets. You must provide your own copy of the original game.

> [!IMPORTANT]
> At a minimum, Dusklight requires a GPU with support for either D3D12, Vulkan, or Metal. Your experience with specific hardware, operating systems, and drivers may vary. In particular, older Intel iGPUs have a high likelihood of incompatibility. We are also aware of a number of issues on devices with Adreno GPUs and are working to resolve them.

### 1. Dump your game

You must dump your own copy of the game, please see [this article](https://wiki.dolphin-emu.org/index.php?title=Ripping_Games) for instructions. After dumping, you can use a program like [Dolphin](https://dolphin-emu.org/) or [nodtool](https://github.com/encounter/nod/releases) to convert the `.iso` to a `.rvz` to save space.

Currently, only the GameCube USA and EUR releases are supported. Support for other versions of the game is planned in the future.

### 2. Download [Dusklight](https://github.com/TwilitRealm/dusklight/releases)

### 3. Setup the game
**Windows / macOS / Linux**
- Extract the .zip file
- Launch Dusklight
- Press **Select Disc Image** and provide the path to your supported game dump
- Press **Play**!

**iOS**
- Follow the [iOS setup guide](docs/ios-install-altstore.md)

**Android**
- Install the Dusklight APK
- Launch Dusklight
- Press **Select Disc Image** and provide the path to your supported game dump
- Press **Play**!

**Apple Vision Pro (visionOS)**
- The visionOS port is built from source. Follow the instructions below, then launch Dusklight from the headset's Home View.

# Building

If you'd like to build Dusklight from source, please read the [build instructions](docs/building.md).

## Apple Vision Pro

The visionOS build requires visionOS 26.5 and an Apple Silicon Mac with:

- Xcode with the visionOS 26.5 SDK and command-line tools selected
- CMake and Ninja
- Rust nightly with the visionOS target: `rustup target add --toolchain nightly aarch64-apple-visionos`
- An Apple Development certificate and a development provisioning profile that includes the headset and covers `dev.twilitrealm.dusk`
- A legally dumped USA or EUR GameCube disc image (`.iso` or `.rvz`)

Clone the repository and its submodules, then configure and build the device target:

```sh
git clone --recursive https://github.com/iRonJ/dusklight_visionos.git
cd dusklight_visionos

cmake --preset visionos-default \
  -DCMAKE_OBJC_FLAGS="-Wno-error=return-mismatch" \
  -DCMAKE_OBJCXX_FLAGS="-Wno-error=return-mismatch"
cmake --build build/visionos-default --target dusklight
```

To sign and install, find the headset identifier with `xcrun devicectl list devices`, then provide your development-signing values to the deployment script:

```sh
export VISIONOS_DEVICE_ID="<paired-headset-identifier>"
export VISIONOS_SIGNING_IDENTITY="Apple Development: Your Name (XXXXXXXXXX)"
export VISIONOS_TEAM_ID="<10-character-team-id>"
export VISIONOS_PROVISIONING_PROFILE="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles/<profile>.mobileprovision"

./scripts/deploy_visionos.sh
```

The script rebuilds, embeds the provisioning profile, signs `build/visionos-default/Dusklight.app`, and installs it with `devicectl`. Launch the installed app from the headset's Home View; launching it through `devicectl` is not reliable for an immersive app.

See [visionOS stereoscopic rendering](docs/visionos-rendering.md) for the renderer architecture,
per-eye GX capture path, IOSurface/Metal handoff, key files, and known effect and lifecycle caveats.

If the document picker cannot access the disc image directly, copy it into the app sandbox:

```sh
xcrun devicectl device copy to \
  --device "$VISIONOS_DEVICE_ID" \
  --domain-type appDataContainer \
  --domain-identifier dev.twilitrealm.dusk \
  --source "/path/to/game.rvz" \
  --destination "Documents/game.rvz"
```

Use the companion window to select the diorama's aspect ratio, change its width and distance, adjust the game world's scene-plane depth, or recenter it in front of you. Direct pinch manipulation of the diorama is not currently supported. Dusklight pauses rendering when it becomes inactive; use **Resume** in the companion window to reopen the immersive space without restarting the game session.

Pull requests are welcomed! Note that we do not accept contributions that are primarily AI-generated and will close your PR if we suspect as much. Please also see the [code conventions](docs/code-conventions.md).

# Credits

Special thanks to the [TP decompilation](https://github.com/zeldaret/tp) team, the GC/Wii decompilation community, the [Aurora](https://github.com/encounter/aurora) developers, the [TP speedrunning community](https://zsrtp.link), and all [contributors](https://github.com/TwilitRealm/dusklight/graphs/contributors).

<br/>
<div align="center">
    <a href="https://github.com/encounter/aurora">
        <img src="assets/aurora-powered.png" alt="Powered by Aurora" width="800">
    </a>
</div>
