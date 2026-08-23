#include "dusk/ios/VisionCompositorRenderer.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dawn/native/MetalBackend.h>
#include <aurora/webgpu.hpp>
#include "dusk/gfx/StereoParallax.hpp"

#include <atomic>
#include <thread>

@interface DusklightVisionRendererHost : NSObject

- (instancetype)initWithLayerRenderer:(cp_layer_renderer_t)layerRenderer;
- (void)start;
- (void)stop;

@end

@implementation DusklightVisionRendererHost {
    cp_layer_renderer_t _layerRenderer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    ar_session_t _arSession;
    ar_world_tracking_provider_t _worldTracking;
    std::thread _renderThread;
    std::atomic<bool> _running;
}

- (instancetype)initWithLayerRenderer:(cp_layer_renderer_t)layerRenderer {
    self = [super init];
    if (self) {
        _layerRenderer = layerRenderer;
        _device = cp_layer_renderer_get_device(layerRenderer);
        _commandQueue = [_device newCommandQueue];
        _running.store(false);

        // Initialize ARKit for head tracking if supported and authorized
        _worldTracking = nil;
        _arSession = nil;
        @try {
            if (ar_world_tracking_provider_is_supported()) {
                _worldTracking = ar_world_tracking_provider_create(nil);
                _arSession = ar_session_create();
                if (_worldTracking && _arSession) {
                    ar_data_providers_t providers = ar_data_providers_create_with_data_providers(_worldTracking, nil);
                    ar_session_run(_arSession, providers);
                }
            }
        } @catch (NSException *exception) {
            NSLog(@"[Dusklight] ARKit world tracking optional fallback: %@", exception);
            _worldTracking = nil;
            _arSession = nil;
        }

        // Initialize the WebGPU stereo parallax hook
        dusk::gfx::InitializeStereoParallaxHook();
    }
    return self;
}

- (void)start {
    if (_running.load()) return;
    _running.store(true);
    _renderThread = std::thread([self]() {
        [self renderLoop];
    });
}

- (void)stop {
    _running.store(false);
    if (_renderThread.joinable()) {
        _renderThread.join();
    }
}

- (void)renderLoop {
    pthread_setname_np("Dusklight.VisionRenderThread");

    while (_running.load()) {
        cp_layer_renderer_state state = cp_layer_renderer_get_state(_layerRenderer);
        if (state == cp_layer_renderer_state_invalidated) {
            break;
        } else if (state == cp_layer_renderer_state_paused) {
            cp_layer_renderer_wait_until_running(_layerRenderer);
            continue;
        }

        @autoreleasepool {
            [self renderFrame];
        }
    }
}

