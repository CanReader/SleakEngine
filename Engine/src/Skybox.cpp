#include <Runtime/Skybox.hpp>
#include <Runtime/Texture.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Graphics/RenderCommandQueue.hpp>
#include <Graphics/RenderContext.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include <Graphics/Vertex.hpp>
#include <Camera/Camera.hpp>
#include <Logger.hpp>

#include <vector>

namespace Sleak {

struct alignas(16) SkyboxCBData {
    Math::Matrix4 ViewProjection;
};

Skybox::Skybox(const std::array<std::string, 6>& facePaths)
    : m_mode(SkyboxMode::Cubemap), m_facePaths(facePaths) {}

Skybox::Skybox(const std::string& panoramaPath)
    : m_mode(SkyboxMode::Panorama), m_panoramaPath(panoramaPath) {}

Skybox::Skybox(float topR, float topG, float topB,
               float midR, float midG, float midB,
               float botR, float botG, float botB)
    : m_mode(SkyboxMode::Gradient),
      m_topR(topR), m_topG(topG), m_topB(topB),
      m_midR(midR), m_midG(midG), m_midB(midB),
      m_botR(botR), m_botG(botG), m_botB(botB) {}

Skybox::Skybox()
    : m_mode(SkyboxMode::Default),
      m_panoramaPath("assets/textures/default_skybox.jpg") {}

Skybox::~Skybox() {}

void Skybox::Initialize() {
    if (m_initialized) return;

    // Create cube mesh for skybox (unit cube, vertices only need position)
    MeshData cube;
    cube.vertices.AddVertex(Vertex(-1, -1,  1, 0,0,0, 0,0,0,0, 0,0));  // 0
    cube.vertices.AddVertex(Vertex(-1,  1,  1, 0,0,0, 0,0,0,0, 0,0));  // 1
    cube.vertices.AddVertex(Vertex( 1,  1,  1, 0,0,0, 0,0,0,0, 0,0));  // 2
    cube.vertices.AddVertex(Vertex( 1, -1,  1, 0,0,0, 0,0,0,0, 0,0));  // 3
    cube.vertices.AddVertex(Vertex(-1, -1, -1, 0,0,0, 0,0,0,0, 0,0));  // 4
    cube.vertices.AddVertex(Vertex(-1,  1, -1, 0,0,0, 0,0,0,0, 0,0));  // 5
    cube.vertices.AddVertex(Vertex( 1,  1, -1, 0,0,0, 0,0,0,0, 0,0));  // 6
    cube.vertices.AddVertex(Vertex( 1, -1, -1, 0,0,0, 0,0,0,0, 0,0));  // 7

    // Winding order: visible from inside the cube
    cube.indices = {
        0, 2, 1, 0, 3, 2,   // Front  (+Z)
        4, 5, 6, 4, 6, 7,   // Back   (-Z)
        4, 1, 5, 4, 0, 1,   // Left   (-X)
        3, 6, 2, 3, 7, 6,   // Right  (+X)
        1, 6, 5, 1, 2, 6,   // Top    (+Y)
        4, 3, 0, 4, 7, 3    // Bottom (-Y)
    };

    m_vertexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Vertex,
        cube.vertices.GetSizeInBytes(),
        cube.vertices.GetRawData()));

    m_indexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Index,
        cube.indices.GetByteSize(),
        cube.indices.GetRawData()));

    // Constant buffer for ViewProjection matrix
    SkyboxCBData cbData;
    cbData.ViewProjection = Math::Matrix4::Identity();
    m_constantBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Constant,
        sizeof(SkyboxCBData),
        &cbData));
    m_constantBuffer->SetSlot(0);

    // Create shader (skybox.hlsl resolves to backend-specific shaders)
    m_shader = RefPtr(RenderEngine::ResourceManager::CreateShader(
        "assets/shaders/skybox.hlsl"));
    if (!m_shader.IsValid()) {
        SLEAK_ERROR("Skybox: Failed to create skybox shader");
        return;
    }

    // Create cubemap texture based on mode
    if (m_mode == SkyboxMode::Cubemap) {
        m_cubemapTexture = RefPtr(
            RenderEngine::ResourceManager::CreateCubemapTexture(m_facePaths));
        if (!m_cubemapTexture.IsValid()) {
            SLEAK_WARN("Skybox: Failed to load cubemap faces, falling back to panorama");
            m_mode = SkyboxMode::Default;
            m_panoramaPath = "assets/textures/default_skybox.jpg";
        }
    }

    if (m_mode == SkyboxMode::Panorama || m_mode == SkyboxMode::Default) {
        m_cubemapTexture = RefPtr(
            RenderEngine::ResourceManager::CreateCubemapTextureFromPanorama(m_panoramaPath));
        if (!m_cubemapTexture.IsValid()) {
            SLEAK_WARN("Skybox: Failed to load panorama '{}', falling back to gradient",
                        m_panoramaPath);
            m_mode = SkyboxMode::Gradient;
        }
    }

    if (m_mode == SkyboxMode::Gradient) {
        SLEAK_INFO("Skybox: Using gradient mode (no cubemap texture)");
    }

    m_initialized = true;
    SLEAK_INFO("Skybox initialized successfully (mode: {})",
               m_mode == SkyboxMode::Cubemap ? "cubemap" :
               m_mode == SkyboxMode::Panorama ? "panorama" :
               m_mode == SkyboxMode::Gradient ? "gradient" : "default");
}

void Skybox::Render() {
    if (!m_initialized) return;
    if (!m_shader.IsValid()) return;

    // Build view-projection matrix with translation zeroed out
    Math::Matrix4 view = Camera::GetMainViewMatrix();
    view(3, 0) = 0.0f;
    view(3, 1) = 0.0f;
    view(3, 2) = 0.0f;

    Math::Matrix4 proj = Camera::GetMainProjectionMatrix();
    Math::Matrix4 viewProj = view * proj;

    // Update constant buffer data
    SkyboxCBData cbData;
    cbData.ViewProjection = viewProj;
    m_constantBuffer->Update(&cbData, sizeof(SkyboxCBData));

    // Capture resources for the lambda
    auto shader = m_shader;
    auto vb = m_vertexBuffer;
    auto ib = m_indexBuffer;
    auto cb = m_constantBuffer;
    auto cubemap = m_cubemapTexture;

    RenderEngine::RenderCommandQueue::GetInstance()->SubmitCustomCommand(
        [shader, vb, ib, cb, cubemap](RenderEngine::RenderContext* ctx) {
            // Switch to skybox pipeline/state (handles Vulkan pipeline swap)
            ctx->BeginSkyboxPass();

            // Set depth state: write disabled, test LEQUAL, no culling
            ctx->SetDepthWrite(false);
            ctx->SetDepthCompare(RenderEngine::DepthCompare::LessEqual);
            ctx->SetCullEnabled(false);

            // Bind shader (no-op for Vulkan, used by OpenGL)
            shader->bind();

            // Bind cubemap texture to slot 0
            ctx->BindTexture(cubemap, 0);

            // Bind constant buffer
            ctx->BindConstantBuffer(cb, 0);

            // Bind vertex and index buffers, then draw
            ctx->BindVertexBuffer(vb, 0);
            ctx->BindIndexBuffer(ib, 0);
            ctx->DrawIndexed(36);

            // Restore to main pipeline/state
            ctx->EndSkyboxPass();

            // Restore depth state for OpenGL
            ctx->SetDepthWrite(true);
            ctx->SetDepthCompare(RenderEngine::DepthCompare::Less);
            ctx->SetCullEnabled(true);
        });
}

} // namespace Sleak
