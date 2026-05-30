# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Dusklight is a reverse-engineered, native reimplementation of *The Legend of Zelda: Twilight Princess* (GameCube). It is built from the [zeldaret/tp](https://github.com/zeldaret/tp) decompilation, ported to run natively on PC, macOS, iOS, Android, and tvOS via the [Aurora](https://github.com/encounter/aurora) GC/Wii runtime. It ships no game assets — users supply their own disc image (passed with `--dvd`).

> The project does **not** accept primarily AI-generated contributions; PRs suspected of being AI-generated are closed. Keep this in mind: assist with understanding and targeted edits, but the final code is expected to be human-authored.

## Build & run

Requires CMake 3.25+ and Python 3. Configure + build with a CMake preset, then run with a disc image:

```sh
# macOS (produces Dusklight.app)
cmake --preset macos-default-relwithdebinfo
cmake --build --preset macos-default-relwithdebinfo
build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight --dvd /path/to/game.iso

# Linux
cmake --preset linux-default-relwithdebinfo
cmake --build --preset linux-default-relwithdebinfo
build/linux-default-relwithdebinfo/dusklight --dvd /path/to/game.iso

# Windows
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
```

Other presets exist per platform (`*-default-debug`, `linux-clang-*`, `windows-clang-*`). Supported disc formats: ISO/GCM, RVZ, WIA, WBFS, CISO, GCZ. Currently only **GameCube USA and EUR** releases are supported. There is no unit test suite; verification is done by running the game.

`git submodule update --init --recursive` is required (Aurora lives in `extern/aurora` as a submodule).

## Source layout: two layers

The codebase has a clear split between original-game code and the Dusk PC layer.

**Decomp layer** — a port of the original game. Uses Zelda/TP decomp naming prefixes:
- `src/d/` — actors and daObjects (the bulk of the game, ~980 files)
- `src/f_op/`, `src/f_pc/` — the fopAc/fpc framework (actor process & manager system)
- `src/m_Do/`, `src/m_Re/` — main "Do" managers (graphics, audio, controller, machine loop) and rendering
- `src/SSystem/`, `src/JSystem/`(headers) — J-prefixed engine subsystems (heaps, resources, kankyo, etc.)
- `src/Z2AudioLib/`, `src/Z2AudioCS/` — audio engine
- `src/REL/`, `src/NdevExi2A/`, `src/odemuexi2/` — REL modules and EXI device stubs

**Dusk layer** — the native reimplementation and enhancements: `src/dusk/` + `include/dusk/`. This is where new feature work happens: config/settings system, ImGui-based UI (`src/dusk/ui/`), achievements, crash handling/reporting, audio backends (`src/dusk/audio/`), HTTP (`src/dusk/http/` — per-platform: curl/winhttp/url_session/android), Discord rich presence, speedrun/LiveSplit tooling, frame interpolation, gyro, and the host `main.cpp`. Platform entry points live in `platforms/{windows,macos,ios,tvos,android,freedesktop}/`.

`extern/aurora/` provides the cross-platform GX graphics abstraction (D3D12/Vulkan/Metal), windowing, and input. Dusk code reaches it via `#include <aurora/aurora.h>` (see `include/dusk/dusk.h`).

`src/lingcod/` is a small stub layer for the NVIDIA SHIELD (Android TV) port's `NVGX`/`NVSI` graphics-tuning hooks.

## Build configuration

- New source files must be registered in **`files.cmake`** (hand-maintained `set(...)` lists like `DOLZEL_FILES`, `DUSK_FILES`). The PC executable is built from `DUSK_FILES`.
- The PC build compiles the original code with these key defines (see `CMakeLists.txt` ~line 330): `TARGET_PC`, `VERSION=0` (GCN USA), `WIDESCREEN_SUPPORT=1`, `AVOID_UB=1`, `MTX_USE_PS=1`.
- `include/global.h` defines the `VERSION_*` constants and the `PLATFORM_GCN/WII/SHIELD` and `REGION_USA/PAL/JPN/...` macros derived from `VERSION`.

## Apple Vision (visionOS) port

Dusklight builds and runs on visionOS (device + simulator), rendering to a standard `CAMetalLayer` window. Verified on the visionOS 26.5 simulator and a real Apple Vision Pro (visionOS 26.5).

**Prerequisites:** Xcode + the xrOS SDK (`xros` / `xrsimulator`); a Rust **nightly** with the visionOS std targets (`rustup target add aarch64-apple-visionos aarch64-apple-visionos-sim` — they ship prebuilt on current nightly, *not* tier-3). The vendored `ios.toolchain.cmake` already supports `PLATFORM=VISIONOS` / `SIMULATOR_VISIONOS`.