- (void)renderFrame {
    cp_frame_t frame = cp_layer_renderer_query_next_frame(_layerRenderer);
    if (!frame) return;

    cp_frame_start_update(frame);
    cp_frame_end_update(frame);

    cp_frame_timing_t timing = cp_frame_predict_timing(frame);
    if (!timing) return;

    cp_time_t optimalInputTime = cp_frame_timing_get_optimal_input_time(timing);
    cp_time_wait_until(optimalInputTime);

    cp_drawable_t drawable = cp_frame_query_drawable(frame);
    if (!drawable) return;

    cp_frame_start_submission(frame);

    // Query head pose from ARKit at presentation timestamp
    cp_time_t presentationTime = cp_frame_timing_get_presentation_time(timing);
    if (_worldTracking) {
        CFTimeInterval presentationTimeSeconds = cp_time_to_cf_time_interval(presentationTime);
        ar_device_anchor_t deviceAnchor = ar_device_anchor_create();
        ar_device_anchor_query_status_t status =
            ar_world_tracking_provider_query_device_anchor_at_timestamp(_worldTracking, presentationTimeSeconds, deviceAnchor);
        if (status == ar_device_anchor_query_status_success) {
            cp_drawable_set_device_anchor(drawable, deviceAnchor);
        }
    }

    // Wait for Dawn to finish encoding before reading the shared IOSurface textures
    wgpu::Device dawnDevice = aurora::webgpu::get_device();
    if (dawnDevice) {
        dawn::native::metal::WaitForCommandsToBeScheduled(dawnDevice.Get());
    }

    auto* stereoPass = dusk::gfx::GetStereoParallaxPass();
    IOSurfaceRef leftColSurf = stereoPass ? stereoPass->GetLeftColorSurface() : nullptr;
    IOSurfaceRef rightColSurf = stereoPass ? stereoPass->GetRightColorSurface() : nullptr;
    IOSurfaceRef leftDepSurf = stereoPass ? stereoPass->GetLeftDepthSurface() : nullptr;
    IOSurfaceRef rightDepSurf = stereoPass ? stereoPass->GetRightDepthSurface() : nullptr;
    uint32_t width = stereoPass ? stereoPass->GetWidth() : 0;
    uint32_t height = stereoPass ? stereoPass->GetHeight() : 0;

    id<MTLCommandBuffer> cmdBuffer = [_commandQueue commandBuffer];
    cmdBuffer.label = @"VisionCompositor Blit CommandBuffer";

    if (leftColSurf && rightColSurf && width > 0 && height > 0) {
        MTLTextureDescriptor* colDesc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        colDesc.storageMode = MTLStorageModeShared;
        colDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

        id<MTLTexture> srcLeftCol = [_device newTextureWithDescriptor:colDesc iosurface:leftColSurf plane:0];
        id<MTLTexture> srcRightCol = [_device newTextureWithDescriptor:colDesc iosurface:rightColSurf plane:0];

        MTLTextureDescriptor* depDesc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        depDesc.storageMode = MTLStorageModeShared;
        depDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

        id<MTLTexture> srcLeftDep = leftDepSurf ? [_device newTextureWithDescriptor:depDesc iosurface:leftDepSurf plane:0] : nil;
        id<MTLTexture> srcRightDep = rightDepSurf ? [_device newTextureWithDescriptor:depDesc iosurface:rightDepSurf plane:0] : nil;

        size_t viewCount = cp_drawable_get_view_count(drawable);
        size_t textureCount = cp_drawable_get_texture_count(drawable);

        id<MTLBlitCommandEncoder> blit = [cmdBuffer blitCommandEncoder];
        blit.label = @"VisionCompositor Blit";

        for (size_t i = 0; i < viewCount; ++i) {
            cp_view_t view = cp_drawable_get_view(drawable, i);
            size_t texIndex = (textureCount > 1 && i < textureCount) ? i : 0;

            id<MTLTexture> dstColor = cp_drawable_get_color_texture(drawable, texIndex);
            id<MTLTexture> dstDepth = cp_drawable_get_depth_texture(drawable, texIndex);

            id<MTLTexture> srcColor = (i == 0) ? srcLeftCol : srcRightCol;
            id<MTLTexture> srcDepth = (i == 0) ? srcLeftDep : srcRightDep;

            cp_view_texture_map_t texMap = cp_view_get_view_texture_map(view);
            MTLViewport vp = cp_view_texture_map_get_viewport(texMap);

            if (srcColor && dstColor) {
                NSUInteger copyW = std::min<NSUInteger>(srcColor.width, static_cast<NSUInteger>(vp.width));
                NSUInteger copyH = std::min<NSUInteger>(srcColor.height, static_cast<NSUInteger>(vp.height));

                NSUInteger dstSlice = (textureCount == 1 && viewCount > 1) ? i : 0;

                [blit copyFromTexture:srcColor
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(copyW, copyH, 1)
                            toTexture:dstColor
                     destinationSlice:dstSlice
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(vp.originX),
                                                   static_cast<NSUInteger>(vp.originY), 0)];
            }

            if (srcDepth && dstDepth) {
                NSUInteger copyW = std::min<NSUInteger>(srcDepth.width, static_cast<NSUInteger>(vp.width));
                NSUInteger copyH = std::min<NSUInteger>(srcDepth.height, static_cast<NSUInteger>(vp.height));

                NSUInteger dstSlice = (textureCount == 1 && viewCount > 1) ? i : 0;

                [blit copyFromTexture:srcDepth
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(copyW, copyH, 1)
                            toTexture:dstDepth
                     destinationSlice:dstSlice
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(vp.originX),
                                                   static_cast<NSUInteger>(vp.originY), 0)];
            }
        }

        [blit endEncoding];
    }

    cp_drawable_encode_present(drawable, cmdBuffer);
    [cmdBuffer commit];

    cp_frame_end_submission(frame);
}

@end

static DusklightVisionRendererHost* g_visionHost = nil;

void dusklight_visionos_start(cp_layer_renderer_t layerRenderer) {
    if (!g_visionHost) {
        g_visionHost = [[DusklightVisionRendererHost alloc] initWithLayerRenderer:layerRenderer];
        [g_visionHost start];
    }
}

#endif // TARGET_OS_VISION
#endif // __APPLE__
