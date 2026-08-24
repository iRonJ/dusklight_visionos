#pragma once

// Metal shader source and quad geometry for the Dusklight visionOS diorama:
// a textured world-space quad that displays one StereoParallax eye texture
// per CompositorServices view.

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#import <simd/simd.h>

namespace dusk::vision {

struct DioramaVertex {
    simd_float3 position;
    simd_float2 uv;
};

// 16:9 diorama quad mesh centered at origin (1.6m x 0.9m)
inline constexpr DioramaVertex kDioramaVertices[4] = {
    {{-0.80f, 0.45f, 0.0f}, {0.0f, 0.0f}},  // Top-Left
    {{-0.80f, -0.45f, 0.0f}, {0.0f, 1.0f}}, // Bottom-Left
    {{0.80f, 0.45f, 0.0f}, {1.0f, 0.0f}},   // Top-Right
    {{0.80f, -0.45f, 0.0f}, {1.0f, 1.0f}},  // Bottom-Right
};

inline constexpr uint16_t kDioramaIndices[6] = {
    0, 1, 2,
    2, 1, 3,
};

inline constexpr const char* kDioramaShaderSource = R"""(
#include <metal_stdlib>
using namespace metal;

struct DioramaVertexIn {
    float3 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct DioramaVertexOut {
    float4 position [[position]];
    float2 uv;
};

struct DioramaUniforms {
    float4x4 mvp;
};

inline float4 sample_diorama_color(texture2d<float> colorTexture,
                                   sampler colorSampler,
                                   float2 uv) {
    const float2 textureSize = float2(colorTexture.get_width(), colorTexture.get_height());
    const float2 uvDx = dfdx(uv);
    const float2 uvDy = dfdy(uv);
    const float footprint = max(length(uvDx * textureSize), length(uvDy * textureSize));

    if (footprint <= 1.25f) {
        return colorTexture.sample(colorSampler, uv);
    }

    // IOSurface-backed textures have no mip chain. Average across the pixel
    // footprint when the physical window is smaller or farther away so fine
    // geometry does not shimmer under head movement.
    const float2 dx = uvDx * 0.375f;
    const float2 dy = uvDy * 0.375f;
    return (colorTexture.sample(colorSampler, uv - dx - dy) +
            colorTexture.sample(colorSampler, uv + dx - dy) +
            colorTexture.sample(colorSampler, uv - dx + dy) +
            colorTexture.sample(colorSampler, uv + dx + dy)) * 0.25f;
}

vertex DioramaVertexOut diorama_vertex_main(DioramaVertexIn in [[stage_in]],
                                            constant DioramaUniforms& uniforms [[buffer(1)]]) {
    DioramaVertexOut out;
    out.position = uniforms.mvp * float4(in.position, 1.0);
    out.uv = in.uv;
    return out;
}

fragment float4 diorama_fragment_main(
    DioramaVertexOut in [[stage_in]],
    texture2d<float> colorTexture [[texture(0)]],
    sampler colorSampler [[sampler(0)]]) {
    return sample_diorama_color(colorTexture, colorSampler, in.uv);
}

struct DioramaInteractiveFragmentOut {
    float4 color [[color(0)]];
    ushort trackingArea [[color(1)]];
};

fragment DioramaInteractiveFragmentOut diorama_interactive_fragment_main(
    DioramaVertexOut in [[stage_in]],
    texture2d<float> colorTexture [[texture(0)]],
    sampler colorSampler [[sampler(0)]],
    constant ushort& trackingArea [[buffer(0)]]) {
    DioramaInteractiveFragmentOut out;
    out.color = sample_diorama_color(colorTexture, colorSampler, in.uv);
    out.trackingArea = trackingArea;
    return out;
}
)""";

inline simd_float4x4 MatrixTranslation(float x, float y, float z) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(x, y, z, 1.0f);
    return m;
}

inline simd_float4x4 MatrixScale(float x, float y, float z) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0].x = x;
    m.columns[1].y = y;
    m.columns[2].z = z;
    return m;
}

inline simd_float4x4 MatrixRotationX(float radians) {
    const float c = cosf(radians);
    const float s = sinf(radians);
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[1] = simd_make_float4(0.0f, c, s, 0.0f);
    m.columns[2] = simd_make_float4(0.0f, -s, c, 0.0f);
    return m;
}

inline simd_float4x4 MatrixRotationY(float radians) {
    const float c = cosf(radians);
    const float s = sinf(radians);
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0] = simd_make_float4(c, 0.0f, -s, 0.0f);
    m.columns[2] = simd_make_float4(s, 0.0f, c, 0.0f);
    return m;
}

} // namespace dusk::vision

#endif // TARGET_OS_VISION
#endif // __APPLE__
