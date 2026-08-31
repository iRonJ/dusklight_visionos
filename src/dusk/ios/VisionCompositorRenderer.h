#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#import <CompositorServices/CompositorServices.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void dusklight_visionos_start(cp_layer_renderer_t layerRenderer);
void dusklight_visionos_stop(void);
void dusklight_visionos_set_app_active(bool active);
void dusklight_visionos_set_game_paused(bool paused);
bool dusklight_visionos_is_game_paused(void);
bool dusklight_visionos_is_compositor_running(void);
void dusklight_visionos_set_diorama_placement(float x, float y, float distance,
                                              float width, float aspectRatio);
void dusklight_visionos_set_scene_plane_distance(float distanceMeters);
void dusklight_visionos_recenter_diorama(void);
float dusklight_visionos_get_diorama_aspect_ratio(void);

#ifdef __cplusplus
}
#endif

#endif // TARGET_OS_VISION
#endif // __APPLE__
