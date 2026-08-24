#include "dusk/ios/VisionCompositorRenderer.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#import <CompositorServices/CompositorServices.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <os/log.h>
#import <simd/simd.h>

#include <aurora/webgpu.hpp>

#include "dusk/gfx/StereoParallax.hpp"
#include "dusk/gfx/VisionStereoRenderer.hpp"
#include "dusk/ios/VisionDioramaAnchor.h"
#include "dusk/ios/VisionDioramaShaders.h"

#include <atomic>
#include <chrono>
#include <thread>

using dusk::vision::kDioramaIndices;
using dusk::vision::kDioramaShaderSource;
using dusk::vision::kDioramaVertices;
using dusk::vision::MatrixRotationX;
using dusk::vision::MatrixRotationY;
using dusk::vision::MatrixScale;
using dusk::vision::MatrixTranslation;

namespace {
constexpr uint64_t kDioramaTrackingAreaIdentifier = 1;
constexpr float kDioramaBaseWidth = 1.6f;
constexpr float kDioramaBaseHeight = 0.9f;
constexpr float kDioramaBaseDistance = 1.5f;

std::atomic<float> g_dioramaX{0.0f};
std::atomic<float> g_dioramaY{0.0f};
std::atomic<float> g_dioramaDistance{kDioramaBaseDistance};
std::atomic<float> g_dioramaWidth{kDioramaBaseWidth};
std::atomic<float> g_dioramaAspect{16.0f / 9.0f};
std::atomic<uint64_t> g_dioramaPlacementSequence{0};
std::atomic<uint64_t> g_dioramaRecenterGeneration{0};
DusklightDioramaAnchor* g_sharedDioramaAnchor = nil;
simd_float4x4 g_dioramaAnchorReference = matrix_identity_float4x4;
std::atomic<bool> g_hasDioramaAnchorReference{false};

struct DioramaPlacement {
    float x;
    float y;
    float distance;
    float width;
    float aspect;
};

DioramaPlacement CurrentDioramaPlacement() {
    for (;;) {
        const uint64_t before = g_dioramaPlacementSequence.load(std::memory_order_acquire);
        if ((before & 1) != 0) {
            continue;
        }

        DioramaPlacement placement{
            .x = g_dioramaX.load(std::memory_order_relaxed),
            .y = g_dioramaY.load(std::memory_order_relaxed),
            .distance = g_dioramaDistance.load(std::memory_order_relaxed),
            .width = g_dioramaWidth.load(std::memory_order_relaxed),
            .aspect = g_dioramaAspect.load(std::memory_order_relaxed),
        };
        const uint64_t after = g_dioramaPlacementSequence.load(std::memory_order_acquire);
        if (before == after) {
            return placement;
        }
    }
}
} // namespace

@interface DusklightVisionRendererHost : NSObject

- (instancetype)initWithLayerRenderer:(cp_layer_renderer_t)layerRenderer;
- (void)start;
- (void)requestStop;
- (void)stop;
- (BOOL)isRunning;
- (BOOL)usesLayerRenderer:(cp_layer_renderer_t)layerRenderer;

@end

@implementation DusklightVisionRendererHost {
    cp_layer_renderer_t _layerRenderer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    id<MTLRenderPipelineState> _pipelineState;
    id<MTLRenderPipelineState> _interactivePipelineState;
    id<MTLDepthStencilState> _depthState;
    id<MTLSamplerState> _samplerState;
    id<MTLBuffer> _vertexBuffer;
    id<MTLBuffer> _indexBuffer;
    id<MTLTexture> _diagnosticTexture;

    // Cached Metal texture wrappers for the StereoParallax IOSurfaces
    id<MTLTexture> _cachedLeftTexture;
    id<MTLTexture> _cachedRightTexture;
    IOSurfaceRef _cachedLeftSurface;
    IOSurfaceRef _cachedRightSurface;
    uint32_t _cachedWidth;
    uint32_t _cachedHeight;

    // Pipeline format tracking - recreate if drawable format changes
    MTLPixelFormat _pipelineColorFormat;
    MTLPixelFormat _pipelineTrackingFormat;

    DusklightDioramaAnchor* _anchor;
    std::thread _renderThread;
    std::atomic<bool> _running;

    BOOL _hasAnchoredInitialPosition;
    BOOL _loggedDiagnosticSource;
    BOOL _loggedGameSource;
    simd_float4x4 _anchorReferenceMatrix;
    uint64_t _lastRecenterGeneration;
}

