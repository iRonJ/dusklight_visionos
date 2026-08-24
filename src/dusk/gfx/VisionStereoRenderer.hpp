#pragma once

namespace dusk::gfx {

// Draws the current scene into independent left/right Aurora targets. Returns
// false when no active 3D view is available so the caller can use the normal
// center-eye path (title screens, loading, and other 2D-only states).
bool RenderVisionStereoFrame();

// CompositorServices can pause or replace its layer during system UI
// interruptions. These functions let the engine stop advancing while no
// drawable consumer exists, then resume the same game session.
void RegisterVisionCompositor(const void* token);
void SetVisionCompositorRunning(const void* token, bool running);
bool IsVisionCompositorRunning();

} // namespace dusk::gfx
