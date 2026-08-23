#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

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

#if defined(__APPLE__)
struct StereoParallaxAppleFrame {
    IOSurfaceRef colorSurface = nullptr;
    void* readyEvent = nullptr;
    uint64_t readyValue = 0;
};

struct StereoParallaxAppleFramePair {
    StereoParallaxAppleFrame left;
    StereoParallaxAppleFrame right;
    uint64_t generation = 0;
};
#endif

struct StereoParallaxSettings {
    float eyeSeparation = 0.035f;      // Interpupillary distance / parallax strength
    float convergenceDepth = 0.5f;     // Distance / depth value where disparity is zero
    float depthScale = 1.0f;           // Scaling factor for depth displacement
    bool enabled = true;               // True = Vision Pro 2-Eye 3D Diorama, False = 2D Window
};

class StereoParallaxPass {
public:
    StereoParallaxPass();
    ~StereoParallaxPass();

    bool Initialize(uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    void Render(void* encoderPtr);

    void SetSettings(const StereoParallaxSettings& settings) {
        m_eyeSeparation.store(settings.eyeSeparation, std::memory_order_relaxed);
        m_convergenceDepth.store(settings.convergenceDepth, std::memory_order_relaxed);
        m_depthScale.store(settings.depthScale, std::memory_order_relaxed);
        m_enabled.store(settings.enabled, std::memory_order_relaxed);
    }

    StereoParallaxSettings GetSettings() const {
        return StereoParallaxSettings{
            .eyeSeparation = m_eyeSeparation.load(std::memory_order_relaxed),
            .convergenceDepth = m_convergenceDepth.load(std::memory_order_relaxed),
            .depthScale = m_depthScale.load(std::memory_order_relaxed),
            .enabled = m_enabled.load(std::memory_order_relaxed),
        };
    }

    bool IsEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
    float GetEyeSeparation() const { return m_eyeSeparation.load(std::memory_order_relaxed); }
    float GetConvergenceDepth() const { return m_convergenceDepth.load(std::memory_order_relaxed); }

    void AdjustEyeSeparation(float delta);
    void AdjustConvergenceDepth(float delta);
    void ToggleEnabled();

#if defined(__APPLE__)
    bool GetAppleFramePair(StereoParallaxAppleFramePair& frames) const;
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

    std::atomic<float> m_eyeSeparation{0.035f};
    std::atomic<float> m_convergenceDepth{0.5f};
    std::atomic<float> m_depthScale{1.0f};
    std::atomic<bool> m_enabled{true};

#if defined(__APPLE__)
    mutable std::mutex m_appleFrameMutex;
    IOSurfaceRef m_leftColorSurface = nullptr;
    IOSurfaceRef m_rightColorSurface = nullptr;
    IOSurfaceRef m_leftDepthSurface = nullptr;
    IOSurfaceRef m_rightDepthSurface = nullptr;
    void* m_leftReadyEvent = nullptr;
    void* m_rightReadyEvent = nullptr;
    uint64_t m_leftReadyValue = 0;
    uint64_t m_rightReadyValue = 0;
    uint64_t m_readyGeneration = 0;
#endif
};

StereoParallaxPass* GetStereoParallaxPass();
void InitializeStereoParallaxHook();

} // namespace dusk::gfx
