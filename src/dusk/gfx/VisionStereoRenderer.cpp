#include "dusk/gfx/VisionStereoRenderer.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_VISION

#include "JSystem/J3DGraphBase/J3DSys.h"
#include "SSystem/SComponent/c_API_graphic.h"
#include "d/d_com_inf_game.h"
#include "dusk/frame_interpolation.h"
#include "dusk/gfx/StereoParallax.hpp"
#include "dusk/logging.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_draw.h"
#include "f_pc/f_pc_manager.h"
#include "f_op/f_op_view.h"
#include "m_Do/m_Do_mtx.h"

#include <aurora/gfx.hpp>
#include <aurora/webgpu.hpp>
#include <dolphin/mtx.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace dusk::gfx {
namespace {

constexpr float kGameUnitsPerMeter = 100.0f;
constexpr uint32_t kLeftEyeCaptureTag = 0x44534c45;  // DSLE
constexpr uint32_t kRightEyeCaptureTag = 0x44535245; // DSRE

std::atomic<const void*> s_compositorToken{nullptr};
std::atomic<const void*> s_runningCompositorToken{nullptr};

struct CameraSnapshot {
    lookat_class lookat;
    Mtx44 projection;
    Mtx view;
    Mtx inverseView;
    Mtx44 projectionView;
    Mtx viewNoTranslation;
};

void SaveCamera(const view_class& view, CameraSnapshot& snapshot) {
    snapshot.lookat = view.lookat;
    std::memcpy(snapshot.projection, view.projMtx, sizeof(Mtx44));
    std::memcpy(snapshot.view, view.viewMtx, sizeof(Mtx));
    std::memcpy(snapshot.inverseView, view.invViewMtx, sizeof(Mtx));
    std::memcpy(snapshot.projectionView, view.projViewMtx, sizeof(Mtx44));
    std::memcpy(snapshot.viewNoTranslation, view.viewMtxNoTrans, sizeof(Mtx));
}

void RestoreCamera(view_class& view, const CameraSnapshot& snapshot) {
    view.lookat = snapshot.lookat;
    std::memcpy(view.projMtx, snapshot.projection, sizeof(Mtx44));
    std::memcpy(view.viewMtx, snapshot.view, sizeof(Mtx));
    std::memcpy(view.invViewMtx, snapshot.inverseView, sizeof(Mtx));
    std::memcpy(view.projViewMtx, snapshot.projectionView, sizeof(Mtx44));
    std::memcpy(view.viewMtxNoTrans, snapshot.viewNoTranslation, sizeof(Mtx));
    j3dSys.setViewMtx(view.viewMtx);
}

void ApplyEyeCamera(view_class& view, const CameraSnapshot& centerCamera,
                    float eyeSign, float halfEyeOffset, float convergenceDistance) {
    // Row zero of the center view rotation is its world-space right vector.
    float rightX = centerCamera.view[0][0];
    float rightY = centerCamera.view[0][1];
    float rightZ = centerCamera.view[0][2];
    const float rightLength = std::sqrt(rightX * rightX + rightY * rightY + rightZ * rightZ);
    if (rightLength > 0.0001f) {
        rightX /= rightLength;
        rightY /= rightLength;
        rightZ /= rightLength;
    }

    const float cameraOffset = eyeSign * halfEyeOffset;
    const cXyz worldOffset(rightX * cameraOffset, rightY * cameraOffset, rightZ * cameraOffset);
    view.lookat = centerCamera.lookat;
    view.lookat.eye += worldOffset;
    view.lookat.center += worldOffset;

    mDoMtx_lookAt(view.viewMtx, &view.lookat.eye, &view.lookat.center, &view.lookat.up, view.bank);
    std::memcpy(view.projMtx, centerCamera.projection, sizeof(Mtx44));

    // Parallel cameras need an opposite projection shift to put the chosen
    // convergence plane at zero disparity. This avoids vertical disparity
    // and the scale/shear artifacts produced by toe-in cameras.
    if (convergenceDistance > 1.0f) {
        view.projMtx[0][2] += -view.projMtx[0][0] * cameraOffset / convergenceDistance;
    }

    cMtx_inverse(view.viewMtx, view.invViewMtx);
    MTXCopy(view.viewMtx, view.viewMtxNoTrans);
    view.viewMtxNoTrans[0][3] = 0.0f;
    view.viewMtxNoTrans[1][3] = 0.0f;
    view.viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(view.projMtx, view.viewMtx, view.projViewMtx);
    j3dSys.setViewMtx(view.viewMtx);
}

bool DrawEye(view_class& view, const CameraSnapshot& centerCamera, float eyeSign,
             float halfEyeOffset, float convergenceDistance, uint32_t width,
             uint32_t height, uint32_t captureTag,
             aurora::gfx::CapturedFrame& capture) {
    if (!aurora::gfx::begin_capture(width, height, captureTag)) {
        return false;
    }

    ApplyEyeCamera(view, centerCamera, eyeSign, halfEyeOffset, convergenceDistance);
    // Re-run camera-dependent actor drawing for each eye. Do not reset the
    // persistent lists here: simulation-tick drawing prepares scene and menu
    // packets that presentation-only actor traversal does not recreate.
    fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);
    cAPIGph_Painter();

