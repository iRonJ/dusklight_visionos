#include "dusk/gfx/StereoParallax.hpp"

#include <aurora/aurora.h>
#include <aurora/webgpu.hpp>
#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cmath>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <IOSurface/IOSurfaceRef.h>
#include <os/log.h>
#endif

namespace dusk::gfx {

namespace {

#if defined(__APPLE__)
IOSurfaceRef CreateIOSurfaceHelper(uint32_t width, uint32_t height, OSType pixelFormat, uint32_t bytesPerElement) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    int32_t w = static_cast<int32_t>(width);
    int32_t h = static_cast<int32_t>(height);
    int32_t pf = static_cast<int32_t>(pixelFormat);
    int32_t bpe = static_cast<int32_t>(bytesPerElement);

    CFNumberRef numW = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w);
    CFNumberRef numH = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h);
    CFNumberRef numPF = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);
    CFNumberRef numBPE = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpe);

    CFDictionaryAddValue(dict, kIOSurfaceWidth, numW);
    CFDictionaryAddValue(dict, kIOSurfaceHeight, numH);
    CFDictionaryAddValue(dict, kIOSurfacePixelFormat, numPF);
    CFDictionaryAddValue(dict, kIOSurfaceBytesPerElement, numBPE);

    CFRelease(numW);
    CFRelease(numH);
    CFRelease(numPF);
    CFRelease(numBPE);

    IOSurfaceRef surface = IOSurfaceCreate(dict);
    CFRelease(dict);
    return surface;
}
#endif

