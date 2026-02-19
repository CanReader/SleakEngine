#pragma once

#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>
#include <Graphics/Vertex.hpp>
#include <Graphics/BufferBase.hpp>
#include <Graphics/Shader.hpp>
#include <Memory/RefPtr.h>
#include <Physics/Colliders.hpp>
#include <vector>

namespace Sleak {

class Camera;

class ENGINE_API DebugLineRenderer {
public:
    static void Initialize();
    static void Shutdown();

    static void SetEnabled(bool enabled) { s_enabled = enabled; }
    static bool IsEnabled() { return s_enabled; }

    static void DrawLine(const Math::Vector3D& start, const Math::Vector3D& end,
                         float r, float g, float b, float a = 1.0f);

    static void DrawAABB(const Physics::AABB& aabb,
                         float r, float g, float b, float a = 1.0f);

    static void DrawSphere(const Math::Vector3D& center, float radius,
                           float r, float g, float b, float a = 1.0f,
                           int segments = 16);

    static void DrawCapsule(const Physics::BoundingCapsule& capsule,
                            float r, float g, float b, float a = 1.0f,
                            int segments = 16);

    static void Flush(Camera* camera);

private:
    static std::vector<Vertex> s_vertices;
    static RefPtr<RenderEngine::BufferBase> s_vertexBuffer;
    static RefPtr<RenderEngine::Shader> s_shader;
    static RefPtr<RenderEngine::BufferBase> s_constantBuffer;
    static bool s_enabled;
    static bool s_initialized;

    static constexpr uint32_t MAX_VERTICES = 65536;
};

} // namespace Sleak
