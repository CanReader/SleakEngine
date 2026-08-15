#pragma once

#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>
#include <Memory/RefPtr.hpp>
#include <Physics/Colliders.hpp>
#include <vector>

namespace Sleak {

struct Vertex;
namespace RenderEngine { class BufferBase; class Shader; }

class Camera;

/// Immediate-mode wireframe line drawing for debug visualization; lines queue up and are flushed once per frame.
/// @ingroup debug
class ENGINE_API DebugLineRenderer {
public:
    /// Allocates the shared vertex buffer, shader, and constant buffer.
    static void Initialize();
    static void Shutdown();

    static void SetEnabled(bool enabled) { s_enabled = enabled; }
    static bool IsEnabled() { return s_enabled; }

    /// Queues a single line segment for the next Flush.
    static void DrawLine(const Math::Vector3D& start, const Math::Vector3D& end,
                         float r, float g, float b, float a = 1.0f);

    /// Queues a wireframe box outline.
    static void DrawAABB(const Physics::AABB& aabb,
                         float r, float g, float b, float a = 1.0f);

    /// Queues a wireframe sphere approximated with segments latitude/longitude rings.
    static void DrawSphere(const Math::Vector3D& center, float radius,
                           float r, float g, float b, float a = 1.0f,
                           int segments = 16);

    /// Queues a wireframe capsule outline.
    static void DrawCapsule(const Physics::BoundingCapsule& capsule,
                            float r, float g, float b, float a = 1.0f,
                            int segments = 16);

    /// Uploads all queued lines and draws them in one pass, then clears the queue.
    static void Flush(Camera* camera);

private:
    static std::vector<Vertex>* s_vertices;
    static RefPtr<RenderEngine::BufferBase> s_vertexBuffer;
    static RefPtr<RenderEngine::Shader> s_shader;
    static RefPtr<RenderEngine::BufferBase> s_constantBuffer;
    static bool s_enabled;
    static bool s_initialized;

    static constexpr uint32_t MAX_VERTICES = 65536;
};

} // namespace Sleak