const char* kStereoWGSL = R"""(
struct StereoUniforms {
    eye_sign: f32,
    eye_separation: f32,
    convergence_depth: f32,
    depth_scale: f32,
};

struct VertexInput {
    @location(0) position: vec2<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: StereoUniforms;
@group(0) @binding(1) var color_texture: texture_2d<f32>;
@group(0) @binding(2) var color_sampler: sampler;
@group(0) @binding(3) var depth_texture: texture_depth_2d;
@group(0) @binding(4) var depth_sampler: sampler;

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.uv = input.position;
    let ndc_x = input.position.x * 2.0 - 1.0;
    let ndc_y = 1.0 - input.position.y * 2.0;
    out.clip_position = vec4<f32>(ndc_x, ndc_y, 0.0, 1.0);
    return out;
}

fn linear_depth_from_raw(raw_depth: f32) -> f32 {
    if (raw_depth <= 0.0001) {
        return 1.0;
    }
    let z_near = 0.5;
    let z_far = 500.0;
    return clamp((z_near * z_far) /
                 (z_far - (1.0 - raw_depth) * (z_far - z_near)) / z_far,
                 0.0, 1.0);
}

fn disparity_from_linear(linear_depth: f32) -> f32 {
    return uniforms.eye_sign * uniforms.eye_separation
         * (linear_depth - uniforms.convergence_depth) * uniforms.depth_scale;
}

// Solve source_x + disparity(depth(source_x)) = target_x. Starting from
// several depth hypotheses lets foreground and background both compete at a
// silhouette instead of connecting them with a stretched mesh triangle.
fn solve_source(target_uv: vec2<f32>, initial_linear_depth: f32) -> vec4<f32> {
    var source_uv = target_uv;
    source_uv.x = clamp(target_uv.x - disparity_from_linear(initial_linear_depth), 0.0, 1.0);

    var raw_depth = 0.0;
    for (var iteration = 0; iteration < 4; iteration++) {
        raw_depth = textureSampleLevel(depth_texture, depth_sampler, source_uv, 0i);
        source_uv.x = clamp(target_uv.x - disparity_from_linear(linear_depth_from_raw(raw_depth)), 0.0, 1.0);
    }

    raw_depth = textureSampleLevel(depth_texture, depth_sampler, source_uv, 0i);
    let projected_x = source_uv.x + disparity_from_linear(linear_depth_from_raw(raw_depth));
    let residual = abs(projected_x - target_uv.x);
    return vec4<f32>(source_uv, raw_depth, residual);
}

fn better_solution(candidate: vec4<f32>, current: vec4<f32>) -> bool {
    let pixel_width = 1.0 / f32(textureDimensions(depth_texture).x);
    if (candidate.w < current.w - pixel_width * 0.25) {
        return true;
    }
    // Reverse-Z: the larger raw value is nearer and wins where two surfaces
    // project onto the same output pixel.
    return abs(candidate.w - current.w) <= pixel_width * 0.25 && candidate.z > current.z;
}

fn solution_covers_target(solution: vec4<f32>) -> bool {
    let pixel_width = 1.0 / f32(textureDimensions(depth_texture).x);
    return solution.w <= pixel_width * 0.75;
}

// A newly revealed background pixel has no inverse-reprojection solution:
// the center view never rendered what was hidden behind the foreground
// silhouette. Fixed-point iteration lands on alternating sides of that
// depth discontinuity and, if accepted anyway, repeats the foreground as a
// displaced clone. For only those uncovered pixels, inspect the complete
// disparity interval and borrow the farthest available layer. Reverse-Z
// makes the smallest raw depth the farthest surface.
fn disocclusion_fill(target_uv: vec2<f32>) -> vec3<f32> {
    let near_disparity = disparity_from_linear(0.0);
    let far_disparity = disparity_from_linear(1.0);
    var best = vec3<f32>(target_uv, 1.0);

    for (var sample_index = 0; sample_index < 9; sample_index++) {
        let t = f32(sample_index) / 8.0;
        var source_uv = target_uv;
        source_uv.x = clamp(target_uv.x - mix(near_disparity, far_disparity, t), 0.0, 1.0);
        let raw_depth = textureSampleLevel(depth_texture, depth_sampler, source_uv, 0i);
        if (raw_depth < best.z) {
            best = vec3<f32>(source_uv, raw_depth);
        }
    }
    return best;
}

struct FragmentOutput {
    @location(0) color: vec4<f32>,
    @builtin(frag_depth) depth: f32,
};

@fragment
fn fs_main(in: VertexOutput) -> FragmentOutput {
    // True stereo already supplies a complete GX render for each eye. In that
    // mode eye separation is zero, so inverse reprojection cannot move a pixel
    // and only wastes many depth fetches reproducing the input image.
    if (abs(uniforms.eye_separation) <= 0.000001) {
        var out: FragmentOutput;
        let game_color = textureSample(color_texture, color_sampler, in.uv);
        out.color = vec4<f32>(game_color.rgb, 1.0);
        out.depth = textureSampleLevel(depth_texture, depth_sampler, in.uv, 0i);
        return out;
    }

    let near_solution = solve_source(in.uv, 0.0);
    let middle_solution = solve_source(in.uv, 0.5);
    let far_solution = solve_source(in.uv, 1.0);

    var solution = vec4<f32>(in.uv, 0.0, 1.0);
    var has_coverage = false;
    if (solution_covers_target(near_solution)) {
        solution = near_solution;
        has_coverage = true;
    }
    if (solution_covers_target(middle_solution) &&
        (!has_coverage || better_solution(middle_solution, solution))) {
        solution = middle_solution;
        has_coverage = true;
    }
    if (solution_covers_target(far_solution) &&
        (!has_coverage || better_solution(far_solution, solution))) {
        solution = far_solution;
        has_coverage = true;
    }

    if (!has_coverage) {
        let fill = disocclusion_fill(in.uv);
        solution = vec4<f32>(fill.xy, fill.z, 0.0);
    }

    var out: FragmentOutput;
    let game_color = textureSample(color_texture, color_sampler, solution.xy);
    // The EFB alpha channel is game render state, not spatial-layer opacity.
    // The 16:9 diorama is opaque everywhere inside its window.
    out.color = vec4<f32>(game_color.rgb, 1.0);
    out.depth = solution.z;
    return out;
}
)""";

// Per-pixel inverse reprojection runs in the fragment shader, so a fullscreen
// two-triangle mesh is sufficient and cannot bridge depth discontinuities.
constexpr uint32_t kGridDivisionsX = 1;
constexpr uint32_t kGridDivisionsY = 1;