- (instancetype)initWithLayerRenderer:(cp_layer_renderer_t)layerRenderer {
    self = [super init];
    if (self) {
        _layerRenderer = layerRenderer;
        _device = cp_layer_renderer_get_device(layerRenderer);
        _commandQueue = [_device newCommandQueue];
        _commandQueue.label = @"Dusklight VisionCompositor Queue";
        _running.store(false);
        _pipelineColorFormat = MTLPixelFormatInvalid;
        _pipelineTrackingFormat = MTLPixelFormatInvalid;
        _hasAnchoredInitialPosition =
            g_hasDioramaAnchorReference.load(std::memory_order_acquire);
        _loggedDiagnosticSource = NO;
        _loggedGameSource = NO;
        _anchorReferenceMatrix = _hasAnchoredInitialPosition
            ? g_dioramaAnchorReference
            : matrix_identity_float4x4;
        _lastRecenterGeneration = g_dioramaRecenterGeneration.load(std::memory_order_acquire);

        [self setupGeometry];
        [self setupDiagnosticTexture];
        if (!g_sharedDioramaAnchor) {
            g_sharedDioramaAnchor = [[DusklightDioramaAnchor alloc] init];
        }
        _anchor = g_sharedDioramaAnchor;
    }
    return self;
}

- (void)setupPipelineForColorFormat:(MTLPixelFormat)colorFormat
                        depthFormat:(MTLPixelFormat)depthFormat
                     trackingFormat:(MTLPixelFormat)trackingFormat {
    if (_pipelineColorFormat == colorFormat && _pipelineTrackingFormat == trackingFormat &&
        _pipelineState != nil) {
        return; // Already created for this format
    }

    os_log(OS_LOG_DEFAULT, "[Dusklight] Creating Metal pipeline for colorFormat=%lu depthFormat=%lu",
           (unsigned long)colorFormat, (unsigned long)depthFormat);

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kDioramaShaderSource];
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> library = [_device newLibraryWithSource:source options:options error:&error];
    if (!library) {
        NSLog(@"[Dusklight] Failed to compile diorama Metal library: %@", error);
        return;
    }

    id<MTLFunction> vertFunc = [library newFunctionWithName:@"diorama_vertex_main"];
    id<MTLFunction> fragFunc = [library newFunctionWithName:@"diorama_fragment_main"];
    id<MTLFunction> interactiveFragFunc =
        [library newFunctionWithName:@"diorama_interactive_fragment_main"];

    MTLVertexDescriptor* vertDesc = [MTLVertexDescriptor vertexDescriptor];
    vertDesc.attributes[0].format = MTLVertexFormatFloat3;
    vertDesc.attributes[0].offset = offsetof(dusk::vision::DioramaVertex, position);
    vertDesc.attributes[0].bufferIndex = 0;

    vertDesc.attributes[1].format = MTLVertexFormatFloat2;
    vertDesc.attributes[1].offset = offsetof(dusk::vision::DioramaVertex, uv);
    vertDesc.attributes[1].bufferIndex = 0;

    vertDesc.layouts[0].stride = sizeof(dusk::vision::DioramaVertex);
    vertDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pld = [[MTLRenderPipelineDescriptor alloc] init];
    pld.label = @"Dusklight Diorama Pipeline";
    pld.vertexFunction = vertFunc;
    pld.fragmentFunction = fragFunc;
    pld.vertexDescriptor = vertDesc;
    pld.colorAttachments[0].pixelFormat = colorFormat;
    pld.colorAttachments[0].blendingEnabled = YES;
    pld.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pld.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pld.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pld.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pld.depthAttachmentPixelFormat = depthFormat;

    _pipelineState = [_device newRenderPipelineStateWithDescriptor:pld error:&error];
    if (!_pipelineState) {
        NSLog(@"[Dusklight] Failed to create diorama render pipeline: %@", error);
        return;
    }

    _interactivePipelineState = nil;
    if (trackingFormat != MTLPixelFormatInvalid) {
        pld.label = @"Dusklight Interactive Diorama Pipeline";
        pld.fragmentFunction = interactiveFragFunc;
        pld.colorAttachments[1].pixelFormat = trackingFormat;
        _interactivePipelineState = [_device newRenderPipelineStateWithDescriptor:pld error:&error];
        if (!_interactivePipelineState) {
            NSLog(@"[Dusklight] Failed to create diorama tracking pipeline: %@", error);
        }
    }

    MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
    // CompositorServices uses reversed Z: clear to the far value and keep the
    // closest fragment so the fallback room and compositor reprojection both
    // receive meaningful depth.
    dsd.depthCompareFunction = MTLCompareFunctionGreaterEqual;
    dsd.depthWriteEnabled = YES;
    _depthState = [_device newDepthStencilStateWithDescriptor:dsd];

    MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
    sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _samplerState = [_device newSamplerStateWithDescriptor:sampDesc];

    _pipelineColorFormat = colorFormat;
    _pipelineTrackingFormat = trackingFormat;
}

