#ifndef OPENGLIBL_HPP_
#define OPENGLIBL_HPP_

#include <Core/OSDef.hpp>
#include <glad/glad.h>

namespace Sleak {
namespace RenderEngine {

class OpenGLShader;

// Image-Based Lighting resources for the OpenGL deferred lighting pass.
//
// Precomputes three textures from a source environment cubemap:
//   * irradiance cubemap (diffuse ambient per-direction)
//   * prefiltered env cubemap (specular ambient per-roughness)
//   * BRDF LUT (split-sum term)
//
// Precompute runs ONCE (Initialize). Bind() attaches the 3 textures to
// the texture units expected by lighting_pass_gl.frag.
class OpenGLIBL {
public:
    static constexpr uint32_t IRRADIANCE_SIZE = 32;
    static constexpr uint32_t PREFILTER_SIZE  = 128;
    static constexpr uint32_t PREFILTER_MIPS  = 5;
    static constexpr uint32_t BRDF_LUT_SIZE   = 512;

    // Texture unit slots matched by lighting_pass_gl.frag:
    static constexpr uint32_t UNIT_IRRADIANCE = 14;
    static constexpr uint32_t UNIT_PREFILTER  = 15;
    static constexpr uint32_t UNIT_BRDF_LUT   = 16;

    OpenGLIBL() = default;
    ~OpenGLIBL();

    /// Bakes the irradiance map, prefiltered specular map, and BRDF LUT from a source cubemap.
    bool Initialize(GLuint sourceCubemap);
    void Cleanup();

    bool IsInitialized() const { return m_initialized; }

    // Bind the 3 IBL textures at the conventional units for the deferred
    // lighting pass. Caller is responsible for binding the IBL UBO.
    void Bind() const;

    GLuint IrradianceCubemap() const { return m_irradianceCubemap; }
    GLuint PrefilterCubemap()  const { return m_prefilterCubemap; }
    GLuint BRDFLUT()           const { return m_brdfLUT; }

private:
    /// Allocates a mipmapped cubemap render target of the given face size.
    bool CreateCubemapTarget(GLuint& tex, uint32_t size, uint32_t mipLevels);
    /// Convolves the source cubemap into a diffuse irradiance map.
    bool BakeIrradiance(GLuint sourceCubemap);
    /// Prefilters the source cubemap per roughness mip for specular IBL.
    bool BakePrefilter(GLuint sourceCubemap);
    /// Renders the split-sum BRDF integration LUT.
    bool BakeBRDFLUT();

    GLuint m_irradianceCubemap = 0;
    GLuint m_prefilterCubemap  = 0;
    GLuint m_brdfLUT           = 0;

    GLuint m_fbo     = 0;
    GLuint m_rbo     = 0;
    GLuint m_cubeVAO = 0;
    GLuint m_cubeVBO = 0;
    GLuint m_quadVAO = 0;

    GLuint m_faceUBO      = 0; // mat4 ViewProjection (shared by irradiance / prefilter)
    GLuint m_prefilterUBO = 0; // mat4 ViewProjection + float Roughness

    OpenGLShader* m_irradianceShader = nullptr;
    OpenGLShader* m_prefilterShader  = nullptr;
    OpenGLShader* m_brdfShader       = nullptr;

    bool m_initialized = false;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLIBL_HPP_