struct AppleReadyFence {
    void* event = nullptr;
    uint64_t value = 0;
};

struct UniformBufferData {
    float eye_sign;
    float eye_separation;
    float convergence_depth;
    float depth_scale;
};

struct EyeResources {
    wgpu::Texture colorTexture;
    wgpu::TextureView colorView;
    wgpu::Texture depthTexture;
    wgpu::TextureView depthView;
#if defined(__APPLE__)
    wgpu::SharedTextureMemory colorSharedMem;
    wgpu::SharedTextureMemory depthSharedMem;
#endif
    wgpu::Buffer uniformBuffer;
    wgpu::BindGroup bindGroup;
    WGPUTextureView boundColorView = nullptr;
    WGPUTextureView boundDepthView = nullptr;
    WGPUSampler boundColorSampler = nullptr;
    WGPUSampler boundDepthSampler = nullptr;
};

} // namespace

struct StereoParallaxPass::Impl {
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::RenderPipeline pipeline;
    wgpu::BindGroupLayout bindGroupLayout;

    wgpu::Buffer vertexBuffer;
    wgpu::Buffer indexBuffer;
    uint32_t indexCount = 0;

    // Aurora's depth texture sampler uses linear filtering, but WebGPU forbids binding a
    // filtering sampler to a `NonFiltering` bind group layout slot (required for
    // texture_depth_2d reads without comparison). Depth is sampled point/nearest here.
    wgpu::Sampler depthNearestSampler;

    EyeResources leftEye;
    EyeResources rightEye;
    aurora::gfx::CapturedFrame leftCapture;
    aurora::gfx::CapturedFrame rightCapture;
    bool hasTrueStereoFrame = false;

    void CreateGridMesh() {
        std::vector<float> vertices;
        vertices.reserve((kGridDivisionsX + 1) * (kGridDivisionsY + 1) * 2);

        for (uint32_t y = 0; y <= kGridDivisionsY; ++y) {
            float v = static_cast<float>(y) / static_cast<float>(kGridDivisionsY);
            for (uint32_t x = 0; x <= kGridDivisionsX; ++x) {
                float u = static_cast<float>(x) / static_cast<float>(kGridDivisionsX);
                vertices.push_back(u);
                vertices.push_back(v);
            }
        }

        std::vector<uint32_t> indices;
        indices.reserve(kGridDivisionsX * kGridDivisionsY * 6);

        for (uint32_t y = 0; y < kGridDivisionsY; ++y) {
            for (uint32_t x = 0; x < kGridDivisionsX; ++x) {
                uint32_t i0 = y * (kGridDivisionsX + 1) + x;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = (y + 1) * (kGridDivisionsX + 1) + x;
                uint32_t i3 = i2 + 1;

                indices.push_back(i0);
                indices.push_back(i2);
                indices.push_back(i1);

                indices.push_back(i1);
                indices.push_back(i2);
                indices.push_back(i3);
            }
        }

        indexCount = static_cast<uint32_t>(indices.size());

        wgpu::BufferDescriptor vbDesc{};
        vbDesc.label = "StereoParallax Grid VB";
        vbDesc.size = vertices.size() * sizeof(float);
        vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vertexBuffer = device.CreateBuffer(&vbDesc);
        queue.WriteBuffer(vertexBuffer, 0, vertices.data(), vbDesc.size);

        wgpu::BufferDescriptor ibDesc{};
        ibDesc.label = "StereoParallax Grid IB";
        ibDesc.size = indices.size() * sizeof(uint32_t);
        ibDesc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        indexBuffer = device.CreateBuffer(&ibDesc);
        queue.WriteBuffer(indexBuffer, 0, indices.data(), ibDesc.size);
    }

