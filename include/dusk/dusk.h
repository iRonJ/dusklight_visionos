#ifndef DUSK_DUSK_H
#define DUSK_DUSK_H

#include <aurora/aurora.h>

#include "aurora/gfx.h"

extern AuroraInfo auroraInfo;

namespace dusk {
    extern AuroraStats lastFrameAuroraStats;
    extern float frameUsagePct;
}

constexpr u32 defaultWindowWidth = 608;
constexpr u32 defaultWindowHeight = 448;

constexpr u32 defaultAspectRatioW = 64;
constexpr u32 defaultAspectRatioH = 27;

// 64:27 (21:9) is the target presentation aspect ratio. These constants are
// documentation only; the render-time aspect is driven dynamically by
// updateRenderSize()/setTvSize() and the FB base coordinate space remains
// defaultWindowWidth x defaultWindowHeight.
static_assert(static_cast<float>(defaultAspectRatioW) / static_cast<float>(defaultAspectRatioH) > 2.0f);

#endif  // DUSK_DUSK_H
