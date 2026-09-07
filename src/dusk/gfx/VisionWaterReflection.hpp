#pragma once

#include <cstdint>

namespace dusk::gfx {

/**
 * Generates and updates a stereoscopically correct, dynamic environment reflection
 * and specular ripple texture for water surfaces in visionOS 3D diorama mode.
 * 
 * Bypasses legacy GameCube 2D framebuffer copies to eliminate thermal fan noise,
 * while preserving authentic 3D underwater depth and fluid surface ripple dynamics.
 */
void UpdateStereoWaterReflectionTexture();

} // namespace dusk::gfx