    void CreatePipeline() {
        wgpu::SamplerDescriptor depthSamplerDesc{};
        depthSamplerDesc.label = "StereoParallax Depth Nearest Sampler";
        depthSamplerDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
        depthSamplerDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
        depthSamplerDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
        depthSamplerDesc.magFilter = wgpu::FilterMode::Nearest;
        depthSamplerDesc.minFilter = wgpu::FilterMode::Nearest;
        depthSamplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        depthNearestSampler = device.CreateSampler(&depthSamplerDesc);

        wgpu::ShaderSourceWGSL wgslDesc{};
        wgslDesc.code = kStereoWGSL;

        wgpu::ShaderModuleDescriptor smDesc{};
        smDesc.label = "StereoParallax Shader";
        smDesc.nextInChain = &wgslDesc;
        auto shaderModule = device.CreateShaderModule(&smDesc);

        // Bind group layout
        std::vector<wgpu::BindGroupLayoutEntry> bglEntries(5);
        // Uniforms
        bglEntries[0].binding = 0;
        bglEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        bglEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        bglEntries[0].buffer.minBindingSize = sizeof(UniformBufferData);

        // Color texture
        bglEntries[1].binding = 1;
        bglEntries[1].visibility = wgpu::ShaderStage::Fragment;
        bglEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        bglEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;

        // Color sampler
        bglEntries[2].binding = 2;
        bglEntries[2].visibility = wgpu::ShaderStage::Fragment;
        bglEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        // Depth texture
        bglEntries[3].binding = 3;
        bglEntries[3].visibility = wgpu::ShaderStage::Fragment;
        bglEntries[3].texture.sampleType = wgpu::TextureSampleType::Depth;
        bglEntries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;

        // Depth sampler
        bglEntries[4].binding = 4;
        bglEntries[4].visibility = wgpu::ShaderStage::Fragment;
        bglEntries[4].sampler.type = wgpu::SamplerBindingType::NonFiltering;

        wgpu::BindGroupLayoutDescriptor bglDesc{};
        bglDesc.label = "StereoParallax BGL";
        bglDesc.entryCount = static_cast<uint32_t>(bglEntries.size());
        bglDesc.entries = bglEntries.data();
        bindGroupLayout = device.CreateBindGroupLayout(&bglDesc);

        wgpu::PipelineLayoutDescriptor plDesc{};
        plDesc.label = "StereoParallax Pipeline Layout";
        plDesc.bindGroupLayoutCount = 1;
        plDesc.bindGroupLayouts = &bindGroupLayout;
        auto pipelineLayout = device.CreatePipelineLayout(&plDesc);

        // Vertex state
        wgpu::VertexAttribute vertAttr{};
        vertAttr.shaderLocation = 0;
        vertAttr.offset = 0;
        vertAttr.format = wgpu::VertexFormat::Float32x2;

        wgpu::VertexBufferLayout vbLayout{};
        vbLayout.arrayStride = 2 * sizeof(float);
        vbLayout.stepMode = wgpu::VertexStepMode::Vertex;
        vbLayout.attributeCount = 1;
        vbLayout.attributes = &vertAttr;

        wgpu::ColorTargetState colorTarget{};
        colorTarget.format = wgpu::TextureFormat::BGRA8Unorm;

        wgpu::FragmentState fragmentState{};
        fragmentState.module = shaderModule;
        fragmentState.entryPoint = "fs_main";
        fragmentState.targetCount = 1;
        fragmentState.targets = &colorTarget;

        wgpu::DepthStencilState depthStencil{};
        depthStencil.format = wgpu::TextureFormat::Depth32Float;
        depthStencil.depthWriteEnabled = true;
        // Aurora/GX uses reversed-Z (1.0 = near, 0.0 = far).
        // Include the 0.0 far plane so skyboxes and cleared-background pixels
        // survive a render target that is also cleared to reversed-Z far.
        depthStencil.depthCompare = wgpu::CompareFunction::GreaterEqual;

        wgpu::RenderPipelineDescriptor rpDesc{};
        rpDesc.label = "StereoParallax Render Pipeline";
        rpDesc.layout = pipelineLayout;
        rpDesc.vertex.module = shaderModule;
        rpDesc.vertex.entryPoint = "vs_main";
        rpDesc.vertex.bufferCount = 1;
        rpDesc.vertex.buffers = &vbLayout;
        rpDesc.fragment = &fragmentState;
        rpDesc.depthStencil = &depthStencil;
        rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        rpDesc.primitive.cullMode = wgpu::CullMode::None;

        pipeline = device.CreateRenderPipeline(&rpDesc);
    }

