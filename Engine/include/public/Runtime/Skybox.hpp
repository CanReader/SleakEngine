#ifndef _SKYBOX_HPP_
#define _SKYBOX_HPP_

#include <Core/OSDef.hpp>
#include <string>
#include <array>
#include <Memory/RefPtr.h>

namespace Sleak {

    namespace RenderEngine {
        class BufferBase;
        class Shader;
    }
    class Texture;

    class ENGINE_API Skybox {
    public:
        /// Construct skybox from 6 cubemap face image paths
        /// Order: +X, -X, +Y, -Y, +Z, -Z
        explicit Skybox(const std::array<std::string, 6>& facePaths);

        /// Construct skybox from a single equirectangular panorama image
        explicit Skybox(const std::string& panoramaPath);

        /// Construct skybox with procedural gradient (top, mid, bottom colors)
        Skybox(float topR, float topG, float topB,
               float midR, float midG, float midB,
               float botR, float botG, float botB);

        /// Default skybox: loads built-in sky panorama
        Skybox();

        ~Skybox();

        /// Create GPU resources (shader, buffers, cubemap texture)
        void Initialize();

        /// Submit render commands for this frame
        void Render();

        bool IsInitialized() const { return m_initialized; }

    private:
        enum class SkyboxMode { Cubemap, Panorama, Gradient, Default };
        SkyboxMode m_mode = SkyboxMode::Default;

        // Cubemap face paths (for Cubemap mode)
        std::array<std::string, 6> m_facePaths;

        // Panorama path (for Panorama/Default mode)
        std::string m_panoramaPath;

        // Gradient colors (for Gradient mode)
        float m_topR = 0.1f, m_topG = 0.3f, m_topB = 0.8f;
        float m_midR = 0.5f, m_midG = 0.7f, m_midB = 1.0f;
        float m_botR = 0.8f, m_botG = 0.85f, m_botB = 0.9f;

        // GPU resources
        RefPtr<RenderEngine::Shader> m_shader;
        RefPtr<RenderEngine::BufferBase> m_vertexBuffer;
        RefPtr<RenderEngine::BufferBase> m_indexBuffer;
        RefPtr<RenderEngine::BufferBase> m_constantBuffer;
        RefPtr<Texture> m_cubemapTexture;

        bool m_initialized = false;
    };

} // namespace Sleak

#endif
