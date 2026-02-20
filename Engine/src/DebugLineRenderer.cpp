#include <Debug/DebugLineRenderer.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Graphics/RenderCommandQueue.hpp>
#include <Graphics/RenderContext.hpp>
#include <Camera/Camera.hpp>
#include <Logger.hpp>
#include <cmath>

namespace Sleak {

struct alignas(16) DebugLineCBData {
    Math::Matrix4 ViewProjection;
};

// Static member definitions
std::vector<Vertex> DebugLineRenderer::s_vertices;
RefPtr<RenderEngine::BufferBase> DebugLineRenderer::s_vertexBuffer;
RefPtr<RenderEngine::Shader> DebugLineRenderer::s_shader;
RefPtr<RenderEngine::BufferBase> DebugLineRenderer::s_constantBuffer;
bool DebugLineRenderer::s_enabled = false;
bool DebugLineRenderer::s_initialized = false;

void DebugLineRenderer::Initialize() {
    if (s_initialized) return;

    s_shader = RefPtr(RenderEngine::ResourceManager::CreateShader(
        "assets/shaders/debug_line.hlsl"));
    if (!s_shader.IsValid()) {
        SLEAK_ERROR("DebugLineRenderer: Failed to create debug line shader");
        return;
    }

    // Create dynamic vertex buffer (pre-allocate for MAX_VERTICES)
    std::vector<Vertex> emptyVerts(MAX_VERTICES);
    s_vertexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Vertex,
        MAX_VERTICES * sizeof(Vertex),
        emptyVerts.data()));

    // Create constant buffer for ViewProjection matrix
    DebugLineCBData cbData;
    cbData.ViewProjection = Math::Matrix4::Identity();
    s_constantBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
        RenderEngine::BufferType::Constant,
        sizeof(DebugLineCBData),
        &cbData));
    s_constantBuffer->SetSlot(0);

    s_vertices.reserve(MAX_VERTICES);
    s_initialized = true;
    SLEAK_INFO("DebugLineRenderer initialized");
}

void DebugLineRenderer::Shutdown() {
    s_vertices.clear();
    s_vertexBuffer.reset();
    s_shader.reset();
    s_constantBuffer.reset();
    s_initialized = false;
}

void DebugLineRenderer::DrawLine(const Math::Vector3D& start, const Math::Vector3D& end,
                                  float r, float g, float b, float a) {
    if (s_vertices.size() + 2 > MAX_VERTICES) return;

    Vertex v1{};
    v1.px = start.GetX(); v1.py = start.GetY(); v1.pz = start.GetZ();
    v1.r = r; v1.g = g; v1.b = b; v1.a = a;

    Vertex v2{};
    v2.px = end.GetX(); v2.py = end.GetY(); v2.pz = end.GetZ();
    v2.r = r; v2.g = g; v2.b = b; v2.a = a;

    s_vertices.push_back(v1);
    s_vertices.push_back(v2);
}

void DebugLineRenderer::DrawAABB(const Physics::AABB& aabb,
                                  float r, float g, float b, float a) {
    Math::Vector3D mn = aabb.min;
    Math::Vector3D mx = aabb.max;

    // 8 corners
    Math::Vector3D c000(mn.GetX(), mn.GetY(), mn.GetZ());
    Math::Vector3D c001(mn.GetX(), mn.GetY(), mx.GetZ());
    Math::Vector3D c010(mn.GetX(), mx.GetY(), mn.GetZ());
    Math::Vector3D c011(mn.GetX(), mx.GetY(), mx.GetZ());
    Math::Vector3D c100(mx.GetX(), mn.GetY(), mn.GetZ());
    Math::Vector3D c101(mx.GetX(), mn.GetY(), mx.GetZ());
    Math::Vector3D c110(mx.GetX(), mx.GetY(), mn.GetZ());
    Math::Vector3D c111(mx.GetX(), mx.GetY(), mx.GetZ());

    // Bottom face
    DrawLine(c000, c100, r, g, b, a);
    DrawLine(c100, c101, r, g, b, a);
    DrawLine(c101, c001, r, g, b, a);
    DrawLine(c001, c000, r, g, b, a);

    // Top face
    DrawLine(c010, c110, r, g, b, a);
    DrawLine(c110, c111, r, g, b, a);
    DrawLine(c111, c011, r, g, b, a);
    DrawLine(c011, c010, r, g, b, a);

    // Vertical edges
    DrawLine(c000, c010, r, g, b, a);
    DrawLine(c100, c110, r, g, b, a);
    DrawLine(c101, c111, r, g, b, a);
    DrawLine(c001, c011, r, g, b, a);
}