    void SetupEyeResources(EyeResources& eye, IOSurfaceRef colorSurface, uint32_t width, uint32_t height) {
        wgpu::BufferDescriptor ubDesc{};
        ubDesc.label = "StereoParallax Eye Uniforms";
        ubDesc.size = sizeof(UniformBufferData);
        ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        eye.uniformBuffer = device.CreateBuffer(&ubDesc);

#if defined(__APPLE__)
        if (colorSurface != nullptr) {
            wgpu::SharedTextureMemoryIOSurfaceDescriptor colorMemDesc{};
            colorMemDesc.ioSurface = colorSurface;
            colorMemDesc.allowStorageBinding = false;

            wgpu::SharedTextureMemoryDescriptor descColor{};
            descColor.label = "StereoParallax Color SharedMem";
            descColor.nextInChain = &colorMemDesc;
            eye.colorSharedMem = device.ImportSharedTextureMemory(&descColor);

            wgpu::TextureDescriptor colorTexDesc{};
            colorTexDesc.label = "StereoParallax Color Texture";
            colorTexDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                                 wgpu::TextureUsage::CopyDst;
            colorTexDesc.size = { width, height, 1 };
            colorTexDesc.format = wgpu::TextureFormat::BGRA8Unorm;
            eye.colorTexture = eye.colorSharedMem.CreateTexture(&colorTexDesc);
            eye.colorView = eye.colorTexture.CreateView();
        }
#endif

        if (!eye.colorTexture) {
            wgpu::TextureDescriptor colorTexDesc{};
            colorTexDesc.label = "StereoParallax Fallback Color Texture";
            colorTexDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                                 wgpu::TextureUsage::CopyDst;
            colorTexDesc.size = { width, height, 1 };
            colorTexDesc.format = wgpu::TextureFormat::BGRA8Unorm;
            eye.colorTexture = device.CreateTexture(&colorTexDesc);
            eye.colorView = eye.colorTexture.CreateView();
        }

        wgpu::TextureDescriptor depthTexDesc{};
        depthTexDesc.label = "StereoParallax Depth Render Target";
        depthTexDesc.usage = wgpu::TextureUsage::RenderAttachment;
        depthTexDesc.size = { width, height, 1 };
        depthTexDesc.format = wgpu::TextureFormat::Depth32Float;
        eye.depthTexture = device.CreateTexture(&depthTexDesc);
        eye.depthView = eye.depthTexture.CreateView();
    }

    void UpdateBindGroups(bool useTrueStereo) {
        auto leftColorView = useTrueStereo ? leftCapture.colorView
                                           : aurora::webgpu::get_present_source_view();
        auto rightColorView = useTrueStereo ? rightCapture.colorView : leftColorView;
        auto colorSampler = aurora::webgpu::get_present_sampler();
        auto leftDepthView = useTrueStereo ? leftCapture.depthView
                                           : aurora::webgpu::get_depth_view();
        auto rightDepthView = useTrueStereo ? rightCapture.depthView : leftDepthView;
        auto depthSampler = depthNearestSampler;

        if (!leftColorView || !rightColorView || !leftDepthView || !rightDepthView) {
            return;
        }

        auto createBg = [&](EyeResources& eye, const wgpu::TextureView& colorView,
                            const wgpu::TextureView& depthView) {
            if (eye.bindGroup && eye.boundColorView == colorView.Get() &&
                eye.boundDepthView == depthView.Get() &&
                eye.boundColorSampler == colorSampler.Get() &&
                eye.boundDepthSampler == depthSampler.Get()) {
                return;
            }

            std::array<wgpu::BindGroupEntry, 5> bgEntries{};
            bgEntries[0].binding = 0;
            bgEntries[0].buffer = eye.uniformBuffer;
            bgEntries[0].size = sizeof(UniformBufferData);

            bgEntries[1].binding = 1;
            bgEntries[1].textureView = colorView;

            bgEntries[2].binding = 2;
            bgEntries[2].sampler = colorSampler;

            bgEntries[3].binding = 3;
            bgEntries[3].textureView = depthView;

            bgEntries[4].binding = 4;
            bgEntries[4].sampler = depthSampler;

            wgpu::BindGroupDescriptor bgDesc{};
            bgDesc.label = "StereoParallax BG";
            bgDesc.layout = bindGroupLayout;
            bgDesc.entryCount = static_cast<uint32_t>(bgEntries.size());
            bgDesc.entries = bgEntries.data();
            eye.bindGroup = device.CreateBindGroup(&bgDesc);
            eye.boundColorView = colorView.Get();
            eye.boundDepthView = depthView.Get();
            eye.boundColorSampler = colorSampler.Get();
            eye.boundDepthSampler = depthSampler.Get();
        };

        createBg(leftEye, leftColorView, leftDepthView);
        createBg(rightEye, rightColorView, rightDepthView);
    }
};