- (void)setupGeometry {
    _vertexBuffer = [_device newBufferWithBytes:kDioramaVertices
                                         length:sizeof(kDioramaVertices)
                                        options:MTLResourceStorageModeShared];
    _indexBuffer = [_device newBufferWithBytes:kDioramaIndices
                                        length:sizeof(kDioramaIndices)
                                       options:MTLResourceStorageModeShared];
}

- (void)setupDiagnosticTexture {
    static constexpr NSUInteger kSize = 64;
    uint32_t pixels[kSize * kSize];
    for (NSUInteger y = 0; y < kSize; ++y) {
        for (NSUInteger x = 0; x < kSize; ++x) {
            const BOOL border = x < 3 || y < 3 || x >= kSize - 3 || y >= kSize - 3;
            const BOOL alternate = ((x / 8) + (y / 8)) % 2 != 0;
            // BGRA8 in native little-endian byte order: white border with a
            // high-contrast cyan/magenta checkerboard.
            pixels[y * kSize + x] = border ? 0xffffffffu
                                           : (alternate ? 0xffff00ffu : 0xff00ffffu);
        }
    }

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                           width:kSize
                                                          height:kSize
                                                       mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    _diagnosticTexture = [_device newTextureWithDescriptor:desc];
    [_diagnosticTexture replaceRegion:MTLRegionMake2D(0, 0, kSize, kSize)
                          mipmapLevel:0
                            withBytes:pixels
                          bytesPerRow:kSize * sizeof(uint32_t)];
    _diagnosticTexture.label = @"Dusklight Compositor Diagnostic";
}

- (void)start {
    if (_running.load()) return;
    if (_renderThread.joinable()) {
        _renderThread.join();
    }
    _running.store(true);
    _renderThread = std::thread([self]() {
        [self renderLoop];
    });
}

- (void)requestStop {
    _running.store(false);
}

- (void)stop {
    [self requestStop];
    if (_renderThread.joinable()) {
        _renderThread.join();
    }
}

- (BOOL)isRunning {
    return _running.load();
}

- (BOOL)usesLayerRenderer:(cp_layer_renderer_t)layerRenderer {
    return _layerRenderer == layerRenderer;
}

