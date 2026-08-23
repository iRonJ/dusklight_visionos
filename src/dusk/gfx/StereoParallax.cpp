#include "dusk/gfx/StereoParallax.hpp"

#include <aurora/aurora.h>
#include <aurora/webgpu.hpp>
#include <webgpu/webgpu_cpp.h>

#include <cmath>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <IOSurface/IOSurfaceRef.h>
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
    @location(1) out_depth: f32,
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

    let raw_depth = textureSampleLevel(depth_texture, depth_sampler, input.position, 0.0);

    // Reprojection parallax disparity:
    // Objects nearer than the convergence plane must shift toward the temple side of
    // the eye that renders them (e.g. the left eye's image shifts right for near
    // objects) to produce correct "crossed disparity" -- matching how a real stereo
    // camera pair converged on convergence_depth would see them.
    let disparity = uniforms.eye_sign * uniforms.eye_separation * (raw_depth - uniforms.convergence_depth) * uniforms.depth_scale;

    let ndc_x = (input.position.x * 2.0 - 1.0) + disparity * 2.0;
    let ndc_y = 1.0 - input.position.y * 2.0;

    out.clip_position = vec4<f32>(ndc_x, ndc_y, raw_depth, 1.0);
    out.out_depth = raw_depth;
    return out;
}

struct FragmentOutput {
    @location(0) color: vec4<f32>,
    @builtin(frag_depth) depth: f32,
};