**Presets / build** (note the mandatory ObjC flag, see SDL patch below):
```sh
# Device (aarch64-apple-visionos, SDK xros)
cmake --preset visionos-default      -DCMAKE_OBJC_FLAGS="-Wno-error=return-mismatch" -DCMAKE_OBJCXX_FLAGS="-Wno-error=return-mismatch"
cmake --build --preset visionos-default
# Simulator (aarch64-apple-visionos-sim, SDK xrsimulator)
cmake --preset visionos-sim-default  -DCMAKE_OBJC_FLAGS="-Wno-error=return-mismatch" -DCMAKE_OBJCXX_FLAGS="-Wno-error=return-mismatch"
cmake --build build/visionos-sim-default
```

### Exact in-repo patches (tracked)

- **`CMakePresets.json`** — added `visionos-default` and `visionos-sim-default` (modeled on `tvos-default`; `PLATFORM=VISIONOS`/`SIMULATOR_VISIONOS`, `DEPLOYMENT_TARGET=2.0`, `Rust_CARGO_TARGET=aarch64-apple-visionos[-sim]`, `Rust_TOOLCHAIN=nightly`, `ENABLE_ARC=false`, find-package disables).
- **`CMakeLists.txt`** (4 hunks):
  - `PLATFORM_NAME`: add `elseif (CMAKE_SYSTEM_NAME STREQUAL visionOS) set(PLATFORM_NAME visionos)`, and fix the pre-existing bug `string(TOLOWER CMAKE_SYSTEM_NAME PLATFORM_NAME)` → `string(TOLOWER "${CMAKE_SYSTEM_NAME}" PLATFORM_NAME)` (visionOS hit the broken `else`).
  - Discord gate → append `AND NOT VISIONOS`.
  - Resource dir → add `elseif (VISIONOS) set(DUSK_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/visionos)`.
  - macOS file-select (AppKit) → `if (APPLE AND NOT IOS AND NOT TVOS AND NOT VISIONOS)`.
  - UIKit file-select → `if (IOS OR VISIONOS)` (compiles `src/dusk/ios/FileSelectDialog.m` for visionOS).
- **`platforms/visionos/`** (new): `Info.plist.in` (`CFBundleSupportedPlatforms=[XROS]`, `UIDeviceFamily=[7]`, `UIApplicationSceneManifest`, no orientation/`UIRequiresFullScreen` keys), `Base.lproj/LaunchScreen.storyboard`, `Assets.car` (an `actool`-compiled layered visionOS AppIcon — generated, **not** a game asset).
- **`src/dusk/file_select.cpp`** (the only real source bug): gate the dialog macros on `TARGET_OS_VISION` —
  `USE_MACOS_FOLDER_DIALOG` → `... && !TARGET_OS_VISION ...`; `USE_IOS_DIALOG` → `(TARGET_OS_IOS || TARGET_OS_VISION)`.
- **21:9 screen** (target ratio 64:27):
  - `include/dusk/dusk.h`: `defaultAspectRatioW/H = 64/27` (documentation-only constants; the `static_assert` becomes a float `> 2.0f` check).
  - `src/m_Do/m_Do_main.cpp`: seed `config.windowWidth = config.windowHeight * 64/27`; under `#if defined(__APPLE__) && defined(TARGET_OS_VISION) && TARGET_OS_VISION` force `AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT)` unconditionally (else keep the `lockAspectRatio` logic).
  - `src/m_Do/m_Do_graphic.cpp`: in `updateRenderSize()`, under the same visionOS guard pin `l_tvSize[1].width = (u16)(l_tvSize[1].height * 64.0f/27.0f)` instead of deriving from the live render AR.
  - Both `m_Do` files add `#ifdef __APPLE__ #include <TargetConditionals.h> #endif`.
- **`.github/workflows/build.yml`** — visionOS CI matrix entry + a nightly `aarch64-apple-visionos` Rust target install step.

### Throwaway SDL patches (NOT tracked — re-apply after a clean configure)

These live under `build/<preset>/_deps/` (wiped when SDL is re-extracted). They are deliberately uncommitted: they're local build fixes to the vendored SDL3 (Aurora pins `libsdl-org/SDL` ~3.5.0), and **SDL's own policy forbids AI-authored changes to its source**.

