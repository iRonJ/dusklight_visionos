#pragma once

// ARKit world-tracking wrapper for the Dusklight visionOS diorama.
// Owns the ar_session/world-tracking provider and answers per-frame device
// (head) pose queries so the compositor renderer can place content in world
// space and hand CompositorServices the device anchor for reprojection.

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#import <ARKit/ARKit.h>
#import <Foundation/Foundation.h>
#import <simd/simd.h>

@interface DusklightDioramaAnchor : NSObject

// Creates and runs an ARKit session with world tracking when supported.
// Falls back gracefully (isTracking == NO) when unavailable (e.g. simulator).
- (instancetype)init;

@property(nonatomic, readonly) BOOL isTracking;

// Queries the device (head) anchor at the given timestamp (seconds).
// Returns nil when tracking is unavailable or the query fails.
// On success outOriginFromDevice (if non-NULL) receives the world-from-device
// transform for that timestamp.
- (ar_device_anchor_t)queryDeviceAnchorAtTime:(CFTimeInterval)timestamp
                          originFromDevice:(simd_float4x4*)outOriginFromDevice;

- (void)stop;

@end

#endif // TARGET_OS_VISION
#endif // __APPLE__