static StereoParallaxPass g_stereoPass;

StereoParallaxPass* GetStereoParallaxPass() {
    return &g_stereoPass;
}

StereoParallaxPass::StereoParallaxPass() : m_impl(std::make_unique<Impl>()) {}
StereoParallaxPass::~StereoParallaxPass() {
    Shutdown();
}

bool StereoParallaxPass::Initialize(uint32_t width, uint32_t height) {
    if (m_initialized && m_width == width && m_height == height) {
        return true;
    }

    Shutdown();

    m_width = width;
    m_height = height;

    m_impl->device = aurora::webgpu::get_device();
    m_impl->queue = aurora::webgpu::get_queue();

    if (!m_impl->device || !m_impl->queue) {
        return false;
    }

#if defined(__APPLE__)
    m_leftColorSurface = CreateIOSurfaceHelper(width, height, kCVPixelFormatType_32BGRA, 4);
    m_rightColorSurface = CreateIOSurfaceHelper(width, height, kCVPixelFormatType_32BGRA, 4);
#endif

    m_impl->CreateGridMesh();
    m_impl->CreatePipeline();

    m_impl->SetupEyeResources(m_impl->leftEye, m_leftColorSurface, width, height);
    m_impl->SetupEyeResources(m_impl->rightEye, m_rightColorSurface, width, height);

    m_initialized = true;
    return true;
}

void StereoParallaxPass::Shutdown() {
    if (!m_initialized) return;

    m_impl->leftEye = {};
    m_impl->rightEye = {};
    m_impl->vertexBuffer = {};
    m_impl->indexBuffer = {};
    m_impl->pipeline = {};
    m_impl->bindGroupLayout = {};
    m_impl->depthNearestSampler = {};
    m_impl->leftCapture = {};
    m_impl->rightCapture = {};
    m_impl->hasTrueStereoFrame = false;

#if defined(__APPLE__)
    std::scoped_lock lock(m_appleFrameMutex);
    m_leftReadyEvent = nullptr;
    m_rightReadyEvent = nullptr;
    m_leftReadyValue = 0;
    m_rightReadyValue = 0;
    m_readyGeneration = 0;
    if (m_leftColorSurface) { CFRelease(m_leftColorSurface); m_leftColorSurface = nullptr; }
    if (m_rightColorSurface) { CFRelease(m_rightColorSurface); m_rightColorSurface = nullptr; }
    if (m_leftDepthSurface) { CFRelease(m_leftDepthSurface); m_leftDepthSurface = nullptr; }
    if (m_rightDepthSurface) { CFRelease(m_rightDepthSurface); m_rightDepthSurface = nullptr; }
#endif

    m_initialized = false;
}

#if defined(__APPLE__)
bool StereoParallaxPass::GetAppleFramePair(StereoParallaxAppleFramePair& frames) const {
    std::scoped_lock lock(m_appleFrameMutex);
    if (!m_leftColorSurface || !m_rightColorSurface || !m_leftReadyEvent || !m_rightReadyEvent ||
        m_leftReadyValue == 0 || m_rightReadyValue == 0 || m_readyGeneration == 0) {
        return false;
    }
    frames.left = {m_leftColorSurface, m_leftReadyEvent, m_leftReadyValue};
    frames.right = {m_rightColorSurface, m_rightReadyEvent, m_rightReadyValue};
    frames.generation = m_readyGeneration;
    return true;
}
#endif