1. **`-Wno-error=return-mismatch`** (the `-DCMAKE_OBJC_FLAGS`/`OBJCXX` above): SDL's `SDL_UIKitBridge.m` returns a value from a `void` function, which Xcode 26.5's clang treats as a default *error*.
2. **Stub `SDL3/SDL3-Swift.h`**: SDL's visionOS layer (RealityKit "curved content") is written in Swift and is *never wired into SDL's CMake build* (true on upstream `main` too), so the generated `SDL3/SDL3-Swift.h` doesn't exist and `SDL_uikitviewcontroller.m` fails. The feature is optional (only invoked when `data.settings != nil`, which Aurora never sets) and the real Metal surface is the standard `CAMetalLayer` from `UIKit_Metal_CreateView` regardless. After configuring, create `build/<preset>/_deps/sdl-src/include/SDL3/SDL3-Swift.h`:
   ```objc
   #ifdef SDL_PLATFORM_VISIONOS
   @interface SDL_uikitviewcontroller (SDLVisionOSCurvedUI)
   - (void)initializeVisionOSCurvedUI;   // dynamically dispatched; never sent for this app
   @end
   #endif
   ```

### Critical gotcha: `TARGET_OS_IOS == 0` on visionOS

On xrOS, Apple sets `TARGET_OS_VISION=1` and `TARGET_OS_IPHONE=1` but **`TARGET_OS_IOS=0`**. So in-source `#if TARGET_OS_IOS` paths do *not* activate on visionOS — handle visionOS with explicit `TARGET_OS_VISION` checks. This produced the only link error (`file_select.cpp` fell into the macOS folder-dialog path → undefined `ShowMacOSFolderSelect`). Other `TARGET_OS_IOS`-keyed paths (the Documents data path in `data.cpp`, the touch overlay, `IsMobile`, audio heap sizing) likewise do not trigger on visionOS and may need re-gating for correct runtime behavior.

### Running

- **Simulator:** `xcrun simctl boot <vision-sim-udid>`; `xcrun simctl install <udid> build/visionos-sim-default/Dusklight.app`; `xcrun simctl launch <udid> dev.twilitrealm.dusk`. With no `--dvd` it opens the "Select Disc Image" prelaunch UI; pass `--dvd <path-inside-app-sandbox>` to boot straight into the game (copy the disc into the app container first).
- **Device:** the device build is **unsigned** — sign it before `devicectl` install: embed a development provisioning profile (whose App ID/wildcard covers the bundle id and whose `ProvisionedDevices`/team includes the Vision Pro) as `Dusklight.app/embedded.mobileprovision`, then `codesign --force --generate-entitlement-der --sign "<Apple Development cert>" --entitlements <ent.plist with application-identifier=<TEAM>.dev.twilitrealm.dusk, team-identifier=<TEAM>, get-task-allow=true>`. Install: `xcrun devicectl device install app --device <id> Dusklight.app`. Push a disc into the sandbox: `xcrun devicectl device copy to --device <id> --domain-type appDataContainer --domain-identifier dev.twilitrealm.dusk --source <disc> --destination Documents/<disc>`. Note: `devicectl ... process launch` **stalls** on visionOS — launch from the headset's Home View instead.

## Code conventions (`docs/code-conventions.md`)

- **Delineate Dusk modifications to original code.** When changing decomp code for Dusk's purposes, guard it with `#if TARGET_PC` and keep the original code in place. Use `#if AVOID_UB` for undefined-behavior fixes, `WIDESCREEN_SUPPORT` for widescreen. Convenience helpers in `global.h`: `IF_DUSK(stmt)`, `IF_NOT_DUSK(stmt)`, `IF_DUSK_ARG(expr)`, `DUSK_IF_ELSE(dusk, orig)`, `DUSK_CONST`.
- **No raw `new`/`delete` in original game code.** The original game allocates into a strict tree of heaps via global `operator new` overloads; these are replaced with `JKR_NEW`, `JKR_DELETE`, and friends (see `JKRHeap.h`).
- **Upstream when appropriate.** Bug fixes, docs, and cleanup that also apply to the original game should preferably be PR'd to [zeldaret/tp](https://github.com/zeldaret/tp).
- Much of `global.h` guards code paths for the matching MWCC/decomp build (`__MWERKS__`, `DECOMPCTX`) versus modern compilers — be careful editing these dual-path macros (`STATIC_ASSERT`, `IS_REF_NULL`, `SJIS`, `MULTI_CHAR`, etc.).
- C/C++ style is enforced by `.clang-format`; `.clangd` provides the language-server config.
