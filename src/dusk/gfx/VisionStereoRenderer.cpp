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
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_draw.h"
#include "f_pc/f_pc_manager.h"
#include "f_op/f_op_view.h"
#include "m_Do/m_Do_mtx.h"

#include <aurora/gfx.hpp>
#include <aurora/webgpu.hpp>
#include <dolphin/mtx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace dusk::gfx {
namespace {

constexpr float kGameUnitsPerMeter = 100.0f;
constexpr float kMaxHeadTranslationMeters = 0.3048f;
constexpr float kMaxHeadRotationRadians = 15.0f * 3.14159265358979323846f / 180.0f;
constexpr float kHeadPoseFilterTimeSeconds = 0.055f;
constexpr uint32_t kLeftEyeCaptureTag = 0x44534c45;  // DSLE
constexpr uint32_t kRightEyeCaptureTag = 0x44535245; // DSRE

std::atomic<const void*> s_compositorToken{nullptr};
std::atomic<const void*> s_runningCompositorToken{nullptr};
std::atomic<bool> s_visionAppActive{false};
std::atomic<bool> s_visionGamePaused{false};
std::mutex s_visionRunStateMutex;
std::condition_variable s_visionRunStateChanged;
thread_local float s_eyeProjectionShift = 0.0f;

std::mutex s_headPoseMutex;
VisionHeadPose s_headPose;
std::atomic<uint64_t> s_headPoseResetGeneration{0};

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Quaternion {
    float x;
    float y;
    float z;
    float w;
};

struct HeadPoseFilterState {
    VisionHeadPose pose;
    std::chrono::steady_clock::time_point lastUpdate;
    uint64_t resetGeneration = 0;
    bool initialized = false;
};

thread_local HeadPoseFilterState s_headPoseFilter;

struct CameraSnapshot {
    lookat_class lookat;
    Mtx44 projection;
    Mtx view;
    Mtx inverseView;
    Mtx44 projectionView;
    Mtx viewNoTranslation;
};

float Length(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vec3 Scale(const Vec3& value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

Vec3 Normalize(const Vec3& value, const Vec3& fallback) {
    const float length = Length(value);
    return length > 0.00001f ? Scale(value, 1.0f / length) : fallback;
}

Vec3 Cross(const Vec3& lhs, const Vec3& rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

Vec3 Rotate(const Quaternion& rotation, const Vec3& value) {
    const Vec3 imaginary{rotation.x, rotation.y, rotation.z};
    const Vec3 firstCross = Cross(imaginary, value);
    const Vec3 secondCross = Cross(imaginary, firstCross);
    return {
        value.x + 2.0f * (rotation.w * firstCross.x + secondCross.x),
        value.y + 2.0f * (rotation.w * firstCross.y + secondCross.y),
        value.z + 2.0f * (rotation.w * firstCross.z + secondCross.z),
    };
}

Vec3 MapCameraLocal(const Vec3& local, const Vec3& right, const Vec3& up,
                    const Vec3& back) {
    return {
        right.x * local.x + up.x * local.y + back.x * local.z,
        right.y * local.x + up.y * local.y + back.y * local.z,
        right.z * local.x + up.z * local.y + back.z * local.z,
    };
}

// Preserve a 1:1 response through 75% of the comfort range, then use a cubic
// Hermite shoulder to reach the hard cap with zero slope at 125% input.
float SoftLimitMagnitude(float magnitude, float limit) {
    const float shoulderStart = limit * 0.75f;
    const float shoulderEnd = limit * 1.25f;
    if (magnitude <= shoulderStart) {
        return magnitude;
    }
    if (magnitude >= shoulderEnd) {
        return limit;
    }

    const float inputRange = shoulderEnd - shoulderStart;
    const float t = (magnitude - shoulderStart) / inputRange;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    return std::clamp(h00 * shoulderStart + h10 * inputRange + h01 * limit,
                      0.0f, limit);
}

Quaternion NormalizeRotation(const VisionHeadPose& pose) {
    Quaternion rotation{pose.rotationX, pose.rotationY, pose.rotationZ, pose.rotationW};
    const float maxComponent = std::max({std::abs(rotation.x), std::abs(rotation.y),
                                         std::abs(rotation.z), std::abs(rotation.w)});
    if (!std::isfinite(maxComponent) || maxComponent <= 0.00001f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    rotation.x /= maxComponent;
    rotation.y /= maxComponent;
    rotation.z /= maxComponent;
    rotation.w /= maxComponent;
    const float scaledLength = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y +
                                         rotation.z * rotation.z + rotation.w * rotation.w);
    rotation.x /= scaledLength;
    rotation.y /= scaledLength;
    rotation.z /= scaledLength;
    rotation.w /= scaledLength;

    // q and -q are the same orientation. Use the representation with the
    // shortest arc from the anchor reference.
    if (rotation.w < 0.0f) {
        rotation.x = -rotation.x;
        rotation.y = -rotation.y;
        rotation.z = -rotation.z;
        rotation.w = -rotation.w;
    }

    return rotation;
}

Quaternion LimitedRotation(const VisionHeadPose& pose) {
    Quaternion rotation = NormalizeRotation(pose);

    rotation.w = std::clamp(rotation.w, -1.0f, 1.0f);
    const float angle = 2.0f * std::acos(rotation.w);
    if (angle <= 0.00001f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    const float limitedAngle = SoftLimitMagnitude(angle, kMaxHeadRotationRadians);
    const float sourceSin = std::sin(angle * 0.5f);
    if (sourceSin <= 0.00001f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float vectorScale = std::sin(limitedAngle * 0.5f) / sourceSin;
    return {
        rotation.x * vectorScale,
        rotation.y * vectorScale,
        rotation.z * vectorScale,
        std::cos(limitedAngle * 0.5f),
    };
}

bool ReadVisionHeadPose(VisionHeadPose& pose) {
    std::scoped_lock lock(s_headPoseMutex);
    pose = s_headPose;
    return pose.valid;
}

bool IsFinite(const VisionHeadPose& pose) {
    return std::isfinite(pose.translationX) && std::isfinite(pose.translationY) &&
           std::isfinite(pose.translationZ) && std::isfinite(pose.rotationX) &&
           std::isfinite(pose.rotationY) && std::isfinite(pose.rotationZ) &&
           std::isfinite(pose.rotationW);
}

VisionHeadPose FilterVisionHeadPose(const VisionHeadPose& target) {
    const auto now = std::chrono::steady_clock::now();
    const uint64_t resetGeneration =
        s_headPoseResetGeneration.load(std::memory_order_acquire);
    if (!s_headPoseFilter.initialized ||
        s_headPoseFilter.resetGeneration != resetGeneration) {
        s_headPoseFilter = {
            .pose = VisionHeadPose{.valid = true},
            .lastUpdate = now,
            .resetGeneration = resetGeneration,
            .initialized = true,
        };
        return s_headPoseFilter.pose;
    }

    const float elapsed = std::clamp(
        std::chrono::duration<float>(now - s_headPoseFilter.lastUpdate).count(),
        0.0f, 0.1f);
    s_headPoseFilter.lastUpdate = now;
    const float alpha = 1.0f - std::exp(-elapsed / kHeadPoseFilterTimeSeconds);

    auto lerp = [alpha](float from, float to) {
        return from + (to - from) * alpha;
    };
    s_headPoseFilter.pose.translationX =
        lerp(s_headPoseFilter.pose.translationX, target.translationX);
    s_headPoseFilter.pose.translationY =
        lerp(s_headPoseFilter.pose.translationY, target.translationY);
    s_headPoseFilter.pose.translationZ =
        lerp(s_headPoseFilter.pose.translationZ, target.translationZ);

    Quaternion from = NormalizeRotation(s_headPoseFilter.pose);
    Quaternion to = NormalizeRotation(target);
    const float dot = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
    if (dot < 0.0f) {
        to = {-to.x, -to.y, -to.z, -to.w};
    }
    VisionHeadPose blended{
        .rotationX = lerp(from.x, to.x),
        .rotationY = lerp(from.y, to.y),
        .rotationZ = lerp(from.z, to.z),
        .rotationW = lerp(from.w, to.w),
        .valid = true,
    };
    const Quaternion normalized = NormalizeRotation(blended);
    s_headPoseFilter.pose.rotationX = normalized.x;
    s_headPoseFilter.pose.rotationY = normalized.y;
    s_headPoseFilter.pose.rotationZ = normalized.z;
    s_headPoseFilter.pose.rotationW = normalized.w;
    s_headPoseFilter.pose.valid = true;
    return s_headPoseFilter.pose;
}

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
    s_eyeProjectionShift = 0.0f;
}

void ApplyLimitedHeadPose(view_class& view, const CameraSnapshot& centerCamera,
                          const VisionHeadPose& pose) {
    const Vec3 eye{
        centerCamera.lookat.eye.x,
        centerCamera.lookat.eye.y,
        centerCamera.lookat.eye.z,
    };
    const Vec3 target{
        centerCamera.lookat.center.x,
        centerCamera.lookat.center.y,
        centerCamera.lookat.center.z,
    };
    const Vec3 fallbackBack{centerCamera.view[2][0], centerCamera.view[2][1],
                            centerCamera.view[2][2]};
    const Vec3 back = Normalize(
        {eye.x - target.x, eye.y - target.y, eye.z - target.z}, fallbackBack);
    const Vec3 sourceUp = Normalize(
        {centerCamera.lookat.up.x, centerCamera.lookat.up.y, centerCamera.lookat.up.z},
        {centerCamera.view[1][0], centerCamera.view[1][1], centerCamera.view[1][2]});
    const Vec3 right = Normalize(Cross(sourceUp, back),
                                 {centerCamera.view[0][0], centerCamera.view[0][1],
                                  centerCamera.view[0][2]});
    const Vec3 up = Normalize(Cross(back, right), sourceUp);

    Vec3 localTranslation{pose.translationX, pose.translationY, pose.translationZ};
    const float translationLength = Length(localTranslation);
    if (translationLength > 0.00001f) {
        localTranslation = Scale(
            localTranslation,
            SoftLimitMagnitude(translationLength, kMaxHeadTranslationMeters) /
                translationLength);
    }
    const Vec3 worldTranslation =
        Scale(MapCameraLocal(localTranslation, right, up, back), kGameUnitsPerMeter);

    const Quaternion rotation = LimitedRotation(pose);
    Vec3 rotatedBack = MapCameraLocal(Rotate(rotation, {0.0f, 0.0f, 1.0f}),
                                      right, up, back);
    Vec3 rotatedUp = MapCameraLocal(Rotate(rotation, {0.0f, 1.0f, 0.0f}),
                                    right, up, back);
    rotatedBack = Normalize(rotatedBack, back);
    const Vec3 rotatedRight = Normalize(Cross(rotatedUp, rotatedBack), right);
    rotatedUp = Normalize(Cross(rotatedBack, rotatedRight), up);

    const float focusDistance = std::max(Length({target.x - eye.x, target.y - eye.y,
                                                  target.z - eye.z}),
                                         1.0f);
    const Vec3 trackedEye{
        eye.x + worldTranslation.x,
        eye.y + worldTranslation.y,
        eye.z + worldTranslation.z,
    };

    view.lookat = centerCamera.lookat;
    view.lookat.eye = cXyz(trackedEye.x, trackedEye.y, trackedEye.z);
    view.lookat.center = cXyz(
        trackedEye.x - rotatedBack.x * focusDistance,
        trackedEye.y - rotatedBack.y * focusDistance,
        trackedEye.z - rotatedBack.z * focusDistance);
    view.lookat.up = cXyz(rotatedUp.x, rotatedUp.y, rotatedUp.z);

    mDoMtx_lookAt(view.viewMtx, &view.lookat.eye, &view.lookat.center, &view.lookat.up, view.bank);
    std::memcpy(view.projMtx, centerCamera.projection, sizeof(Mtx44));
    cMtx_inverse(view.viewMtx, view.invViewMtx);
    MTXCopy(view.viewMtx, view.viewMtxNoTrans);
    view.viewMtxNoTrans[0][3] = 0.0f;
    view.viewMtxNoTrans[1][3] = 0.0f;
    view.viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(view.projMtx, view.viewMtx, view.projViewMtx);
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
    s_eyeProjectionShift = 0.0f;
    if (convergenceDistance > 1.0f) {
        s_eyeProjectionShift =
            -view.projMtx[0][0] * cameraOffset / convergenceDistance;
        view.projMtx[0][2] += s_eyeProjectionShift;
    }

    cMtx_inverse(view.viewMtx, view.invViewMtx);
    MTXCopy(view.viewMtx, view.viewMtxNoTrans);
    view.viewMtxNoTrans[0][3] = 0.0f;
    view.viewMtxNoTrans[1][3] = 0.0f;
    view.viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(view.projMtx, view.viewMtx, view.projViewMtx);
    j3dSys.setViewMtx(view.viewMtx);
}

void BuildEyeViewMtx(const CameraSnapshot& centerCamera, float eyeSign,
                     float halfEyeOffset, Mtx viewMtx) {
    MTXCopy(centerCamera.view, viewMtx);
    // Translating a camera along its own right axis changes only the first
    // view-space translation component and preserves authored bank exactly.
    viewMtx[0][3] -= eyeSign * halfEyeOffset;
}

bool DrawEye(view_class& view, const CameraSnapshot& centerCamera, float eyeSign,
             const CameraSnapshot& projectionCamera, float halfEyeOffset,
             float convergenceDistance, uint32_t width, uint32_t height, uint32_t captureTag,
             aurora::gfx::CapturedFrame& capture) {
    if (!aurora::gfx::begin_capture(width, height, captureTag)) {
        return false;
    }

    ApplyEyeCamera(view, centerCamera, eyeSign, halfEyeOffset, convergenceDistance);
    Mtx projectionEyeView;
    BuildEyeViewMtx(projectionCamera, eyeSign, halfEyeOffset, projectionEyeView);
    J3DSetTexProjectionViewOverride(projectionEyeView);
    // Re-run camera-dependent actor drawing for each eye. Do not reset the
    // persistent lists here: simulation-tick drawing prepares scene and menu
    // packets that presentation-only actor traversal does not recreate.
    fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);
    cAPIGph_Painter();
    J3DClearTexProjectionViewOverride();

    if (!aurora::gfx::end_capture(capture)) {
        return false;
    }
    return true;
}

} // namespace

void PublishVisionHeadPose(const VisionHeadPose& pose) {
    if (!pose.valid || !IsFinite(pose)) {
        ResetVisionHeadPose();
        return;
    }
    std::scoped_lock lock(s_headPoseMutex);
    s_headPose = pose;
}

void ResetVisionHeadPose() {
    {
        std::scoped_lock lock(s_headPoseMutex);
        s_headPose = {};
    }
    s_headPoseResetGeneration.fetch_add(1, std::memory_order_release);
}

float GetVisionStereoProjectionShift() {
    return s_eyeProjectionShift;
}

void RegisterVisionCompositor(const void* token) {
    {
        std::scoped_lock lock(s_visionRunStateMutex);
        s_compositorToken.store(token, std::memory_order_release);
        s_runningCompositorToken.store(nullptr, std::memory_order_release);
    }
    s_visionRunStateChanged.notify_all();
}

void SetVisionCompositorRunning(const void* token, bool running) {
    if (running && s_compositorToken.load(std::memory_order_acquire) == token &&
        s_runningCompositorToken.load(std::memory_order_acquire) == token) {
        return;
    }
    if (!running && s_runningCompositorToken.load(std::memory_order_acquire) != token) {
        return;
    }

    bool changed = false;
    std::scoped_lock lock(s_visionRunStateMutex);
    if (running) {
        if (s_compositorToken.load(std::memory_order_acquire) == token) {
            changed = s_runningCompositorToken.exchange(token, std::memory_order_acq_rel) != token;
        }
    } else {
        const void* expected = token;
        changed = s_runningCompositorToken.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    if (changed) {
        s_visionRunStateChanged.notify_all();
    }
}

void SetVisionAppActive(bool active) {
    {
        std::scoped_lock lock(s_visionRunStateMutex);
        if (s_visionAppActive.exchange(active, std::memory_order_acq_rel) == active) {
            return;
        }
    }
    s_visionRunStateChanged.notify_all();
}

void SetVisionGamePaused(bool paused) {
    {
        std::scoped_lock lock(s_visionRunStateMutex);
        if (s_visionGamePaused.exchange(paused, std::memory_order_acq_rel) == paused) {
            return;
        }
    }
    s_visionRunStateChanged.notify_all();
}

bool IsVisionCompositorRunning() {
    const void* current = s_compositorToken.load(std::memory_order_acquire);
    return current != nullptr &&
           s_runningCompositorToken.load(std::memory_order_acquire) == current;
}

bool IsVisionGamePaused() {
    return s_visionGamePaused.load(std::memory_order_acquire);
}

bool IsVisionGameRunnable() {
    return s_visionAppActive.load(std::memory_order_acquire) &&
           !IsVisionGamePaused() && IsVisionCompositorRunning();
}

void WaitForVisionGameResume() {
    std::unique_lock lock(s_visionRunStateMutex);
    s_visionRunStateChanged.wait(lock, [] { return IsVisionGameRunnable(); });
}

bool RenderVisionStereoFrame() {
    s_eyeProjectionShift = 0.0f;
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

    CameraSnapshot originalCamera{};
    SaveCamera(*view, originalCamera);

    // Composite wipes and loading imagery have no stable world pose. Keep both
    // stereo and head-tracked camera offsets neutral until the scene is ready.
    const bool sceneTransition = fopOvlpM_IsDoingReq() != 0;
    // GRDWATER projects the already-rendered scene back onto its surface. A
    // temporary head-pose view changes both that projection and the geometry
    // consuming it, producing a doubled camera rotation. Ease the bounded
    // head offset to neutral while the actor exists; independent eye views
    // remain enabled, so the scene retains correct stereoscopic depth.
    const bool suppressHeadTracking =
        fopAcM_SearchByName(fpcNm_GRDWATER_e) != nullptr;
    CameraSnapshot stereoCamera = originalCamera;
    VisionHeadPose headPose{};
    const bool hasHeadPose = ReadVisionHeadPose(headPose);
    if (!sceneTransition && (suppressHeadTracking || hasHeadPose)) {
        const VisionHeadPose presentationPose =
            suppressHeadTracking ? VisionHeadPose{.valid = true} : headPose;
        ApplyLimitedHeadPose(*view, originalCamera,
                             FilterVisionHeadPose(presentationPose));
        SaveCamera(*view, stereoCamera);
    } else {
        // Resume from the neutral pose after a transition or tracking gap
        // instead of applying the accumulated delta in one frame.
        s_headPoseFilter.initialized = false;
    }

    const StereoParallaxSettings settings = stereoPass->GetSettings();
    const float halfEyeOffset =
        sceneTransition ? 0.0f : settings.eyeSeparation * kGameUnitsPerMeter;
    const float convergenceDistance = 150.0f + settings.convergenceDepth * 1900.0f;

    aurora::gfx::CapturedFrame left;
    aurora::gfx::CapturedFrame right;
    aurora::gfx::set_offscreen_uses_native_logical_size(true);
    const bool leftOk = DrawEye(*view, stereoCamera, -1.0f, originalCamera, halfEyeOffset,
                                convergenceDistance, width, height,
                                kLeftEyeCaptureTag, left);
    RestoreCamera(*view, stereoCamera);

    // The first painter invocation performs the frame's menu/fade updates.
    // The right eye must render the resulting lists without advancing those
    // state machines a second time.
    dusk::frame_interp::set_ui_tick_pending(false);
    const bool rightOk = leftOk && DrawEye(*view, stereoCamera, 1.0f, originalCamera, halfEyeOffset,
                                           convergenceDistance, width, height,
                                           kRightEyeCaptureTag, right);
    RestoreCamera(*view, originalCamera);
    aurora::gfx::set_offscreen_uses_native_logical_size(false);

    static bool captureFailureLogged = false;
    if (!leftOk || !rightOk) {
        // The caller will redraw a complete center-eye frame. The post pass
        // can safely depth-warp that fallback instead of publishing a partial
        // eye capture or freezing on the previous image (which may be a fade).
        if (!captureFailureLogged) {
            captureFailureLogged = true;
            DuskLog.warn("[DuskStereo] Dual-draw capture failed; using center-eye fallback");
        }
        return false;
    }

    if (captureFailureLogged) {
        captureFailureLogged = false;
        DuskLog.info("[DuskStereo] Dual-draw capture recovered");
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

float GetVisionStereoProjectionShift() {
    return 0.0f;
}

void RegisterVisionCompositor(const void*) {}
void SetVisionCompositorRunning(const void*, bool) {}
void SetVisionAppActive(bool) {}
void SetVisionGamePaused(bool) {}
void PublishVisionHeadPose(const VisionHeadPose&) {}
void ResetVisionHeadPose() {}
bool IsVisionCompositorRunning() {
    return true;
}
bool IsVisionGamePaused() {
    return false;
}
bool IsVisionGameRunnable() {
    return true;
}
void WaitForVisionGameResume() {}
} // namespace dusk::gfx

#endif
