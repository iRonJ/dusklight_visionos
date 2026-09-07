#include "dusk/gfx/VisionWaterReflection.hpp"
#include "dusk/gfx/VisionStereoRenderer.hpp"
#include "d/d_com_inf_game.h"
#include "d/d_camera.h"
#include "f_op/f_op_camera_mng.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_wether.h"
#include "m_Do/m_Do_graphic.h"
#include "dolphin/gx/GXAurora.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dusk::gfx {

namespace {

constexpr uint32_t kWaterTexWidth = 320;
constexpr uint32_t kWaterTexHeight = 240;

std::vector<uint32_t> s_reflectionPixels;
uint32_t s_lastFrameCount = 0xFFFFFFFF;

inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

inline float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

inline uint32_t PackRGBA8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Little-endian ARM64: byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A.
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

} // namespace

void UpdateStereoWaterReflectionTexture() {
    if (!IsVisionStereoDrawing()) {
        return;
    }

    void* dest = (void*)mDoGph_gInf_c::getFrameBufferTex();
    if (dest == nullptr) {
        return;
    }

    if (s_reflectionPixels.empty()) {
        s_reflectionPixels.resize(kWaterTexWidth * kWaterTexHeight);
    }

    camera_process_class* camProc = (camera_process_class*)dComIfGp_getCamera(0);
    const uint32_t currentFrame = camProc != nullptr ? camProc->mCamera.mFrameCounter : 0;
    if (currentFrame == s_lastFrameCount && currentFrame != 0) {
        // Already updated this simulation frame; the existing GPU texture remains valid.
        return;
    }
    s_lastFrameCount = currentFrame;

    camera_class* camera_p = (camera_class*)camProc;

    // Environmental parameters
    const float shineRate = Clamp01(g_env_light.mWaterSurfaceShineRate);
    const bool isDarkWorld = (dKy_darkworld_check() == TRUE);

    const char* stage = dComIfGp_getStartStageName();
    const bool isSewer = (stage != nullptr && (std::strcmp(stage, "F_SP103") == 0 ||
                                              std::strstr(stage, "GRD") != nullptr));
    const bool isLakebed = (stage != nullptr && std::strcmp(stage, "D_MN03") == 0);

    // Color palettes
    float zenR, zenG, zenB;
    float horizR, horizG, horizB;
    float glintR, glintG, glintB;
    float baseAlpha, horizonAlpha;

    if (isDarkWorld) {
        // Dark World / Twilight Realm: eerie twilight bronze horizon, dark purple zenith, golden glint
        zenR = 38.0f;   zenG = 16.0f;  zenB = 58.0f;
        horizR = 172.0f; horizG = 122.0f; horizB = 48.0f;
        glintR = 255.0f; glintG = 218.0f; glintB = 120.0f;
        baseAlpha = 0.22f;
        horizonAlpha = 0.40f;
    } else if (isSewer) {
        // Subterranean sewer groundwater: damp mossy stone tones, subtle specular sheen
        zenR = 26.0f;   zenG = 34.0f;  zenB = 30.0f;
        horizR = 42.0f;  horizG = 54.0f;  horizB = 46.0f;
        glintR = 190.0f; glintG = 210.0f; glintB = 200.0f;
        baseAlpha = 0.18f;
        horizonAlpha = 0.30f;
    } else if (isLakebed) {
        // Lakebed Temple: crystalline subterranean pool, crisp aquamarine tones
        zenR = 24.0f;   zenG = 65.0f;  zenB = 110.0f;
        horizR = 48.0f;  horizG = 120.0f; horizB = 175.0f;
        glintR = 195.0f; glintG = 240.0f; glintB = 255.0f;
        baseAlpha = 0.22f;
        horizonAlpha = 0.38f;
    } else {
        // Lake Hylia / outdoor water bodies: dynamic sky gradient modulated by ambient lighting
        const float ambR = std::clamp(static_cast<float>(g_env_light.bg_amb_col[1].r), 0.0f, 255.0f);
        const float ambG = std::clamp(static_cast<float>(g_env_light.bg_amb_col[1].g), 0.0f, 255.0f);
        const float ambB = std::clamp(static_cast<float>(g_env_light.bg_amb_col[1].b), 0.0f, 255.0f);

        // Blend ambient water tint with sky zenith and horizon
        zenR = Lerp(50.0f, ambR, 0.45f);
        zenG = Lerp(120.0f, ambG, 0.45f);
        zenB = Lerp(210.0f, ambB, 0.45f);

        horizR = Lerp(185.0f, ambR, 0.35f);
        horizG = Lerp(215.0f, ambG, 0.35f);
        horizB = Lerp(245.0f, ambB, 0.35f);

        glintR = 255.0f; glintG = 255.0f; glintB = 242.0f;
        baseAlpha = 0.18f;
        horizonAlpha = 0.38f;
    }

    // Calculate sun azimuth relative to camera view
    float sunU = 0.5f;
    float sunFacing = 0.5f;

    if (camera_p != nullptr && g_env_light.mSunInitialized &&
        g_env_light.mpSunPacket != nullptr && !isDarkWorld && !isSewer)
    {
        const cXyz& sunPos = g_env_light.mpSunPacket->mPos[0];
        const cXyz& camPos = camera_p->view.lookat.eye;
        const cXyz& camTarget = camera_p->view.lookat.center;

        const float sunDx = sunPos.x - camPos.x;
        const float sunDz = sunPos.z - camPos.z;
        const float camDx = camTarget.x - camPos.x;
        const float camDz = camTarget.z - camPos.z;

        const float sunLen = std::sqrt(sunDx * sunDx + sunDz * sunDz);
        const float camLen = std::sqrt(camDx * camDx + camDz * camDz);

        if (sunLen > 1.0f && camLen > 1.0f) {
            const float sX = sunDx / sunLen;
            const float sZ = sunDz / sunLen;
            const float cX = camDx / camLen;
            const float cZ = camDz / camLen;

            const float dotFwd = cX * sX + cZ * sZ;
            const float crossY = cZ * sX - cX * sZ;

            if (dotFwd > -0.3f) {
                sunU = std::clamp(0.5f + 0.5f * crossY, 0.1f, 0.9f);
                sunFacing = std::clamp((dotFwd + 0.3f) / 1.3f, 0.0f, 1.0f);
            } else {
                sunFacing = 0.0f;
            }
        }
    }

    // Generate 320x240 reflection texture
    for (uint32_t y = 0; y < kWaterTexHeight; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(kWaterTexHeight - 1);
        // v = 0 is steep downward view, v = 1 is glancing horizon view
        const float skyR = Lerp(zenR, horizR, v);
        const float skyG = Lerp(zenG, horizG, v);
        const float skyB = Lerp(zenB, horizB, v);
        const float fresnel = Lerp(baseAlpha, horizonAlpha, v * v);

        for (uint32_t x = 0; x < kWaterTexWidth; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kWaterTexWidth - 1);

            // Specular highlight bloom column
            const float du = std::abs(u - sunU);
            constexpr float kSpecSpread = 0.14f;
            const float specBloom = std::exp(-(du * du) / (2.0f * kSpecSpread * kSpecSpread)) * sunFacing;

            // Micro-ripple wave peak modulation
            const float wavePhase = u * 48.0f + v * 32.0f;
            const float rippleWave = 0.78f + 0.22f * std::sin(wavePhase);
            const float specGlint = specBloom * shineRate * rippleWave;

            // Final color and alpha
            const float r = std::clamp(skyR + (glintR - skyR) * specGlint, 0.0f, 255.0f);
            const float g = std::clamp(skyG + (glintG - skyG) * specGlint, 0.0f, 255.0f);
            const float b = std::clamp(skyB + (glintB - skyB) * specGlint, 0.0f, 255.0f);
            const float a = std::clamp(fresnel + specGlint * 0.42f, 0.12f, 0.72f);

            s_reflectionPixels[y * kWaterTexWidth + x] = PackRGBA8(
                static_cast<uint8_t>(r + 0.5f),
                static_cast<uint8_t>(g + 0.5f),
                static_cast<uint8_t>(b + 0.5f),
                static_cast<uint8_t>(a * 255.0f + 0.5f)
            );
        }
    }

    // Register with Aurora as a linear RGBA8 texture for fbtex_dummy
    GXSetCustomCopyTextureRGBA8(dest, kWaterTexWidth, kWaterTexHeight, s_reflectionPixels.data());
}

} // namespace dusk::gfx