void DebugLineRenderer::DrawSphere(const Math::Vector3D& center, float radius,
                                    float r, float g, float b, float a,
                                    int segments) {
    float step = 2.0f * 3.14159265f / static_cast<float>(segments);

    // XY plane circle
    for (int i = 0; i < segments; ++i) {
        float angle0 = step * i;
        float angle1 = step * (i + 1);
        Math::Vector3D p0(center.GetX() + radius * std::cos(angle0),
                          center.GetY() + radius * std::sin(angle0),
                          center.GetZ());
        Math::Vector3D p1(center.GetX() + radius * std::cos(angle1),
                          center.GetY() + radius * std::sin(angle1),
                          center.GetZ());
        DrawLine(p0, p1, r, g, b, a);
    }

    // XZ plane circle
    for (int i = 0; i < segments; ++i) {
        float angle0 = step * i;
        float angle1 = step * (i + 1);
        Math::Vector3D p0(center.GetX() + radius * std::cos(angle0),
                          center.GetY(),
                          center.GetZ() + radius * std::sin(angle0));
        Math::Vector3D p1(center.GetX() + radius * std::cos(angle1),
                          center.GetY(),
                          center.GetZ() + radius * std::sin(angle1));
        DrawLine(p0, p1, r, g, b, a);
    }

    // YZ plane circle
    for (int i = 0; i < segments; ++i) {
        float angle0 = step * i;
        float angle1 = step * (i + 1);
        Math::Vector3D p0(center.GetX(),
                          center.GetY() + radius * std::cos(angle0),
                          center.GetZ() + radius * std::sin(angle0));
        Math::Vector3D p1(center.GetX(),
                          center.GetY() + radius * std::cos(angle1),
                          center.GetZ() + radius * std::sin(angle1));
        DrawLine(p0, p1, r, g, b, a);
    }
}

void DebugLineRenderer::DrawCapsule(const Physics::BoundingCapsule& capsule,
                                     float r, float g, float b, float a,
                                     int segments) {
    Math::Vector3D pointA = capsule.GetPointA();
    Math::Vector3D pointB = capsule.GetPointB();
    float radius = capsule.radius;

    // Draw connecting lines between the two hemispheres
    float step = 2.0f * 3.14159265f / static_cast<float>(segments);

    // Determine the perpendicular axes based on capsule axis
    int ax = capsule.axis;
    int ax1 = (ax + 1) % 3;
    int ax2 = (ax + 2) % 3;

    for (int i = 0; i < segments; ++i) {
        float angle = step * i;
        float c1 = radius * std::cos(angle);
        float c2 = radius * std::sin(angle);

        Math::Vector3D offset(0, 0, 0);
        if (ax1 == 0) offset.SetX(c1); else if (ax1 == 1) offset.SetY(c1); else offset.SetZ(c1);
        if (ax2 == 0) offset.SetX(c2); else if (ax2 == 1) offset.SetY(c2); else offset.SetZ(c2);

        DrawLine(pointA + offset, pointB + offset, r, g, b, a);
    }

    // Draw circles at both ends
    DrawSphere(pointA, radius, r, g, b, a, segments);
    DrawSphere(pointB, radius, r, g, b, a, segments);
}

void DebugLineRenderer::Flush(Camera* camera) {
    if (!s_initialized || !s_enabled || s_vertices.empty()) {
        s_vertices.clear();
        return;
    }

    if (!camera) {
        s_vertices.clear();
        return;
    }

    uint32_t vertexCount = static_cast<uint32_t>(s_vertices.size());
    if (vertexCount > MAX_VERTICES) vertexCount = MAX_VERTICES;

    // Update constant buffer with ViewProjection
    DebugLineCBData cbData;
    cbData.ViewProjection = Camera::GetMainViewMatrix() * Camera::GetMainProjectionMatrix();
    s_constantBuffer->Update(&cbData, sizeof(DebugLineCBData));

    // Update vertex buffer
    s_vertexBuffer->Update(s_vertices.data(), vertexCount * sizeof(Vertex));

    // Capture for lambda
    auto shader = s_shader;
    auto vb = s_vertexBuffer;
    auto cb = s_constantBuffer;
    uint32_t count = vertexCount;

    RenderEngine::RenderCommandQueue::GetInstance()->SubmitCustomCommand(
        [shader, vb, cb, count](RenderEngine::RenderContext* ctx) {
            ctx->BeginDebugLinePass();

            shader->bind();

            ctx->BindConstantBuffer(cb, 0);
            ctx->BindVertexBuffer(vb, 0);
            ctx->Draw(count);

            ctx->EndDebugLinePass();
        });

    s_vertices.clear();
}

} // namespace Sleak
