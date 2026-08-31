# visionOS stereoscopic rendering

This document describes the current Apple Vision Pro renderer in the
`dusklight_visionos` fork. It targets visionOS 26.5 and renders Twilight Princess as a
world-anchored stereoscopic diorama. It is an implementation guide for contributors, not a general
build guide; see the [README](../README.md#apple-vision-pro) for build, signing, and deployment.

## What the renderer does

The production path performs two independent GX scene draws, one for each eye. Each draw has a
parallel, off-axis game camera and produces its own color and reversed-Z depth target. A WebGPU pass
publishes the pair into IOSurfaces, and a separate Metal/Compositor Services loop displays the left
or right IOSurface on a world-space quad for the corresponding headset view.

```text
game simulation and presentation camera
             |
             +-- left GX draw  --> Aurora color + reversed-Z depth --+
             |                                                    |
             +-- right GX draw --> Aurora color + reversed-Z depth --+
                                                                  |
                                  StereoParallax WebGPU pass       |
                                  (copy/publish; depth-warp fallback)
                                               |
                                  left/right BGRA8 IOSurfaces
                                  + MTLSharedEvent fences
                                               |
                                  Metal Compositor Services loop
                                  + ARKit device anchor
                                               |
                                  per-view world-space diorama quad
```

This is not spatial video and does not use AVKit stereo-buffer tags. It is also no longer primarily
a single-image depth reprojection renderer. The WGSL depth-warp implementation remains as a
fallback when a complete dual-eye capture cannot be produced.

### Bounded head-tracked camera

ARKit head pose keeps the diorama quad stable in world space and gives Compositor Services a device
anchor for late reprojection. On the `visionos-limited-6dof` branch, the compositor also publishes
the device transform relative to the current diorama anchor. The GX renderer maps that relative
translation and orientation into the game's presentation-camera basis before applying the left and
right eye offsets.

For comfort, this is deliberately not an unrestricted VR camera:

- Translation is radially limited to 0.3048 m (one foot) from the anchor reference.
- Rotation is limited to a 15-degree shortest-arc cone around the anchor orientation.
- Response is 1:1 through 75% of each range, then enters a cubic Hermite shoulder and reaches the
  hard cap with zero slope at 125% physical input.
- A frame-rate-independent 55 ms low-pass filter suppresses ARKit sample jitter before limiting.
- Tracking loss, recenter, app stop, and scene transitions reset the filtered pose to neutral. A
  recovered pose eases in instead of applying a stale accumulated delta in one frame.

The pose changes only the presentation camera; it does not mutate simulation or the game's saved
camera. The original matrices are restored after both eye captures. The physical compositor depth
is still the flat quad, so this bounded camera can reveal more of the in-game view but does not make
the game world independently occlude other visionOS content at per-pixel depth.

## Application and thread model

visionOS owns the real process entry point through the SwiftUI `@main` app in
`platforms/visionos/Swift/DusklightVisionApp.swift`. The app declares two scenes:

- A `WindowGroup` containing the disc picker and companion controls.
- An `ImmersiveSpace` containing a `CompositorLayer`, which is the supported route to a
  `LayerRenderer` and stereoscopic compositor drawables.

After the immersive space opens, Swift calls `dusklight_start_game_with_iso()` in
`src/dusk/main.cpp`. The C++ game runs on one detached thread. The compositor host in
`src/dusk/ios/VisionCompositorRenderer.mm` owns another thread and consumes the newest completed
eye pair at the system compositor cadence. The two loops are intentionally decoupled: the
compositor can redisplay the latest pair when the game has not produced a new one.

The visionOS target must not link `aurora::main`. That library owns the normal SDL process entry
point and conflicts with SwiftUI's `@main`. The conditional link and Swift static library are in the
visionOS blocks of `CMakeLists.txt`.

## Headless Aurora

Aurora normally refuses to begin a frame until an SDL window has a presentable WebGPU surface.
There is no such surface in this architecture. The visionOS Aurora fork therefore:

1. Skips SDL video/window creation in `extern/aurora/lib/window.cpp`.
2. Sets `aurora::webgpu::g_headless` and requests a high-performance adapter without a compatible
   surface in `extern/aurora/lib/webgpu/gpu.cpp`.
3. Creates the WebGPU device and offscreen render resources without a swapchain.
4. Lets `begin_frame()` execute the complete GX frame in `extern/aurora/lib/aurora.cpp`.
5. Skips surface acquire/present, but still invokes the post-render callback and submits the Dawn
   command buffer.

The headless render size is fixed at 2560x1440 in `extern/aurora/lib/window.cpp`. This supersamples
the source relative to the original 1920x1080 target and reduces aliasing when the physical
diorama is small or distant. Changing the companion window's width or distance does not change
this render resolution.

The Aurora device requires Dawn's `SharedTextureMemoryIOSurface` and
`SharedFenceMTLSharedEvent` features. Without them the renderer cannot publish GPU work to Metal
without a CPU readback.

## Per-eye GX rendering

`RenderVisionStereoFrame()` in `src/dusk/gfx/VisionStereoRenderer.cpp` is called from the
interpolated presentation path in `src/m_Do/m_Do_main.cpp`.

For each displayed game frame it:

1. Saves every camera matrix and the `lookat_class` state from the game camera.
2. Applies the filtered and bounded anchor-relative head pose to a temporary presentation camera.
3. Starts a tagged Aurora capture for the left eye.
4. Translates the camera along its local right vector and adds an opposite horizontal projection
   offset.
5. Re-runs the camera-dependent actor draw traversal and the GX painter.
6. Ends the capture and restores the tracked center camera.
7. Repeats the same work for the right eye without advancing UI animation state a second time.
8. Restores the game's original camera and submits the coherent capture pair to
   `StereoParallaxPass`.

The cameras are parallel; they are never toed inward. For a camera offset `e`, projection scale
`Pxx`, and convergence distance `c`, the horizontal projection term is adjusted by:

```text
projection[0][2] += -Pxx * e / c
```

Parallel off-axis cameras avoid vertical disparity and the scale/shear distortion seen with toe-in
cameras. `eyeSeparation` is treated as the half-eye offset in meters and converted to game units at
100 game units per meter. The default `0.035` therefore produces a 7 cm full separation.

The companion window's **Scene Plane** control changes the convergence distance, not the physical
distance of the Metal quad. Screen-space HUD and menu geometry does not receive the 3D camera
offset, so it remains at zero disparity on the perceived window plane. During scene transitions,
eye separation is temporarily set to zero because wipes, fades, and loading imagery do not have a
stable world depth and otherwise create a brief uncomfortable disparity spike.

### Draw-list and state caveat

An eye draw is not just a second call to the final framebuffer pass. Actor traversal creates
camera-dependent model matrices, packets, translucent ordering, shadows, and effect state. Reusing
only the first eye's finished geometry caused clones, spikes, and smeared silhouettes in the other
eye. Each eye must rerun `fpcM_DrawIterater()` and `cAPIGph_Painter()`.

Some scene and menu packets are prepared by the simulation tick and are not reconstructed by a
presentation-only traversal, so the renderer deliberately preserves those persistent lists. The
first painter call advances per-frame UI/fade state; `set_ui_tick_pending(false)` prevents the
right-eye draw from advancing it again. Changes to draw-list reset behavior must be tested in
gameplay, pause menus, save selection, fades, particles, and cutscenes.

## Aurora eye capture

The tagged offscreen capture API is implemented in `extern/aurora/lib/gfx/common.cpp`:

- `begin_capture(width, height, tag)` suspends the EFB target and begins a cached, tagged color and
  depth target.
- Nested GX framebuffer effects can temporarily suspend the eye target.
- Render passes whose results are resumed or exported are marked `externallyConsumed`; otherwise
  Aurora's render-pass optimizer may discard them as apparently unobservable.
- `end_capture()` drains queued GX commands, verifies framebuffer nesting, exports color and depth,
  and resumes the EFB.
- An unfinished nested effect is unwound and the eye capture is rejected instead of publishing a
  partial frame. The caller then draws a complete center-eye fallback.

This machinery is necessary because Twilight Princess performs many internal `GXCopyTex`
operations. Earlier versions lost textures, produced white geometry, or blacked out after
transitions because a producer pass was skipped or a nested framebuffer remained active at the
capture boundary. Warnings containing `Recovering ... unfinished framebuffer pass(es)` indicate
that an effect violated this contract and should be investigated rather than ignored.

`set_offscreen_uses_native_logical_size(true)` makes the outer eye capture behave like the native
EFB while preserving an effect's own logical dimensions for nested offscreen passes.

## WebGPU to IOSurface publication

`StereoParallaxPass` in `src/dusk/gfx/StereoParallax.cpp` is installed as Aurora's post-render
callback. It owns one IOSurface-backed BGRA8 WebGPU texture per eye.

On the normal dual-draw path, the pass samples the matching captured color/depth target and writes
it to the IOSurface. Stereo displacement is set to zero because perspective is already present in
the two independent GX views. The pass still copies the captured reversed-Z depth into its private
depth target, although the current Metal compositor publishes the game image as a flat quad and
does not expose per-pixel game depth to Compositor Services.

On a capture failure or a 2D-only frame, both eyes use the center-eye source and the WGSL fragment
shader performs inverse depth reprojection. It solves for a source pixel from several depth
hypotheses and fills disocclusions from the farthest available layer. This is a resilience path,
not equivalent to a second scene draw: a single color/depth image cannot recover geometry hidden
behind foreground silhouettes, which is why the old renderer produced duplicated outlines and a
"warped glass" strip around moving models.

The EFB alpha channel contains game render state, not spatial-layer transparency, so the WebGPU
pass forces alpha to 1 inside the game image. Allowing EFB alpha through made the entire game
translucent and caused backgrounds to disappear. Reversed-Z uses 1 for near and 0 for far;
`GreaterEqual` depth testing is required so sky and cleared far-plane pixels survive.

After Dawn finishes writing an IOSurface texture, `SharedTextureMemory::EndAccess` returns a shared
fence. The code exports its `MTLSharedEvent` and signaled value with the IOSurface pointer. This is a
zero-CPU-copy handoff, but it is still a GPU render pass from Aurora's captured texture into the
shared texture.

## Compositor Services presentation

`VisionCompositorRenderer.mm` follows the visionOS 26 frame contract:

1. Query the next frame.
2. Start/end update.
3. Predict timing and wait until optimal input time.
4. Query the drawable array and select the built-in headset target.
5. Start submission.
6. Query the ARKit device anchor at the predicted presentation timestamp and attach it to the
   drawable.
7. Encode the per-view Metal passes, encode present, commit the command buffer, and end submission.

Once submission starts, a queried drawable must be completed and presented. When tracking or game
surfaces are unavailable, the renderer clears, encodes present, and commits rather than abandoning
the drawable. Before the first engine frame, it displays a cyan/magenta diagnostic diorama so the
Compositor Services and tracking path can be tested independently.

The compositor supports both layered array textures and dedicated per-eye textures. For each
`cp_view_t`, it selects the corresponding source IOSurface, computes Apple's projection matrix,
combines it with the predicted device/eye transform and world-space diorama transform, and renders
a 1.6 m by 0.9 m base quad. The command buffer waits directly on both Dawn-exported shared events;
there is no CPU fence wait.

The IOSurface is wrapped as `BGRA8Unorm_sRGB`. The game output is sRGB encoded, so Metal sampling
decodes to linear and the sRGB compositor drawable encodes once on store. Treating it as linear
caused incorrect gamma. The Metal shader also performs footprint-aware 3x3 downsampling when a
source texel footprint becomes large and antialiases the quad edge. IOSurfaces have no mip chain,
so distant images remain limited by fixed source resolution and this explicit filter.

Compositor foveation is intentionally disabled. The foveation map caused the explicit right-eye
pass to curl toward one side of the field of view. Resolution is recovered through the fixed
2560x1440 eye targets instead. This costs GPU time: a displayed frame can require two complete GX
draws, the two WebGPU publication passes, and two Metal quad passes.

## World anchoring and lifecycle

`VisionDioramaAnchor.mm` owns an ARKit session and world-tracking provider. At first valid pose,
the compositor stores the complete device transform as its anchor reference, including orientation,
then places the quad in front of that reference. Recenter discards the reference and captures a new
one on the next tracked frame. The reference is kept across compositor-layer replacement so opening
Home View or a notification does not automatically recenter the diorama.

The immersive scene phase, compositor state, and explicit companion-window Pause control are kept as
independent inputs to the engine gate. If any input suspends the session, `m_Do_main.cpp` pauses audio
and blocks on a condition variable without advancing or rendering the game. Resume wakes the same
game thread, resets frame timing, and preserves the current session. If visionOS
replaces the `LayerRenderer`, the old compositor thread is stopped and joined before its host is
released; destroying a joinable C++ thread caused an earlier crash.

The companion window is the supported control surface for aspect ratio, physical width, physical
distance, convergence plane, recenter, and explicit Pause/Resume. Manual pause freezes the latest
diorama frame and remains set across a Home View interruption until Resume is selected. The panel
closes after eight seconds of inactivity while the game is running; pinching the game quad opens it
again. Tracking-area rendering and spatial-event code exists for this selection gesture, but direct
pinch manipulation of the quad is not currently reliable and is not advertised as supported.

## Projection-textured effects

Water, reflections, portals, cloud shadows, particles, and other legacy effects may sample a copy
of the current framebuffer through a `C_MTXLightPerspective` texture matrix. A normal model follows
the per-eye view automatically; a framebuffer-projected material does not. Without special handling
it appears at zero disparity, duplicates submerged geometry, or samples the other eye's framebuffer.

The current fixes are split across:

- `mDoGph_gInf_c::setStereoLightProjection()` in `src/m_Do/m_Do_graphic.cpp`, which adds the active
  eye's off-axis projection term to a texture-projection matrix.
- The water/effect actors that construct their own projection matrices, including the `lv3Water`,
  groundwater, portal, mirror-hole, and related environment paths.
- `JPADrawInfo`, which applies the same correction to projection-textured particles.
- A per-eye `retry_capture_frame()` before invisible framebuffer-sampling lists, with copy dimensions
  matching the actual half-resolution framebuffer texture.

These paths are data- and material-dependent. A newly encountered effect that uses texture names
such as `dummy` or `fbtex_dummy`, an invisible draw list, or its own LightPerspective matrix may
need the same per-eye correction. Castle Sewer's `GRDWATER` actor reprojects the rendered scene in a
way that doubles temporary head-pose rotation. While that actor is loaded, the renderer smoothly
neutralizes bounded 6DOF head tracking but retains the independent left/right eye draws. Tracking
smoothly resumes after the actor is removed. This actor-scoped fallback is intentionally conservative,
and this area should not be considered exhaustive.

## Key files

| File | Responsibility |
| --- | --- |
| `platforms/visionos/Swift/DusklightVisionApp.swift` | SwiftUI entry point, launcher, companion controls, immersive lifecycle |
| `platforms/visionos/Swift/DusklightCompositorConfig.swift` | Drawable formats, layout, tracking areas, foveation policy |
| `src/dusk/main.cpp` | Starts one persistent game thread from Swift |
| `src/m_Do/m_Do_main.cpp` | Engine lifecycle gating and stereo-frame dispatch |
| `src/dusk/gfx/VisionStereoRenderer.cpp` | Camera snapshot, parallel/off-axis per-eye GX draws, fallback selection |
| `src/dusk/gfx/StereoParallax.cpp` | IOSurfaces, WebGPU publication/depth-warp fallback, shared fences |
| `src/dusk/ios/VisionCompositorRenderer.mm` | Compositor Services frame loop, Metal draw, placement and resume |
| `src/dusk/ios/VisionDioramaAnchor.mm` | ARKit session and predicted device-anchor lookup |
| `src/dusk/ios/VisionDioramaShaders.h` | Metal quad shaders, downsampling, edge coverage, diagnostic room geometry |
| `src/m_Do/m_Do_graphic.cpp` | Painter ordering, per-eye framebuffer copies, projected-effect correction |
| `extern/aurora/lib/aurora.cpp` | Headless frame begin/end and post-render callback |
| `extern/aurora/lib/webgpu/gpu.cpp` | Surfaceless Dawn device and Apple shared-memory features |
| `extern/aurora/lib/window.cpp` | No-SDL-window behavior and fixed 2560x1440 target |
| `extern/aurora/lib/gfx/common.cpp` | Tagged color/depth capture and nested-pass preservation/recovery |

## Debugging checklist

- Use `TARGET_OS_VISION`, not `TARGET_OS_IOS`; Apple defines the latter as 0 on visionOS.
- Launch immersive builds from Home View. `devicectl process launch` has been unreliable.
- Filter Console for `[Dusklight]`, `[DuskStereo]`, `[DuskDepthProbe]`, and `aurora::gfx`.
- A checkerboard with no game image means Compositor Services works but no fenced IOSurface pair is
  available yet.
- `Presenting a drawable without a device anchor` means ARKit was queried before its provider was
  running or the anchor was not attached.
- Repeated capture-recovery warnings usually predict black frames or a fallback to center-eye
  reprojection. Locate the GX effect that left a nested framebuffer open.
- A right-eye curl suggests foveation/layout mismatch. Confirm foveation is disabled before changing
  eye-camera math.
- Transparent output usually means EFB alpha escaped into spatial-layer opacity. Missing sky often
  means reversed-Z far pixels failed the depth test.
- Incorrect brightness usually means the IOSurface was wrapped with the wrong sRGB/linear format.
- Test renderer changes in gameplay, save/load UI, pause menus, water, particles, cutscenes, scene
  transitions, Home View interruption, and companion-window resume.

Controller tuning remains available for development: hold L+R, use D-pad Up/Down for eye
separation, D-pad Left/Right for convergence, and Z to toggle stereo.
