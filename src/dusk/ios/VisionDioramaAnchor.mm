#include "dusk/ios/VisionDioramaAnchor.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#import <os/log.h>

@implementation DusklightDioramaAnchor {
    ar_session_t _session;
    ar_world_tracking_provider_t _worldTracking;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _session = nil;
        _worldTracking = nil;
        @try {
            if (ar_world_tracking_provider_is_supported()) {
                ar_world_tracking_configuration_t config = ar_world_tracking_configuration_create();
                _worldTracking = ar_world_tracking_provider_create(config);
                _session = ar_session_create();
                if (_worldTracking && _session) {
                    ar_data_providers_t providers =
                        ar_data_providers_create_with_data_providers(_worldTracking, nil);
                    ar_session_run(_session, providers);
                    os_log(OS_LOG_DEFAULT, "[Dusklight] ARKit world tracking started");
                }
            } else {
                os_log(OS_LOG_DEFAULT, "[Dusklight] ARKit world tracking unsupported; head pose disabled");
            }
        } @catch (NSException* exception) {
            os_log_error(OS_LOG_DEFAULT, "[Dusklight] ARKit tracking unavailable: %@", exception);
            _worldTracking = nil;
            _session = nil;
        }
    }
    return self;
}

- (BOOL)isTracking {
    return _worldTracking != nil &&
           ar_data_provider_get_state(_worldTracking) == ar_data_provider_state_running;
}

- (ar_device_anchor_t)queryDeviceAnchorAtTime:(CFTimeInterval)timestamp
                          originFromDevice:(simd_float4x4*)outOriginFromDevice {
    if (!self.isTracking) {
        return nil;
    }
    ar_device_anchor_t anchor = ar_device_anchor_create();
    ar_device_anchor_query_status_t status =
        ar_world_tracking_provider_query_device_anchor_at_timestamp(_worldTracking, timestamp, anchor);
    if (status != ar_device_anchor_query_status_success) {
        return nil;
    }
    if (outOriginFromDevice) {
        *outOriginFromDevice = ar_device_anchor_get_origin_from_anchor_transform(anchor);
    }
    return anchor;
}

- (void)stop {
    if (_session) {
        ar_session_stop(_session);
        _session = nil;
    }
    _worldTracking = nil;
}

@end

#endif // TARGET_OS_VISION
#endif // __APPLE__
