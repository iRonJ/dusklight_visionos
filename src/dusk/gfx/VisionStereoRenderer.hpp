#pragma once

namespace dusk::gfx {

// Draws the current scene into independent left/right Aurora targets. Returns
// false when no active 3D view is available so the caller can use the normal
// center-eye path (title screens, loading, and other 2D-only states).
bool RenderVisionStereoFrame();

// Returns the horizontal off-axis projection term added for the active eye.
// Screen-projected effects use this to sample that eye's framebuffer with the
// same convergence plane as regular scene geometry.
float GetVisionStereoProjectionShift();

// CompositorServices can pause or replace its layer during system UI
// interruptions. These functions let the engine stop advancing while no
// drawable consumer exists, then resume the same game session.
void RegisterVisionCompositor(const void* token);
void SetVisionCompositorRunning(const void* token, bool running);
void SetVisionAppActive(bool active);
bool IsVisionCompositorRunning();

} // namespace dusk::gfx
