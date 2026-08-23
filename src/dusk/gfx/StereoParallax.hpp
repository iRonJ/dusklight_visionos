#pragma once

#include <cstdint>
#include <memory>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

namespace wgpu {
class Device;
class Queue;
class CommandEncoder;
} // namespace wgpu

namespace dusk::gfx {

struct StereoParallaxSettings {
    float eyeSeparation = 0.035f;      // Interpupillary distance / parallax strength
    float convergenceDepth = 0.5f;     // Distance / depth value where disparity is zero
    float depthScale = 1.0f;           // Scaling factor for depth displacement
    bool enabled = true;
};

class StereoParallaxPass {
public:
    StereoParallaxPass();
    ~StereoParallaxPass();

    bool Initialize(uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    void Render(void* encoderPtr);

    void SetSettings(const StereoParallaxSettings& settings) { m_settings = settings; }
    const StereoParallaxSettings& GetSettings() const { return m_settings; }
    StereoParallaxSettings& GetSettings() { return m_settings; }

#if defined(__APPLE__)
    IOSurfaceRef GetLeftColorSurface() const { return m_leftColorSurface; }
    IOSurfaceRef GetRightColorSurface() const { return m_rightColorSurface; }
    IOSurfaceRef GetLeftDepthSurface() const { return m_leftDepthSurface; }
    IOSurfaceRef GetRightDepthSurface() const { return m_rightDepthSurface; }
#endif

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool IsInitialized() const { return m_initialized; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;
    StereoParallaxSettings m_settings;

#if defined(__APPLE__)
    IOSurfaceRef m_leftColorSurface = nullptr;
    IOSurfaceRef m_rightColorSurface = nullptr;
    IOSurfaceRef m_leftDepthSurface = nullptr;
    IOSurfaceRef m_rightDepthSurface = nullptr;
#endif
};

StereoParallaxPass* GetStereoParallaxPass();
void InitializeStereoParallaxHook();

} // namespace dusk::gfx