void StereoParallaxPass::Resize(uint32_t width, uint32_t height) {
    if (m_width == width && m_height == height) return;
    Initialize(width, height);
}

void StereoParallaxPass::SubmitTrueStereoFrame(const aurora::gfx::CapturedFrame& left,
                                                const aurora::gfx::CapturedFrame& right) {
    if (!left.colorTexture || !left.colorView || !left.depthView ||
        !right.colorTexture || !right.colorView || !right.depthView ||
        left.width == 0 || left.height == 0 ||
        left.width != right.width || left.height != right.height) {
        return;
    }
    m_impl->leftCapture = left;
    m_impl->rightCapture = right;
    m_impl->hasTrueStereoFrame = true;
}

void StereoParallaxPass::Render(void* encoderPtr) {
    if (!m_enabled.load(std::memory_order_relaxed) || !encoderPtr) {
        return;
    }

    uint32_t curWidth = aurora::webgpu::get_present_width();
    uint32_t curHeight = aurora::webgpu::get_present_height();
    if (curWidth == 0 || curHeight == 0) {
        return;
    }

    if (!m_initialized || m_width != curWidth || m_height != curHeight) {
        if (!Initialize(curWidth, curHeight)) {
#if defined(__APPLE__)
            os_log_error(OS_LOG_DEFAULT, "[Dusklight] StereoParallaxPass::Initialize failed for size (%u, %u)!", curWidth, curHeight);
#endif
            return;
        }
#if defined(__APPLE__)
        os_log(OS_LOG_DEFAULT, "[Dusklight] StereoParallaxPass::Initialize SUCCEEDED for size (%u, %u)", curWidth, curHeight);
#endif
    }

    const bool useTrueStereo = m_impl->hasTrueStereoFrame &&
        m_impl->leftCapture.width == curWidth && m_impl->leftCapture.height == curHeight;
    m_impl->UpdateBindGroups(useTrueStereo);

    if (!m_impl->pipeline || !m_impl->vertexBuffer || !m_impl->indexBuffer ||
        !m_impl->leftEye.bindGroup || !m_impl->rightEye.bindGroup ||
        !m_impl->leftEye.colorView || !m_impl->rightEye.colorView ||
        !m_impl->leftEye.depthView || !m_impl->rightEye.depthView) {
        return;
    }

    float eyeSep = useTrueStereo ? 0.0f : m_eyeSeparation.load(std::memory_order_relaxed);
    float convDepth = m_convergenceDepth.load(std::memory_order_relaxed);
    float depthSc = m_depthScale.load(std::memory_order_relaxed);

    auto* cmdEncoder = static_cast<wgpu::CommandEncoder*>(encoderPtr);

    auto renderEye = [&](EyeResources& eye, float eyeSign, AppleReadyFence& readyFence) {
        UniformBufferData uboData{
            .eye_sign = eyeSign,
            .eye_separation = eyeSep,
            .convergence_depth = convDepth,
            .depth_scale = depthSc,
        };
        m_impl->queue.WriteBuffer(eye.uniformBuffer, 0, &uboData, sizeof(uboData));

#if defined(__APPLE__)
        wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{};
        if (eye.colorSharedMem) {
            eye.colorSharedMem.BeginAccess(eye.colorTexture, &beginDesc);
        }
#endif

        wgpu::RenderPassColorAttachment colorAttachment{};
        colorAttachment.view = eye.colorView;
        colorAttachment.loadOp = wgpu::LoadOp::Clear;
        colorAttachment.storeOp = wgpu::StoreOp::Store;
        colorAttachment.clearValue = { 0.0, 0.0, 0.0, 1.0 };

        wgpu::RenderPassDepthStencilAttachment depthAttachment{};
        depthAttachment.view = eye.depthView;
        depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
        depthAttachment.depthStoreOp = wgpu::StoreOp::Store;
        depthAttachment.depthClearValue = 0.0f;

        wgpu::RenderPassDescriptor passDesc{};
        passDesc.label = "StereoParallax RenderPass";
        passDesc.colorAttachmentCount = 1;
        passDesc.colorAttachments = &colorAttachment;
        passDesc.depthStencilAttachment = &depthAttachment;

        auto pass = cmdEncoder->BeginRenderPass(&passDesc);
        pass.SetPipeline(m_impl->pipeline);
        pass.SetBindGroup(0, eye.bindGroup);
        pass.SetVertexBuffer(0, m_impl->vertexBuffer);
        pass.SetIndexBuffer(m_impl->indexBuffer, wgpu::IndexFormat::Uint32);
        pass.DrawIndexed(m_impl->indexCount);
        pass.End();

#if defined(__APPLE__)
        wgpu::SharedTextureMemoryEndAccessState endState{};
        if (eye.colorSharedMem) {
            eye.colorSharedMem.EndAccess(eye.colorTexture, &endState);
            if (endState.fenceCount > 0 && endState.signaledValueCount > 0) {
                wgpu::SharedFenceMTLSharedEventExportInfo metalEventInfo{};
                wgpu::SharedFenceExportInfo fenceInfo{};
                fenceInfo.nextInChain = &metalEventInfo;
                endState.fences[0].ExportInfo(&fenceInfo);
                if (fenceInfo.type == wgpu::SharedFenceType::MTLSharedEvent && metalEventInfo.sharedEvent) {
                    readyFence.event = metalEventInfo.sharedEvent;
                    readyFence.value = endState.signaledValues[0];
                }
            }
        }
#endif
    };

    AppleReadyFence leftReady;
    AppleReadyFence rightReady;
    renderEye(m_impl->leftEye, -1.0f, leftReady);
    renderEye(m_impl->rightEye, 1.0f, rightReady);
    m_impl->hasTrueStereoFrame = false;

#if defined(__APPLE__)
    if (leftReady.event && rightReady.event && leftReady.value > 0 && rightReady.value > 0) {
        std::scoped_lock lock(m_appleFrameMutex);
        m_leftReadyEvent = leftReady.event;
        m_leftReadyValue = leftReady.value;
        m_rightReadyEvent = rightReady.event;
        m_rightReadyValue = rightReady.value;
        ++m_readyGeneration;
    }
#endif
}