- (void)renderLoop {
    pthread_setname_np("Dusklight.VisionRenderThread");
    const void* compositorToken = (__bridge const void*)_layerRenderer;
    dusk::gfx::SetVisionCompositorRunning(compositorToken, false);

    while (_running.load() && !_anchor.isTracking) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (_running.load()) {
        os_log(OS_LOG_DEFAULT, "[Dusklight] ARKit world tracking is running; starting compositor frames");
    }

    while (_running.load()) {
        cp_layer_renderer_state state = cp_layer_renderer_get_state(_layerRenderer);
        if (state == cp_layer_renderer_state_invalidated) {
            dusk::gfx::SetVisionCompositorRunning(compositorToken, false);
            break;
        } else if (state == cp_layer_renderer_state_paused) {
            dusk::gfx::SetVisionCompositorRunning(compositorToken, false);
            // Poll instead of blocking in cp_layer_renderer_wait_until_running().
            // A paused layer can be replaced during a system interruption, and
            // the owner must be able to stop and join this thread first.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        dusk::gfx::SetVisionCompositorRunning(compositorToken, true);

        @autoreleasepool {
            BOOL hadFrame = [self renderFrame];
            if (!hadFrame) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    _running.store(false);
    dusk::gfx::SetVisionCompositorRunning(compositorToken, false);
    os_log(OS_LOG_DEFAULT, "[Dusklight] VisionCompositorRenderer frame loop stopped");
}

- (id<MTLTexture>)metalTextureForIOSurface:(IOSurfaceRef)surface
                                     width:(uint32_t)width
                                    height:(uint32_t)height
                                    cached:(id<MTLTexture> __strong*)cachedTex
                             cachedSurface:(IOSurfaceRef*)cachedSurf {
    // Reuse cached texture if IOSurface pointer and dimensions match
    if (*cachedTex && *cachedSurf == surface && _cachedWidth == width && _cachedHeight == height) {
        return *cachedTex;
    }

    // The game renders sRGB-encoded color into a BGRA8 IOSurface; wrap it as
    // an sRGB texture so sampling decodes to linear and the sRGB drawable
    // re-encodes on store (avoids double gamma).
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;

    *cachedTex = [_device newTextureWithDescriptor:desc iosurface:surface plane:0];
    *cachedSurf = surface;
    return *cachedTex;
}

// Encode a clear+present so the compositor always receives a completed frame,
// even before the game engine has produced stereo content.
- (void)presentClearedDrawable:(cp_drawable_t)drawable {
    id<MTLCommandBuffer> cmdBuffer = [_commandQueue commandBuffer];
    cmdBuffer.label = @"VisionCompositor Clear";

    size_t textureCount = cp_drawable_get_texture_count(drawable);
    size_t trackingTextureCount = 0;
    if (@available(visionOS 26.0, *)) {
        trackingTextureCount = cp_drawable_get_tracking_areas_texture_count(drawable);
    }
    for (size_t texIdx = 0; texIdx < textureCount; ++texIdx) {
        id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texIdx);
        if (!color) continue;
        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture = color;
        passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        if (color.textureType == MTLTextureType2DArray) {
            passDesc.renderTargetArrayLength = color.arrayLength;
        }
        if (@available(visionOS 26.0, *)) {
            if (texIdx < trackingTextureCount) {
                id<MTLTexture> tracking =
                    cp_drawable_get_tracking_areas_texture(drawable, texIdx);
                if (tracking) {
                    passDesc.colorAttachments[1].texture = tracking;
                    passDesc.colorAttachments[1].loadAction = MTLLoadActionClear;
                    passDesc.colorAttachments[1].storeAction = MTLStoreActionStore;
                    passDesc.colorAttachments[1].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
                }
            }
        }
        id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texIdx);
        if (depth) {
            passDesc.depthAttachment.texture = depth;
            passDesc.depthAttachment.loadAction = MTLLoadActionClear;
            passDesc.depthAttachment.storeAction = MTLStoreActionStore;
            passDesc.depthAttachment.clearDepth = 0.0;
        }
        id<MTLRenderCommandEncoder> encoder = [cmdBuffer renderCommandEncoderWithDescriptor:passDesc];
        [encoder endEncoding];
    }

    cp_drawable_encode_present(drawable, cmdBuffer);
    [cmdBuffer commit];
}

- (BOOL)renderFrame {
    cp_frame_t frame = cp_layer_renderer_query_next_frame(_layerRenderer);
    if (!frame) return NO;

    cp_frame_start_update(frame);
    cp_frame_end_update(frame);

    cp_frame_timing_t timing = cp_frame_predict_timing(frame);
    if (!timing) return NO;

    cp_time_t optimalInputTime = cp_frame_timing_get_optimal_input_time(timing);
    cp_time_wait_until(optimalInputTime);

    // visionOS 26 returns an array because a frame may contain separate
    // headset and capture targets. Query it before starting submission, as
    // required by the current Compositor Services frame contract.
    cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
    if (!drawables) return NO;
    const size_t drawableCount = cp_drawable_array_get_count(drawables);
    if (drawableCount == 0) return NO;

    cp_drawable_t drawable = cp_drawable_array_get_drawable(drawables, 0);
    for (size_t index = 0; index < drawableCount; ++index) {
        cp_drawable_t candidate = cp_drawable_array_get_drawable(drawables, index);
        if (candidate && cp_drawable_get_target(candidate) == cp_drawable_target_built_in) {
            drawable = candidate;
            break;
        }
    }
    if (!drawable) return NO;

    cp_frame_start_submission(frame);

    // Query head pose from ARKit at the predicted presentation timestamp
    simd_float4x4 originFromDevice = matrix_identity_float4x4;
    BOOL hasPose = NO;
    cp_time_t presentationTime = cp_frame_timing_get_presentation_time(timing);
    CFTimeInterval presentationSeconds = cp_time_to_cf_time_interval(presentationTime);
    ar_device_anchor_t deviceAnchor = [_anchor queryDeviceAnchorAtTime:presentationSeconds
                                                      originFromDevice:&originFromDevice];
    if (deviceAnchor) {
        hasPose = YES;
        cp_drawable_set_device_anchor(drawable, deviceAnchor);

        const uint64_t recenterGeneration =
            g_dioramaRecenterGeneration.load(std::memory_order_acquire);
        if (recenterGeneration != _lastRecenterGeneration) {
            _lastRecenterGeneration = recenterGeneration;
            _hasAnchoredInitialPosition = NO;
        }

        if (!_hasAnchoredInitialPosition) {
            // Preserve the initial head orientation as well as position so the
            // flat diorama is perpendicular to the user's gaze, not world axes.
            _anchorReferenceMatrix = originFromDevice;
            g_dioramaAnchorReference = _anchorReferenceMatrix;
            g_hasDioramaAnchorReference.store(true, std::memory_order_release);
            simd_float4x4 initialModel =
                simd_mul(_anchorReferenceMatrix,
                         MatrixTranslation(0.0f, 0.0f, -kDioramaBaseDistance));
            simd_float4 targetPos = initialModel.columns[3];
            _hasAnchoredInitialPosition = YES;
            os_log(OS_LOG_DEFAULT, "[Dusklight] Anchored 3D diorama window in world space at (%.2f, %.2f, %.2f)",
                   targetPos.x, targetPos.y, targetPos.z);
        }
    }

    if (!hasPose) {
        // Once submission has started, xrOS requires the queried drawable to
        // be presented. Tracking commonly disappears briefly while Home View
        // or a system message interrupts the immersive space.
        [self presentClearedDrawable:drawable];
        cp_frame_end_submission(frame);
        return YES;
    }

    // CompositorServices consumes reverse-Z depth (far, near distances).
    cp_drawable_set_depth_range(drawable, simd_make_float2(100.0f, 0.1f));

    // Get a coherent IOSurface + Dawn completion-fence snapshot for each eye.
    auto* stereoPass = dusk::gfx::GetStereoParallaxPass();
    dusk::gfx::StereoParallaxAppleFramePair stereoFrames{};
    const bool hasStereoFrame = stereoPass && stereoPass->GetAppleFramePair(stereoFrames);
    const auto& leftFrame = stereoFrames.left;
    const auto& rightFrame = stereoFrames.right;
    IOSurfaceRef leftSurf = hasStereoFrame ? leftFrame.colorSurface : nullptr;
    IOSurfaceRef rightSurf = hasStereoFrame ? rightFrame.colorSurface : nullptr;
    uint32_t srcWidth = stereoPass ? stereoPass->GetWidth() : 0;
    uint32_t srcHeight = stereoPass ? stereoPass->GetHeight() : 0;

    size_t viewCount = cp_drawable_get_view_count(drawable);
    size_t textureCount = cp_drawable_get_texture_count(drawable);
    size_t trackingTextureCount = 0;
    uint16_t trackingRenderValue = 0;
    if (@available(visionOS 26.0, *)) {
        trackingTextureCount = cp_drawable_get_tracking_areas_texture_count(drawable);
        if (trackingTextureCount > 0) {
            cp_tracking_area_t trackingArea =
                cp_drawable_add_tracking_area(
                    drawable,
                    static_cast<cp_tracking_area_identifier>(kDioramaTrackingAreaIdentifier));
            if (trackingArea) {
                trackingRenderValue = cp_tracking_area_get_render_value(trackingArea);
                cp_tracking_area_add_automatic_hover_effect(trackingArea);
            }
        }
    }

    static bool s_loggedDrawableConfiguration = false;
    if (!s_loggedDrawableConfiguration) {
        s_loggedDrawableConfiguration = true;
        id<MTLTexture> sampleColor = cp_drawable_get_color_texture(drawable, 0);
        os_log(OS_LOG_DEFAULT,
               "[Dusklight] VisionCompositor: views=%zu textures=%zu tracked=%d "
               "drawableColorFormat=%lu drawableSize=(%lux%lu) srcSurfaces=(%p,%p) srcSize=(%ux%u)",
               viewCount, textureCount, (int)hasPose,
               (unsigned long)sampleColor.pixelFormat,
               (unsigned long)sampleColor.width, (unsigned long)sampleColor.height,
               leftSurf, rightSurf, srcWidth, srcHeight);
    }

    // Lazily create/recreate pipeline to match the drawable's actual formats.
    {
        id<MTLTexture> drawableColor = cp_drawable_get_color_texture(drawable, 0);
        id<MTLTexture> drawableDepth = cp_drawable_get_depth_texture(drawable, 0);
        id<MTLTexture> drawableTracking = nil;
        if (@available(visionOS 26.0, *)) {
            if (trackingTextureCount > 0) {
                drawableTracking = cp_drawable_get_tracking_areas_texture(drawable, 0);
            }
        }
        MTLPixelFormat colorFmt = drawableColor ? drawableColor.pixelFormat : MTLPixelFormatBGRA8Unorm_sRGB;
        MTLPixelFormat depthFmt = drawableDepth ? drawableDepth.pixelFormat : MTLPixelFormatDepth32Float;
        MTLPixelFormat trackingFmt = drawableTracking ? drawableTracking.pixelFormat : MTLPixelFormatInvalid;
        [self setupPipelineForColorFormat:colorFmt depthFormat:depthFmt trackingFormat:trackingFmt];
    }

    // Wrap the stereo IOSurfaces as Metal textures (zero-copy)
    id<MTLTexture> srcLeftCol = nil;
    id<MTLTexture> srcRightCol = nil;
    if (leftSurf && rightSurf && srcWidth > 0 && srcHeight > 0) {
        srcLeftCol = [self metalTextureForIOSurface:leftSurf width:srcWidth height:srcHeight
                                             cached:&_cachedLeftTexture cachedSurface:&_cachedLeftSurface];
        srcRightCol = [self metalTextureForIOSurface:rightSurf width:srcWidth height:srcHeight
                                              cached:&_cachedRightTexture cachedSurface:&_cachedRightSurface];
        _cachedWidth = srcWidth;
        _cachedHeight = srcHeight;
    }

    if (!_pipelineState) {
        [self presentClearedDrawable:drawable];
        cp_frame_end_submission(frame);
        return NO;
    }


    const BOOL hasGameSurfaces = srcLeftCol != nil && srcRightCol != nil;
    if (!hasGameSurfaces) {
        srcLeftCol = _diagnosticTexture;
        srcRightCol = _diagnosticTexture;
        if (!_loggedDiagnosticSource) {
            _loggedDiagnosticSource = YES;
            os_log(OS_LOG_DEFAULT,
                   "[Dusklight] Presenting compositor diagnostic checkerboard while game surfaces are unavailable");
        }
    } else if (!_loggedGameSource) {
        _loggedGameSource = YES;
        os_log(OS_LOG_DEFAULT, "[Dusklight] Stereo game IOSurfaces are ready; presenting engine output");
    }

    id<MTLCommandBuffer> cmdBuffer = [_commandQueue commandBuffer];
    cmdBuffer.label = @"VisionCompositor Diorama";

    // Dawn's EndAccess fence is the ownership handoff. Waiting on Metal's GPU
    // timeline avoids touching Dawn's queue from this compositor thread.
    if (hasGameSurfaces) {
        [cmdBuffer encodeWaitForEvent:(__bridge id<MTLSharedEvent>)leftFrame.readyEvent
                                value:leftFrame.readyValue];
        [cmdBuffer encodeWaitForEvent:(__bridge id<MTLSharedEvent>)rightFrame.readyEvent
                                value:rightFrame.readyValue];
    }

    const DioramaPlacement placement = CurrentDioramaPlacement();
    const float width = fmaxf(0.4f, placement.width);
    const float aspect = fmaxf(1.0f, placement.aspect);
    const float height = width / aspect;
    const float distance = fminf(fmaxf(placement.distance, 0.75f), 5.0f);
    const simd_float4x4 localPlacement = simd_mul(
        MatrixTranslation(placement.x, placement.y, -distance),
        MatrixScale(width / kDioramaBaseWidth, height / kDioramaBaseHeight, 1.0f));
    const simd_float4x4 modelMatrix = simd_mul(_anchorReferenceMatrix, localPlacement);

    bool isDedicated = (textureCount == viewCount && viewCount > 1);
    bool isLayered = (textureCount == 1 && viewCount > 1);
    for (size_t viewIdx = 0; viewIdx < viewCount; ++viewIdx) {
        cp_view_t view = cp_drawable_get_view(drawable, viewIdx);
        id<MTLTexture> srcColor = (viewIdx == 0) ? srcLeftCol : srcRightCol;

        // Determine which texture and slice to render into
        id<MTLTexture> dstColor = nil;
        id<MTLTexture> dstDepth = nil;
        id<MTLTexture> dstTracking = nil;
        NSUInteger dstSlice = 0;

        if (isDedicated) {
            // Dedicated: separate texture per eye
            dstColor = cp_drawable_get_color_texture(drawable, viewIdx);
            dstDepth = cp_drawable_get_depth_texture(drawable, viewIdx);
            if (@available(visionOS 26.0, *)) {
                if (viewIdx < trackingTextureCount) {
                    dstTracking = cp_drawable_get_tracking_areas_texture(drawable, viewIdx);
                }
            }
        } else if (isLayered) {
            // Layered: single array texture, each eye is a slice
            dstColor = cp_drawable_get_color_texture(drawable, 0);
            dstDepth = cp_drawable_get_depth_texture(drawable, 0);
            if (@available(visionOS 26.0, *)) {
                if (trackingTextureCount > 0) {
                    dstTracking = cp_drawable_get_tracking_areas_texture(drawable, 0);
                }
            }
            dstSlice = viewIdx;
        } else {
            // Single view fallback
            dstColor = cp_drawable_get_color_texture(drawable, 0);
            dstDepth = cp_drawable_get_depth_texture(drawable, 0);
            if (@available(visionOS 26.0, *)) {
                if (trackingTextureCount > 0) {
                    dstTracking = cp_drawable_get_tracking_areas_texture(drawable, 0);
                }
            }
        }

        if (!dstColor) continue;

        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture = dstColor;
        passDesc.colorAttachments[0].slice = dstSlice;
        passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        // Transparent clear for passthrough — alpha=0 where we don't draw
        passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

        if (dstTracking && _interactivePipelineState && trackingRenderValue != 0) {
            passDesc.colorAttachments[1].texture = dstTracking;
            passDesc.colorAttachments[1].slice = dstSlice;
            passDesc.colorAttachments[1].loadAction = MTLLoadActionClear;
            passDesc.colorAttachments[1].storeAction = MTLStoreActionStore;
            passDesc.colorAttachments[1].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        } else {
            dstTracking = nil;
        }

        if (dstDepth) {
            passDesc.depthAttachment.texture = dstDepth;
            passDesc.depthAttachment.slice = dstSlice;
            passDesc.depthAttachment.loadAction = MTLLoadActionClear;
            passDesc.depthAttachment.storeAction = MTLStoreActionStore;
            passDesc.depthAttachment.clearDepth = 0.0;
        }

        // Each individual render pass targets a single slice, so array length is 1
        passDesc.renderTargetArrayLength = 1;

        id<MTLRenderCommandEncoder> encoder = [cmdBuffer renderCommandEncoderWithDescriptor:passDesc];
        if (!encoder) continue;
        encoder.label = viewIdx == 0 ? @"Dusklight Diorama Left Eye"
                                     : @"Dusklight Diorama Right Eye";

        cp_view_texture_map_t texMap = cp_view_get_view_texture_map(view);
        MTLViewport vp = cp_view_texture_map_get_viewport(texMap);
        [encoder setViewport:vp];
        [encoder setCullMode:MTLCullModeNone];

        // Per-eye camera: world -> device (tracked head) -> eye
        simd_float4x4 eyeToWorld = simd_mul(originFromDevice, cp_view_get_transform(view));
        simd_float4x4 viewMatrix = simd_inverse(eyeToWorld);
        simd_float4x4 projMatrix =
            cp_drawable_compute_projection(drawable, cp_axis_direction_convention_right_up_back, viewIdx);
        [encoder setRenderPipelineState:dstTracking ? _interactivePipelineState : _pipelineState];
        [encoder setDepthStencilState:_depthState];
        [encoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
        [encoder setFragmentTexture:srcColor atIndex:0];
        [encoder setFragmentSamplerState:_samplerState atIndex:0];
        if (dstTracking) {
            [encoder setFragmentBytes:&trackingRenderValue
                               length:sizeof(trackingRenderValue)
                              atIndex:0];
        }

        auto drawQuad = [&](simd_float4x4 localMatrix) {
            const simd_float4x4 worldMatrix = simd_mul(modelMatrix, localMatrix);
            const simd_float4x4 mvp =
                simd_mul(projMatrix, simd_mul(viewMatrix, worldMatrix));
            [encoder setVertexBytes:&mvp length:sizeof(mvp) atIndex:1];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:6
                                 indexType:MTLIndexTypeUInt16
                               indexBuffer:_indexBuffer
                         indexBufferOffset:0];
        };

        if (hasGameSurfaces) {
            drawQuad(matrix_identity_float4x4);
        } else {
            // A 1.6m x 0.9m checker-lined room recessed 0.55m from the opening.
            // Its corners and side-wall motion make binocular depth and tracked
            // head parallax directly visible before the game produces surfaces.
            constexpr float kDepth = 0.55f;
            constexpr float kHalfWidth = 0.80f;
            constexpr float kHalfHeight = 0.45f;
            constexpr float kPiOverTwo = 1.57079632679f;
            drawQuad(MatrixTranslation(0.0f, 0.0f, -kDepth));
            drawQuad(simd_mul(MatrixTranslation(-kHalfWidth, 0.0f, -kDepth * 0.5f),
                              simd_mul(MatrixRotationY(kPiOverTwo),
                                       MatrixScale(kDepth / 1.6f, 1.0f, 1.0f))));
            drawQuad(simd_mul(MatrixTranslation(kHalfWidth, 0.0f, -kDepth * 0.5f),
                              simd_mul(MatrixRotationY(kPiOverTwo),
                                       MatrixScale(kDepth / 1.6f, 1.0f, 1.0f))));
            drawQuad(simd_mul(MatrixTranslation(0.0f, -kHalfHeight, -kDepth * 0.5f),
                              simd_mul(MatrixRotationX(kPiOverTwo),
                                       MatrixScale(1.0f, kDepth / 0.9f, 1.0f))));
            drawQuad(simd_mul(MatrixTranslation(0.0f, kHalfHeight, -kDepth * 0.5f),
                              simd_mul(MatrixRotationX(kPiOverTwo),
                                       MatrixScale(1.0f, kDepth / 0.9f, 1.0f))));
        }

        [encoder endEncoding];
    }

    cp_drawable_encode_present(drawable, cmdBuffer);
    [cmdBuffer commit];

    cp_frame_end_submission(frame);
    return YES;
}

@end

static DusklightVisionRendererHost* g_visionHost = nil;

void dusklight_visionos_start(cp_layer_renderer_t layerRenderer) {
    if (g_visionHost && [g_visionHost usesLayerRenderer:layerRenderer]) {
        [g_visionHost start];
        return;
    }

    if (g_visionHost) {
        // A system interruption can replace the compositor layer. Fully join
        // the old host before releasing it; destroying a joinable std::thread
        // invokes std::terminate.
        [g_visionHost stop];
        os_log(OS_LOG_DEFAULT,
               "[Dusklight] Replacing invalidated compositor layer; game engine remains running");
    }

    dusk::gfx::RegisterVisionCompositor((__bridge const void*)layerRenderer);
    g_visionHost = [[DusklightVisionRendererHost alloc] initWithLayerRenderer:layerRenderer];
    [g_visionHost start];
    os_log(OS_LOG_DEFAULT, "[Dusklight] VisionCompositorRenderer started");
}

void dusklight_visionos_stop(void) {
    if (g_visionHost) {
        [g_visionHost stop];
        g_visionHost = nil;
    }
    [g_sharedDioramaAnchor stop];
    g_sharedDioramaAnchor = nil;
    g_hasDioramaAnchorReference.store(false, std::memory_order_release);
}

void dusklight_visionos_set_app_active(bool active) {
    dusk::gfx::SetVisionAppActive(active);
}

bool dusklight_visionos_is_compositor_running(void) {
    return dusk::gfx::IsVisionCompositorRunning();
}

void dusklight_visionos_set_diorama_placement(float x, float y, float distance,
                                              float width, float aspectRatio) {
    g_dioramaPlacementSequence.fetch_add(1, std::memory_order_acq_rel);
    g_dioramaX.store(x, std::memory_order_relaxed);
    g_dioramaY.store(y, std::memory_order_relaxed);
    g_dioramaDistance.store(fminf(fmaxf(distance, 0.75f), 5.0f),
                            std::memory_order_relaxed);
    g_dioramaWidth.store(fminf(fmaxf(width, 0.4f), 4.0f), std::memory_order_relaxed);
    g_dioramaAspect.store(fminf(fmaxf(aspectRatio, 4.0f / 3.0f), 64.0f / 27.0f),
                           std::memory_order_relaxed);
    g_dioramaPlacementSequence.fetch_add(1, std::memory_order_release);
}

void dusklight_visionos_set_scene_plane_distance(float distanceMeters) {
    auto* stereoPass = dusk::gfx::GetStereoParallaxPass();
    if (!stereoPass) return;

    auto settings = stereoPass->GetSettings();
    const float clampedMeters = fminf(fmaxf(distanceMeters, 1.5f), 20.5f);
    settings.convergenceDepth = (clampedMeters - 1.5f) / 19.0f;
    stereoPass->SetSettings(settings);
}

void dusklight_visionos_recenter_diorama(void) {
    g_dioramaPlacementSequence.fetch_add(1, std::memory_order_acq_rel);
    g_dioramaX.store(0.0f, std::memory_order_relaxed);
    g_dioramaY.store(0.0f, std::memory_order_relaxed);
    g_dioramaPlacementSequence.fetch_add(1, std::memory_order_release);
    g_hasDioramaAnchorReference.store(false, std::memory_order_release);
    g_dioramaRecenterGeneration.fetch_add(1, std::memory_order_acq_rel);
}

float dusklight_visionos_get_diorama_aspect_ratio(void) {
    return g_dioramaAspect.load(std::memory_order_acquire);
}

#endif // TARGET_OS_VISION
#endif // __APPLE__