    if (!aurora::gfx::end_capture(capture)) {
        return false;
    }
    return true;
}

} // namespace

void RegisterVisionCompositor(const void* token) {
    s_compositorToken.store(token, std::memory_order_release);
    s_runningCompositorToken.store(nullptr, std::memory_order_release);
}

void SetVisionCompositorRunning(const void* token, bool running) {
    if (running) {
        if (s_compositorToken.load(std::memory_order_acquire) == token) {
            s_runningCompositorToken.store(token, std::memory_order_release);
        }
        return;
    }

    const void* expected = token;
    s_runningCompositorToken.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool IsVisionCompositorRunning() {
    const void* current = s_compositorToken.load(std::memory_order_acquire);
    return current != nullptr &&
           s_runningCompositorToken.load(std::memory_order_acquire) == current;
}

bool RenderVisionStereoFrame() {
    auto* stereoPass = GetStereoParallaxPass();
    if (!stereoPass || !stereoPass->IsEnabled() || dComIfGp_getWindowNum() == 0) {
        return false;
    }

    dDlst_window_c* window = dComIfGp_getWindow(0);
    camera_process_class* camera = window ? dComIfGp_getCamera(window->getCameraID()) : nullptr;
    view_class* view = camera ? &camera->view : nullptr;
    if (!view) {
        return false;
    }

    const uint32_t width = aurora::webgpu::get_present_width();
    const uint32_t height = aurora::webgpu::get_present_height();
    if (width == 0 || height == 0) {
        return false;
    }

    CameraSnapshot centerCamera{};
    SaveCamera(*view, centerCamera);

    const StereoParallaxSettings settings = stereoPass->GetSettings();
    const float halfEyeOffset = settings.eyeSeparation * kGameUnitsPerMeter;
    const float convergenceDistance = 150.0f + settings.convergenceDepth * 1900.0f;

    aurora::gfx::CapturedFrame left;
    aurora::gfx::CapturedFrame right;
    aurora::gfx::set_offscreen_uses_native_logical_size(true);
    const bool leftOk = DrawEye(*view, centerCamera, -1.0f, halfEyeOffset,
                                convergenceDistance, width, height,
                                kLeftEyeCaptureTag, left);
    RestoreCamera(*view, centerCamera);

    // The first painter invocation performs the frame's menu/fade updates.
    // The right eye must render the resulting lists without advancing those
    // state machines a second time.
    dusk::frame_interp::set_ui_tick_pending(false);
    const bool rightOk = leftOk && DrawEye(*view, centerCamera, 1.0f, halfEyeOffset,
                                           convergenceDistance, width, height,
                                           kRightEyeCaptureTag, right);
    RestoreCamera(*view, centerCamera);
    aurora::gfx::set_offscreen_uses_native_logical_size(false);

    if (!leftOk || !rightOk) {
        DuskLog.warn("[DuskStereo] Dual-draw capture failed; using center-eye fallback");
        return false;
    }

    stereoPass->SubmitTrueStereoFrame(left, right);
    static bool logged = false;
    if (!logged) {
        logged = true;
        DuskLog.info("[DuskStereo] True dual-eye GX scene rendering active ({}x{})", width, height);
    }
    return true;
}

} // namespace dusk::gfx

#else

namespace dusk::gfx {
bool RenderVisionStereoFrame() {
    return false;
}

void RegisterVisionCompositor(const void*) {}
void SetVisionCompositorRunning(const void*, bool) {}
bool IsVisionCompositorRunning() {
    return true;
}
} // namespace dusk::gfx

#endif