@fragment
fn fs_main(in: VertexOutput) -> FragmentOutput {
    var out: FragmentOutput;
    out.color = textureSample(color_texture, color_sampler, in.uv);
    out.depth = in.out_depth;
    return out;
}
)""";

constexpr uint32_t kGridDivisionsX = 96;
constexpr uint32_t kGridDivisionsY = 64;

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

    bool resourcesReady = false;

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
        bglEntries[3].visibility = wgpu::ShaderStage::Vertex;
        bglEntries[3].texture.sampleType = wgpu::TextureSampleType::Depth;
        bglEntries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;

        // Depth sampler
        bglEntries[4].binding = 4;
        bglEntries[4].visibility = wgpu::ShaderStage::Vertex;
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
        // The displaced grid can fold over itself near depth discontinuities (a near
        // object's edge stretching over the background). Depth-test so the nearer
        // (correct) surface wins instead of whichever triangle happens to rasterize last.
        depthStencil.depthCompare = wgpu::CompareFunction::Less;

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

    void SetupEyeResources(EyeResources& eye, IOSurfaceRef colorSurface, IOSurfaceRef depthSurface, uint32_t width, uint32_t height) {
        wgpu::BufferDescriptor ubDesc{};
        ubDesc.label = "StereoParallax Eye Uniforms";
        ubDesc.size = sizeof(UniformBufferData);
        ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        eye.uniformBuffer = device.CreateBuffer(&ubDesc);

#if defined(__APPLE__)
        if (colorSurface != nullptr && depthSurface != nullptr) {
            wgpu::SharedTextureMemoryIOSurfaceDescriptor colorMemDesc{};
            colorMemDesc.ioSurface = colorSurface;
            colorMemDesc.allowStorageBinding = false;

            wgpu::SharedTextureMemoryDescriptor descColor{};
            descColor.label = "StereoParallax Color SharedMem";
            descColor.nextInChain = &colorMemDesc;
            eye.colorSharedMem = device.ImportSharedTextureMemory(&descColor);

            wgpu::TextureDescriptor colorTexDesc{};
            colorTexDesc.label = "StereoParallax Color Texture";
            colorTexDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            colorTexDesc.size = { width, height, 1 };
            colorTexDesc.format = wgpu::TextureFormat::BGRA8Unorm;
            eye.colorTexture = eye.colorSharedMem.CreateTexture(&colorTexDesc);
            eye.colorView = eye.colorTexture.CreateView();

            wgpu::SharedTextureMemoryIOSurfaceDescriptor depthMemDesc{};
            depthMemDesc.ioSurface = depthSurface;
            depthMemDesc.allowStorageBinding = false;

            wgpu::SharedTextureMemoryDescriptor descDepth{};
            descDepth.label = "StereoParallax Depth SharedMem";
            descDepth.nextInChain = &depthMemDesc;
            eye.depthSharedMem = device.ImportSharedTextureMemory(&descDepth);

            wgpu::TextureDescriptor depthTexDesc{};
            depthTexDesc.label = "StereoParallax Depth Texture";
            depthTexDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            depthTexDesc.size = { width, height, 1 };
            depthTexDesc.format = wgpu::TextureFormat::Depth32Float;
            eye.depthTexture = eye.depthSharedMem.CreateTexture(&depthTexDesc);
            eye.depthView = eye.depthTexture.CreateView();
        }
#endif

        if (!eye.colorTexture) {
            wgpu::TextureDescriptor colorTexDesc{};
            colorTexDesc.label = "StereoParallax Fallback Color Texture";
            colorTexDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            colorTexDesc.size = { width, height, 1 };
            colorTexDesc.format = wgpu::TextureFormat::BGRA8Unorm;
            eye.colorTexture = device.CreateTexture(&colorTexDesc);
            eye.colorView = eye.colorTexture.CreateView();

            wgpu::TextureDescriptor depthTexDesc{};
            depthTexDesc.label = "StereoParallax Fallback Depth Texture";
            depthTexDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            depthTexDesc.size = { width, height, 1 };
            depthTexDesc.format = wgpu::TextureFormat::Depth32Float;
            eye.depthTexture = device.CreateTexture(&depthTexDesc);
            eye.depthView = eye.depthTexture.CreateView();
        }
    }

    void UpdateBindGroups() {
        auto colorView = aurora::webgpu::get_present_source_view();
        auto colorSampler = aurora::webgpu::get_present_sampler();
        auto depthView = aurora::webgpu::get_depth_view();
        auto depthSampler = depthNearestSampler;

        if (!colorView || !depthView) {
            return;
        }

        auto createBg = [&](EyeResources& eye) {
            std::vector<wgpu::BindGroupEntry> bgEntries(5);
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
        };

        createBg(leftEye);
        createBg(rightEye);
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
    m_leftDepthSurface = CreateIOSurfaceHelper(width, height, kCVPixelFormatType_DepthFloat32, 4);
    m_rightDepthSurface = CreateIOSurfaceHelper(width, height, kCVPixelFormatType_DepthFloat32, 4);
#endif

    m_impl->CreateGridMesh();
    m_impl->CreatePipeline();

    m_impl->SetupEyeResources(m_impl->leftEye, m_leftColorSurface, m_leftDepthSurface, width, height);
    m_impl->SetupEyeResources(m_impl->rightEye, m_rightColorSurface, m_rightDepthSurface, width, height);

    m_impl->resourcesReady = true;
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
    m_impl->resourcesReady = false;

#if defined(__APPLE__)
    if (m_leftColorSurface) { CFRelease(m_leftColorSurface); m_leftColorSurface = nullptr; }
    if (m_rightColorSurface) { CFRelease(m_rightColorSurface); m_rightColorSurface = nullptr; }
    if (m_leftDepthSurface) { CFRelease(m_leftDepthSurface); m_leftDepthSurface = nullptr; }
    if (m_rightDepthSurface) { CFRelease(m_rightDepthSurface); m_rightDepthSurface = nullptr; }
#endif

    m_initialized = false;
}

void StereoParallaxPass::Resize(uint32_t width, uint32_t height) {
    if (m_width == width && m_height == height) return;
    Initialize(width, height);
}

void StereoParallaxPass::Render(void* encoderPtr) {
    if (!m_settings.enabled || !encoderPtr) {
        return;
    }

    uint32_t curWidth = aurora::webgpu::get_present_width();
    uint32_t curHeight = aurora::webgpu::get_present_height();
    if (curWidth == 0 || curHeight == 0) {
        return;
    }

    if (!m_initialized || m_width != curWidth || m_height != curHeight) {
        if (!Initialize(curWidth, curHeight)) {
            return;
        }
    }

    m_impl->UpdateBindGroups();

    auto* cmdEncoder = static_cast<wgpu::CommandEncoder*>(encoderPtr);

    auto renderEye = [&](EyeResources& eye, float eyeSign) {
        UniformBufferData uboData{
            .eye_sign = eyeSign,
            .eye_separation = m_settings.eyeSeparation,
            .convergence_depth = m_settings.convergenceDepth,
            .depth_scale = m_settings.depthScale,
        };
        m_impl->queue.WriteBuffer(eye.uniformBuffer, 0, &uboData, sizeof(uboData));

#if defined(__APPLE__)
        wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{};
        if (eye.colorSharedMem) {
            eye.colorSharedMem.BeginAccess(eye.colorTexture, &beginDesc);
        }
        if (eye.depthSharedMem) {
            eye.depthSharedMem.BeginAccess(eye.depthTexture, &beginDesc);
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
        depthAttachment.depthClearValue = 1.0f;

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
        }
        if (eye.depthSharedMem) {
            eye.depthSharedMem.EndAccess(eye.depthTexture, &endState);
        }
#endif
    };

    renderEye(m_impl->leftEye, -1.0f);
    renderEye(m_impl->rightEye, 1.0f);
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