void StereoParallaxPass::AdjustEyeSeparation(float delta) {
    float cur = m_eyeSeparation.load(std::memory_order_relaxed);
    float newVal = std::clamp(cur + delta, 0.0f, 0.20f);
    m_eyeSeparation.store(newVal, std::memory_order_relaxed);
#if defined(__APPLE__)
    os_log(OS_LOG_DEFAULT, "[DuskStereo] Adjusted eyeSeparation -> %.4f", newVal);
#endif
}

void StereoParallaxPass::AdjustConvergenceDepth(float delta) {
    float cur = m_convergenceDepth.load(std::memory_order_relaxed);
    float newVal = std::clamp(cur + delta, 0.0f, 1.0f);
    m_convergenceDepth.store(newVal, std::memory_order_relaxed);
#if defined(__APPLE__)
    os_log(OS_LOG_DEFAULT, "[DuskStereo] Adjusted convergenceDepth -> %.4f", newVal);
#endif
}

void StereoParallaxPass::ToggleEnabled() {
    bool cur = m_enabled.load(std::memory_order_relaxed);
    m_enabled.store(!cur, std::memory_order_relaxed);
#if defined(__APPLE__)
    os_log(OS_LOG_DEFAULT, "[DuskStereo] Parallax enabled -> %d", !cur ? 1 : 0);
#endif
}

void InitializeStereoParallaxHook() {
    aurora_set_post_render_callback([](void* encoder, void* userdata) {
        auto* pass = static_cast<StereoParallaxPass*>(userdata);
        if (pass) {
            pass->Render(encoder);
        }
    }, GetStereoParallaxPass());
}

} // namespace dusk::gfx
