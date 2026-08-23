#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#import <CompositorServices/CompositorServices.h>

#ifdef __cplusplus
extern "C" {
#endif

void dusklight_visionos_start(cp_layer_renderer_t layerRenderer);
void dusklight_visionos_stop(void);

#ifdef __cplusplus
}
#endif

#endif // TARGET_OS_VISION
#endif // __APPLE__
