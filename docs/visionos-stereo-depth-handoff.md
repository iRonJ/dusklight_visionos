# visionOS "Window into the Scene" Stereo Depth — Handoff

## Goal

Dusklight currently runs on visionOS ([recent port](../CLAUDE.md#apple-vision-visionos-port)) by
painting the normal 2D game output onto a flat `CAMetalLayer` inside a regular windowed SDL/UIKit
app. Both eyes see the identical texture — there is no stereo separation, so it reads as a "TV
floating in space," not a window into a 3D scene.

The ask: give the presented image real per-eye depth/parallax — the "diorama" or "looking into a
window" effect — without doing a full engine-side stereo re-render (that option was evaluated and
explicitly deferred; see [Rejected approach](#rejected-approach-true-dual-eye-re-render) below).

**Chosen approach:** keep the game rendering mono, exactly as today, then in a new **full immersive
space** stage a Metal pass that turns the mono color+depth image into a depth-displaced stereo pair
(a "depth-parallax" or "2.5D relief" effect), composited via **CompositorServices**, with small
head-position-driven parallax on top so it doesn't read as a flat projected movie.

This document is a handoff — it records what's been confirmed by reading the actual SDKs/source in
this checkout, what's still open, and an ordered task list. It assumes no prior context beyond
`CLAUDE.md`.

## Reference material already in the environment

- `/Users/ronj/Documents/Splatanyl/apple/Splatanyl/Splatanyl/Renderers/VisionSceneRenderer.swift` —
  a working from-scratch CompositorServices host (Gaussian splat viewer). This is the best local
  example of the exact plumbing we need: `ImmersiveSpace`/`CompositorLayer` setup, per-eye
  `viewportCameras`, `ARKitSession`/`WorldTrackingProvider` for head pose, layered vs. dedicated
  per-view texture handling, `MTLVertexAmplificationViewMapping`, and the `estimatedClearDepth`
  trick for stable reprojection. **Read this file end to end before starting** — most of the
  boilerplate (frame loop, drawable handling, per-eye texture branching) can be copied near-verbatim.
- Colleague's advice (already validated against the SDK, see below):
  - Vertex amplification cuts 3D cost — relevant if we ever do true dual-eye re-render; for the
    chosen depth-parallax approach the draw is a single small grid mesh so it's optional, but cheap
    to add via `setVertexAmplificationCount` (see Splatanyl's layered-texture branch).
  - Head pose from ARKit to add small parallax on head movement — **do this**, it's what makes the
    depth-parallax trick read as "a window" instead of "a photo." `WorldTrackingProvider.queryDeviceAnchor(atTimestamp:)` per Splatanyl.
  - Start with full immersive view, quad-based — **confirmed correct and effectively mandatory**,
    see next section.
  - `GCDualSenseGamepad`/adaptive trigger — unrelated to this task, not covered here.

## Why full immersive + CompositorServices is mandatory (confirmed, not assumed)

Checked in the XROS 26.5 SDK (`$(xcrun --sdk xros --show-sdk-path)`):

- A plain windowed `CAMetalLayer` (what we do today) is composited by visionOS as a flat plane —
  identical content per eye, by design. No shader trick changes this; the compositor, not our app,
  decides how a normal window is placed in space.
- `CompositorLayer` (the SwiftUI type that gets you a `cp_layer_renderer_t`) only conforms to
  `ImmersiveSpaceContent`/`CompositorContent`. There is **no volumetric-window or plain-window
  variant** that exposes raw per-eye Metal drawables.
- Searched every visionOS framework header for a way to obtain a `cp_layer_renderer_t` without
  going through SwiftUI's `ImmersiveSpace { CompositorLayer(...) { layerRenderer in ... } }`
  closure. `UIScene.h` does define `UISceneSessionRoleImmersiveSpaceApplication`, confirming
  immersive space is backed by a UIKit scene role under the hood, but **no public UIKit API vends
  the `cp_layer_renderer_t` from that scene** — it is only ever handed to you inside the SwiftUI
  closure. This matches every Apple sample (all are SwiftUI apps).
- Conclusion: **a small SwiftUI shim is unavoidable.** This is a real bring-up item since the
  project currently has zero Swift and zero `enable_language(Swift)` — see
  [Open risk: Swift in the build](#open-risk-swift-in-the-cmakeninja-build).

## The Dawn/WebGPU ↔ raw Metal boundary (the trickiest part, now resolved)

Aurora renders through Dawn/WebGPU (`extern/aurora/lib/webgpu/gpu.cpp`), not raw Metal. Every
CompositorServices call, by contrast, deals in raw `id<MTLTexture>` (`cp_drawable_get_color_texture`
etc. — see `layer_renderer.h`/`drawable.h` in the CompositorServices headers). Getting pixels across
that boundary needed real investigation; here's what's actually available in the pinned Dawn
revision (`extern/aurora` submodule pins `libsdl-org/SDL` per CLAUDE.md; Dawn is fetched by Aurora's
CMake as `_deps/dawn-src` — inspect at `build/visionos-default/_deps/dawn-src` after configuring):

- `include/dawn/native/MetalBackend.h` (Dawn's **public** Metal interop header) exposes exactly two
  things:
  - `dawn::native::metal::GetMTLDevice(WGPUDevice)` — get the raw `id<MTLDevice>` Dawn is using.
  - `dawn::native::metal::WaitForCommandsToBeScheduled(WGPUDevice)` — **this function's doc comment
    is explicitly about Metal interop with other APIs**: it blocks until Dawn's submitted command
    buffers are scheduled, which is the correct sync point before another Metal client (our
    CompositorServices encoder) touches shared resources. We will need this.
  - **There is no public "give me the `id<MTLTexture>` behind this `wgpu::Texture`" function** in
    this Dawn revision (unlike the D3D11/D3D12 backends, which do have that kind of public
    interop header). Do not waste time looking for one — confirmed absent.
- What Dawn **does** support (found in `src/dawn/native/metal/SharedTextureMemoryMTL.{h,mm}` and
  confirmed present in the public generated `webgpu_cpp.h` via
  `wgpu::SharedTextureMemoryIOSurfaceDescriptor`, referenced from Dawn's own
  `SharedTextureMemoryTests_apple.mm`): **wrapping a client-owned `IOSurfaceRef` as a
  `wgpu::Texture`**, i.e. Dawn can render *into* a texture that is backed by an `IOSurface` we
  created ourselves. Since an `IOSurface`-backed `id<MTLTexture>` is trivial to also open directly
  in raw Metal (`[MTLDevice newTextureWithDescriptor:iosurface:plane:]`), this is our zero-copy
  bridge.

**Resulting design:** don't try to extract Dawn's *existing* internal color/depth buffers. Instead,
add one new WebGPU render pass (Dawn-side, using WGSL, fits naturally next to
`extern/aurora/lib/gfx/`) that:

1. Binds the already-existing `aurora::webgpu::present_source()` (final composited color,
   `g_frameBufferResolved`/`g_frameBuffer`) and `aurora::webgpu::g_depthBuffer` (already
   `Depth32Float` with `TextureBinding` usage — see `gpu.cpp:335-365` — so it's already sampleable,
   no changes needed there) as sampled textures.
2. Runs a depth-parallax fragment shader twice (once per eye, differing by a horizontal
   eye-separation/convergence uniform) into two small output textures sized to the compositor's
   per-eye viewport.
3. Those two **output** textures are the only ones that need to be IOSurface/SharedTextureMemory
   backed — created via `wgpu::SharedTextureMemoryIOSurfaceDescriptor` → `CreateSharedTextureMemory`
   → `.CreateTexture()`. This confines all the new interop complexity to a single, small,
   well-isolated spot.
4. On the CompositorServices side, each frame: call `WaitForCommandsToBeScheduled`, then a plain
   `MTLBlitCommandEncoder` copy from our two IOSurface-backed eye textures into
   `cp_drawable_get_color_texture(drawable, viewIndex)`. No extraction trickery needed there since
   we own the source textures' native handles from creation.
5. **Also write the compositor's depth attachment**, not just color: emit real per-pixel displaced
   depth from the same parallax shader pass into an equivalent IOSurface-backed depth texture, blit
   that into `cp_drawable_get_depth_texture`. This matters more than it looks: visionOS's
   asynchronous reprojection (time warp) uses the drawable's depth to correct for head motion
   between our render and the photon-out time. If we only submit color at one flat depth while the
   image itself has parallax baked in, reprojection will warp it incorrectly on head rotation and
   partially undo the effect (swimming/judder). This is also the concrete mechanism that prevents
   the "projected 3D movie" look the colleague warned about — real depth + `DeviceAnchor` micro-
   parallax together are what sells "a window," not either alone.

## Rejected approach: true dual-eye re-render

Considered and explicitly not chosen (user picked depth-parallax reprojection instead): replay the
GX FIFO twice per frame with a sheared projection injected at
`extern/aurora/lib/gx/command_processor.cpp:1416` (where the projection matrix is reconstructed from
GX register writes), into two separate EFB targets. Geometrically correct (fixes transparency/HUD
depth ordering issues the reprojection approach inherently has — alpha blended things like water,
fog, particles will inherit the depth of whatever's behind them under reprojection), but ~2x GPU
cost, 2x EFB memory, and requires snapshotting/restoring GX state mid-frame twice. Noting it here in
case the reprojection approach's edge-stretching/transparency artifacts prove unacceptable later —
the FIFO is a flat replayable buffer (`extern/aurora/lib/gx/fifo.cpp`), so this remains buildable as
a follow-up without re-deriving the projection-injection point.

## Open risk: Swift in the CMake/Ninja build

Confirmed via `grep enable_language CMakeLists.txt`: **zero Swift currently compiles anywhere in
this project.** `CMakeLists.txt:101` only does `enable_language(OBJC OBJCXX)`.

CMake's Swift support (`enable_language(Swift)`) with the Ninja generator (which is what all our
presets use, including `visionos-default`) mixed into a target that also has C++/ObjC++ sources is
poorly supported / effectively unofficial — CMake's Swift+Ninja story historically assumes
Swift-only targets. Attempting to add Swift files directly to the existing `DUSK_FILES`/executable
target is likely to be a dead end.

**Recommended approach:** build the SwiftUI/`ImmersiveSpace`/`CompositorLayer` shim as its own
small, Swift-only static library CMake target (e.g. `dusklight_visionos_swift`), and link it into
the main executable. That shim needs to expose only a minimal C ABI (`@_cdecl` functions, or a
bridging header) to hand control to the existing/new ObjC++ Metal code once it has a
`LayerRenderer`. Keep the shim as thin as possible — ideally just:
```swift
@main
struct DusklightVisionApp: App {
    var body: some Scene {
        ImmersiveSpace {
            CompositorLayer(configuration: DusklightCompositorConfig()) { layerRenderer in
                dusklight_visionos_run(layerRenderer) // extern "C", defined in ObjC++
            }
        }
    }
}
```
Everything else (frame loop, ARKit, Metal encoding) can stay in ObjC++ alongside the rest of
`src/dusk/ios/`, matching the existing `FileSelectDialog.m` precedent for platform-specific ObjC in
this codebase.

This is the single highest-uncertainty item in this whole plan — validate it first (Task 1) before
writing any renderer code, since if it doesn't pan out the fallback (all-Swift immersive host calling
into C++ via a bridging header, mirroring Splatanyl's structure more directly) changes the file
layout for everything else.

## Open questions to resolve during bring-up (not blocking, but flag early)

- **World-sensing permission prompt.** Splatanyl's `VisionSceneRenderer` uses
  `ARKitSession`/`WorldTrackingProvider` to get `DeviceAnchor` for head pose. Verify during bring-up
  whether this triggers a "world sensing" permission prompt on first launch for a fully-immersive
  game (some rendering-only head-pose queries are exempt; not confirmed either way for this SDK
  version). If it does, `Info.plist.in` needs an `NSWorldSensingUsageDescription` key added, and the
  first-run flow needs to tolerate the user denying it (fall back to head-locked, no micro-parallax).
- **SDL window coexistence.** The existing SDL/UIKit window (used for input, and for the
  pre-launch "Select Disc Image" UI per `CLAUDE.md`) can keep existing in the background — visionOS
  immersive space is a separate compositor layer, it doesn't need to be destroyed. Confirm gamepad/
  keyboard input continues to route through it correctly once an immersive space is active and has
  focus; SDL's event pump may need to keep running on whatever thread currently owns it (today,
  `main01()`'s loop in `src/m_Do/m_Do_main.cpp`) independent of the new CompositorServices render
  thread (which, per Splatanyl's pattern, must be its own dedicated `Thread`).
- **CMake configure is currently broken on a clean checkout, unrelated to this work.** Running
  `cmake --preset visionos-default` right now fails during Dawn's vendored `protobuf` configure
  step (`Cannot specify link libraries for target "libprotobuf" which is not built by this
  project`), before reaching any of our code. This needs to be fixed (likely a `zlib`/protobuf CMake
  option Dawn expects that isn't satisfied on this host — `-- Could NOT find ZLIB` appears just
  above the failure) before any of the tasks below can be compile-tested. Rust nightly +
  `aarch64-apple-visionos`/`-sim` targets are already installed via `rustup` in this environment, so
  that prerequisite is done; this is a separate, Dawn-side CMake issue.

## Task list

Roughly ordered; items in the same numbered group can be parallelized.

### 1. De-risk the Swift bring-up (spike, do first)
- Fix the Dawn/protobuf CMake configure failure blocking `visionos-default` (see above) — needed
  before anything else can be built/tested.
- Add a minimal `dusklight_visionos_swift` static-lib CMake target with a single trivial `.swift`
  file, `enable_language(Swift)` gated to `VISIONOS`/`SIMULATOR_VISIONOS`, and confirm it links into
  the existing executable target and that a C-callable symbol from it is reachable from ObjC++.
  This validates the whole approach in `docs/visionos-stereo-depth-handoff.md#open-risk-swift-in-the-cmakeninja-build`
  before investing in the real renderer.
- Files: `CMakeLists.txt`, `CMakePresets.json` (new Swift-related cache vars if needed),
  new `platforms/visionos/Swift/` directory, `files.cmake`.

### 2. Immersive space host (Swift shim + scene plumbing)
- `platforms/visionos/Swift/DusklightVisionApp.swift`: `@main` `App` with a single `ImmersiveSpace`
  scene containing a `CompositorLayer`. Model closely on
  `Splatanyl/Renderers/VisionSceneRenderer.swift`'s `init`/`startRenderLoop`/`renderLoop` structure,
  but the renderer target is our new ObjC++ code (Task 3), not a Swift splat renderer — the Swift
  file should be a thin pass-through once `layerRenderer` is obtained.
- Decide immediately whether ARKit device-anchor querying needs the world-sensing entitlement (see
  Open Questions) and stub around it if uncertain, so Task 4 isn't blocked.
- Update `platforms/visionos/Info.plist.in`: add `UIApplicationSceneManifest` entry for the
  immersive space scene role (`UISceneSessionRoleImmersiveSpaceApplication`), matching what
  SwiftUI's `ImmersiveSpace` needs at the Info.plist level (check current Apple sample project
  Info.plist for the exact required keys — SwiftUI may generate this automatically in an Xcode
  project but since we hand-author the plist here it likely needs the scene manifest entry added
  explicitly).

### 3. ObjC++ immersive renderer host
- New file, e.g. `src/dusk/ios/VisionCompositorRenderer.mm` (+ `.h`), compiled only for
  `VISIONOS`/`SIMULATOR_VISIONOS` in `files.cmake`.
- Owns: the CompositorServices render loop (`cp_layer_renderer_query_next_frame`,
  `cp_drawable_compute_projection`, per-view color/depth texture access — mirror
  `VisionSceneRenderer.renderFrame()`'s dedicated-vs-layered-texture branching, it already handles
  both device configurations), the `MTLBlitCommandEncoder` copy step from Task 5's eye textures into
  the drawable, and (once validated) the `ARKitSession`/`DeviceAnchor` micro-parallax input.
- Runs on its own dedicated thread, per Splatanyl's pattern — does not block or get blocked by
  `main01()`'s loop in `src/m_Do/m_Do_main.cpp`.

### 4. Aurora: expose a hook to run an extra post-process pass on the finished frame
- `extern/aurora` changes (keep them minimal and clearly delineated, this is a submodule — consider
  whether these should eventually be upstreamed to `encounter/aurora` separately from Dusklight-only
  code, per `CLAUDE.md`'s "delineate Dusk modifications" convention, adapted for this dependency).
- Add an optional callback/extension point, invoked from `aurora::end_frame()` in
  `extern/aurora/lib/aurora.cpp` right after `gfx::render(encoder)` but before (or instead of, when
  no on-screen surface exists) the existing EFB→XFB blit — giving Dusk-layer code the `encoder`, and
  read access to `webgpu::present_source()` (color) and `webgpu::g_depthBuffer` (depth).
- On visionOS, `main01()`'s frame loop still calls `aurora_begin_frame()`/`aurora_end_frame()`
  unchanged (`src/m_Do/m_Do_main.cpp:183,193` and `:269,338` — there are two call sites, the UI-only
  pre-launch loop and the main game loop, both need the hook to at least no-op correctly when the
  immersive space isn't active yet, e.g. during disc-select).

### 5. The depth-parallax stereo pass (new WebGPU/WGSL shader)
- New pass, likely under `extern/aurora/lib/gfx/` alongside `clear.cpp`/`depth_peek.cpp` as a
  sibling (e.g. `stereo_parallax.{cpp,hpp}`), or Dusk-side if that fits better once Task 4's hook
  shape is settled.
- Binds `present_source()` + `g_depthBuffer` as sampled textures (both already correctly usage-
  flagged for this — no changes needed to their creation in `gpu.cpp`).
- Renders a displaced grid mesh (not a flat quad — the whole point is per-pixel depth
  displacement) twice, once per eye, into two IOSurface-backed `wgpu::Texture` render targets
  created via `wgpu::SharedTextureMemoryIOSurfaceDescriptor` (see architecture section above).
  Also render the corresponding displaced-depth output for the compositor's depth attachment.
- Exposed tunables (for a settings UI later, not required for MVP): eye separation / "depth
  strength" and convergence distance — these directly control how aggressive the "looking into a
  window" effect reads; expect to need to tune against the game's typical depth range (mostly indoor/
  outdoor field-of-view scenes) rather than guessing values up front.

### 6. Bring-up / testing
- Simulator first (`visionos-sim-default`), per the existing device-side instructions in
  `CLAUDE.md`'s Apple Vision section for install/launch commands (`xcrun simctl boot/install/launch`).
- Then real device — note `CLAUDE.md`'s existing warning that `devicectl ... process launch`
  **stalls** on visionOS; launch from the headset's Home View instead, same as the existing flat
  build.
- Validate: no stereo divergence/eye strain from a bad convergence default, reprojection doesn't
  visibly swim on head rotation (this is the signal that depth-attachment submission in Task 5 is
  wired correctly), and HUD/alpha-blended elements (menus, subtitles) don't look obviously wrong
  under the parallax displacement (likely candidate for "render UI flat, no displacement" as a
  follow-up refinement if it looks bad — not scoped here).

## Summary of confirmed facts (quick reference)

| Question | Answer | Where confirmed |
|---|---|---|
| Can a windowed CAMetalLayer show stereo on visionOS? | No | XROS 26.5 SDK: `CompositorLayer` only conforms to `ImmersiveSpaceContent` |
| Can we get a `cp_layer_renderer_t` without SwiftUI? | No | No vending API found in any visionOS framework header |
| Does Dawn (pinned rev) expose raw `id<MTLTexture>` from a `wgpu::Texture`? | No | `include/dawn/native/MetalBackend.h` only has `GetMTLDevice`/`WaitForCommandsToBeScheduled` |
| Can Dawn render into a client-owned IOSurface? | Yes | `SharedTextureMemoryMTL.{h,mm}`, `wgpu::SharedTextureMemoryIOSurfaceDescriptor` in generated `webgpu_cpp.h` |
| Is `g_depthBuffer` already sampleable? | Yes, no changes needed | `extern/aurora/lib/webgpu/gpu.cpp:335-365`, `TextureUsage::TextureBinding` already set |
| Does Swift currently build anywhere in this project? | No | `CMakeLists.txt:101`, only `enable_language(OBJC OBJCXX)` |
| Is Rust nightly + visionOS std installed on this machine? | Yes | `rustup target list --toolchain nightly --installed` shows `aarch64-apple-visionos[-sim]` |
| Does `cmake --preset visionos-default` currently configure cleanly? | No | Fails in Dawn's vendored protobuf/zlib CMake step, unrelated to this task |
