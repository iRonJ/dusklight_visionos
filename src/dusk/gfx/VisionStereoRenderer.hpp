#pragma once

namespace dusk::gfx {

struct VisionHeadPose {
    float translationX = 0.0f;
    float translationY = 0.0f;
    float translationZ = 0.0f;
    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;
    float rotationW = 1.0f;
    bool valid = false;
};

// Draws the current scene into independent left/right Aurora targets. Returns
// false when no active 3D view is available so the caller can use the normal
// center-eye path (title screens, loading, and other 2D-only states).
bool RenderVisionStereoFrame();

// Returns the horizontal off-axis projection term added for the active eye.
// Screen-projected effects use this to sample that eye's framebuffer with the
// same convergence plane as regular scene geometry.
float GetVisionStereoProjectionShift();

// CompositorServices can pause or replace its layer during system UI
// interruptions. App activity and the explicit user pause are independent
// inputs to the engine gate so neither can accidentally clear the other.
void RegisterVisionCompositor(const void* token);
void SetVisionCompositorRunning(const void* token, bool running);
void SetVisionAppActive(bool active);
void SetVisionGamePaused(bool paused);
bool IsVisionCompositorRunning();
bool IsVisionGamePaused();
bool IsVisionGameRunnable();
void WaitForVisionGameResume();

// Publishes the latest device pose relative to the diorama's anchor reference.
// The compositor thread writes it and the game render thread consumes a coherent
// snapshot. The renderer applies its own comfort limits before changing a camera.
void PublishVisionHeadPose(const VisionHeadPose& pose);
void ResetVisionHeadPose();

} // namespace dusk::gfx
