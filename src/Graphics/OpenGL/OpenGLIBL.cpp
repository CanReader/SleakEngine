#include "../../include/private/Graphics/OpenGL/OpenGLIBL.hpp"
#include "../../include/private/Graphics/OpenGL/OpenGLShader.hpp"
#include <Logger.hpp>
#include <Math/Matrix.hpp>
#include <Math/Vector.hpp>
#include <cstring>

namespace Sleak {
namespace RenderEngine {

namespace {

// Unit cube vertices — each face as two triangles, position only.
// 6 faces * 2 triangles * 3 verts = 36 vertices, 3 floats each.
constexpr float kCubeVertices[] = {
    // -Z
    -1, -1, -1,   1,  1, -1,   1, -1, -1,
    -1, -1, -1,  -1,  1, -1,   1,  1, -1,
    // +Z
    -1, -1,  1,   1, -1,  1,   1,  1,  1,
    -1, -1,  1,   1,  1,  1,  -1,  1,  1,
    // -X
    -1, -1, -1,  -1, -1,  1,  -1,  1,  1,
    -1, -1, -1,  -1,  1,  1,  -1,  1, -1,
    // +X
     1, -1, -1,   1,  1,  1,   1, -1,  1,
     1, -1, -1,   1,  1, -1,   1,  1,  1,
    // -Y
    -1, -1, -1,   1, -1, -1,   1, -1,  1,
    -1, -1, -1,   1, -1,  1,  -1, -1,  1,
    // +Y
    -1,  1, -1,  -1,  1,  1,   1,  1,  1,
    -1,  1, -1,   1,  1,  1,   1,  1, -1,
};

// 6 capture views (LH, Y-up). The "up" vectors are chosen so that the
// resulting cubemap face orientation matches GL_TEXTURE_CUBE_MAP_*
// when the captured cube is sampled with a unit direction vector.
Math::Matrix4 BuildCaptureView(int face) {
    using V3 = Math::Vector<float, 3>;
    const V3 origin{0.0f, 0.0f, 0.0f};
    V3 target, up;
    switch (face) {
        case 0: target = V3{ 1, 0, 0}; up = V3{0, -1,  0}; break; // +X
        case 1: target = V3{-1, 0, 0}; up = V3{0, -1,  0}; break; // -X
        case 2: target = V3{ 0, 1, 0}; up = V3{0,  0,  1}; break; // +Y
        case 3: target = V3{ 0,-1, 0}; up = V3{0,  0, -1}; break; // -Y
        case 4: target = V3{ 0, 0, 1}; up = V3{0, -1,  0}; break; // +Z
        case 5: target = V3{ 0, 0,-1}; up = V3{0, -1,  0}; break; // -Z
        default: target = V3{ 1, 0, 0}; up = V3{0, -1,  0}; break;
    }
    return Math::Matrix4::LookAt(origin, target, up);
}

}  // namespace

OpenGLIBL::~OpenGLIBL() { Cleanup(); }

void OpenGLIBL::Cleanup() {
    if (m_irradianceCubemap) { glDeleteTextures(1, &m_irradianceCubemap); m_irradianceCubemap = 0; }
    if (m_prefilterCubemap)  { glDeleteTextures(1, &m_prefilterCubemap);  m_prefilterCubemap  = 0; }
    if (m_brdfLUT)           { glDeleteTextures(1, &m_brdfLUT);           m_brdfLUT           = 0; }
    if (m_fbo)     { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_rbo)     { glDeleteRenderbuffers(1, &m_rbo); m_rbo = 0; }
    if (m_cubeVBO) { glDeleteBuffers(1, &m_cubeVBO); m_cubeVBO = 0; }
    if (m_cubeVAO) { glDeleteVertexArrays(1, &m_cubeVAO); m_cubeVAO = 0; }
    if (m_quadVAO) { glDeleteVertexArrays(1, &m_quadVAO); m_quadVAO = 0; }
    if (m_faceUBO)      { glDeleteBuffers(1, &m_faceUBO); m_faceUBO = 0; }
    if (m_prefilterUBO) { glDeleteBuffers(1, &m_prefilterUBO); m_prefilterUBO = 0; }
    delete m_irradianceShader; m_irradianceShader = nullptr;
    delete m_prefilterShader;  m_prefilterShader  = nullptr;
    delete m_brdfShader;       m_brdfShader       = nullptr;
    m_initialized = false;
}

bool OpenGLIBL::CreateCubemapTarget(GLuint& tex, uint32_t size, uint32_t mipLevels) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    for (int face = 0; face < 6; ++face) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F,
                     size, size, 0, GL_RGB, GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S,    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T,    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R,    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    mipLevels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    if (mipLevels > 1) glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return true;
}

bool OpenGLIBL::Initialize(GLuint sourceCubemap) {
    if (m_initialized) return true;
    if (sourceCubemap == 0) {
        SLEAK_WARN("OpenGLIBL: source cubemap is 0 — IBL precompute skipped");
        return false;
    }

    // ---- Cube VAO ----
    glGenVertexArrays(1, &m_cubeVAO);
    glGenBuffers(1, &m_cubeVBO);
    glBindVertexArray(m_cubeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVertices), kCubeVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    // ---- Empty quad VAO for fullscreen BRDF pass ----
    glGenVertexArrays(1, &m_quadVAO);

    // ---- Shared FBO + depth RBO (resized per pass) ----
    glGenFramebuffers(1, &m_fbo);
    glGenRenderbuffers(1, &m_rbo);

    // ---- UBOs ----
    glGenBuffers(1, &m_faceUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_faceUBO);
    glBufferData(GL_UNIFORM_BUFFER, 64, nullptr, GL_DYNAMIC_DRAW); // mat4
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glGenBuffers(1, &m_prefilterUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_prefilterUBO);
    glBufferData(GL_UNIFORM_BUFFER, 80, nullptr, GL_DYNAMIC_DRAW); // mat4 + 4 floats
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // ---- Shaders ----
    m_irradianceShader = new OpenGLShader();
    if (!m_irradianceShader->compile("assets/shaders/ibl_irradiance_gl.vert",
                                     "assets/shaders/ibl_irradiance_gl.frag")) {
        SLEAK_ERROR("OpenGLIBL: failed to compile irradiance shader");
        Cleanup();
        return false;
    }

    m_prefilterShader = new OpenGLShader();
    if (!m_prefilterShader->compile("assets/shaders/ibl_prefilter_gl.vert",
                                    "assets/shaders/ibl_prefilter_gl.frag")) {
        SLEAK_ERROR("OpenGLIBL: failed to compile prefilter shader");
        Cleanup();
        return false;
    }

    m_brdfShader = new OpenGLShader();
    if (!m_brdfShader->compile("assets/shaders/ibl_brdf_lut_gl.vert",
                               "assets/shaders/ibl_brdf_lut_gl.frag")) {
        SLEAK_ERROR("OpenGLIBL: failed to compile BRDF LUT shader");
        Cleanup();
        return false;
    }

    // ---- Targets ----
    CreateCubemapTarget(m_irradianceCubemap, IRRADIANCE_SIZE, 1);
    CreateCubemapTarget(m_prefilterCubemap,  PREFILTER_SIZE,  PREFILTER_MIPS);

    glGenTextures(1, &m_brdfLUT);
    glBindTexture(GL_TEXTURE_2D, m_brdfLUT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, BRDF_LUT_SIZE, BRDF_LUT_SIZE,
                 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // ---- Save GL state we're about to clobber ----
    GLint prevFBO = 0, prevViewport[4] = {0,0,0,0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    bool ok = BakeIrradiance(sourceCubemap)
           && BakePrefilter(sourceCubemap)
           && BakeBRDFLUT();

    // ---- Restore ----
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    if (!ok) {
        SLEAK_ERROR("OpenGLIBL: precompute failed");
        Cleanup();
        return false;
    }

    m_initialized = true;
    SLEAK_INFO("OpenGLIBL: precompute complete (irradiance {}², prefilter {}² × {} mips, BRDF LUT {}²)",
               IRRADIANCE_SIZE, PREFILTER_SIZE, PREFILTER_MIPS, BRDF_LUT_SIZE);
    return true;
}

bool OpenGLIBL::BakeIrradiance(GLuint sourceCubemap) {
    Math::Matrix4 proj = Math::Matrix4::Perspective(
        1.5707963f /* 90° */, 1.0f, 0.1f, 10.0f);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                          IRRADIANCE_SIZE, IRRADIANCE_SIZE);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_rbo);
    glViewport(0, 0, IRRADIANCE_SIZE, IRRADIANCE_SIZE);

    m_irradianceShader->bind();

    // Bind source cubemap via the conventional sampler name. The shader
    // uses default texture-unit-0 sampler (no explicit binding qualifier),
    // so we bind to unit 0 and set the sampler uniform once.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, sourceCubemap);
    GLint envLoc = glGetUniformLocation(m_irradianceShader->GetProgram(), "environmentMap");
    if (envLoc >= 0) glUniform1i(envLoc, 0);

    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_faceUBO);

    glBindVertexArray(m_cubeVAO);
    for (int face = 0; face < 6; ++face) {
        Math::Matrix4 view = BuildCaptureView(face);
        Math::Matrix4 vp   = proj * view;
        glBindBuffer(GL_UNIFORM_BUFFER, m_faceUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, 64, &vp(0,0));

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                               m_irradianceCubemap, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            SLEAK_ERROR("OpenGLIBL: irradiance FBO incomplete on face {}", face);
            return false;
        }
        glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);
    return true;
}

bool OpenGLIBL::BakePrefilter(GLuint sourceCubemap) {
    Math::Matrix4 proj = Math::Matrix4::Perspective(
        1.5707963f /* 90° */, 1.0f, 0.1f, 10.0f);

    m_prefilterShader->bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, sourceCubemap);
    GLint envLoc = glGetUniformLocation(m_prefilterShader->GetProgram(), "environmentMap");
    if (envLoc >= 0) glUniform1i(envLoc, 0);

    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_prefilterUBO);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glBindVertexArray(m_cubeVAO);

    struct PrefilterUBOData {
        float vp[16];
        float roughness;
        float pad[3];
    };

    for (uint32_t mip = 0; mip < PREFILTER_MIPS; ++mip) {
        uint32_t mipSize = PREFILTER_SIZE >> mip;
        glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                              mipSize, mipSize);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, m_rbo);
        glViewport(0, 0, mipSize, mipSize);

        float roughness = static_cast<float>(mip) /
                          static_cast<float>(PREFILTER_MIPS - 1);

        for (int face = 0; face < 6; ++face) {
            Math::Matrix4 view = BuildCaptureView(face);
            Math::Matrix4 vp   = proj * view;

            PrefilterUBOData ubo{};
            std::memcpy(ubo.vp, &vp(0,0), 64);
            ubo.roughness = roughness;
            glBindBuffer(GL_UNIFORM_BUFFER, m_prefilterUBO);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ubo), &ubo);

            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                                   m_prefilterCubemap, mip);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                SLEAK_ERROR("OpenGLIBL: prefilter FBO incomplete (mip {}, face {})",
                            mip, face);
                return false;
            }
            glClearColor(0,0,0,1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
    }
    glBindVertexArray(0);
    return true;
}

bool OpenGLIBL::BakeBRDFLUT() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                          BRDF_LUT_SIZE, BRDF_LUT_SIZE);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_rbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_brdfLUT, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SLEAK_ERROR("OpenGLIBL: BRDF LUT FBO incomplete");
        return false;
    }

    glViewport(0, 0, BRDF_LUT_SIZE, BRDF_LUT_SIZE);
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_brdfShader->bind();
    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    return true;
}

void OpenGLIBL::Bind() const {
    if (!m_initialized) return;
    glActiveTexture(GL_TEXTURE0 + UNIT_IRRADIANCE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_irradianceCubemap);
    glActiveTexture(GL_TEXTURE0 + UNIT_PREFILTER);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_prefilterCubemap);
    glActiveTexture(GL_TEXTURE0 + UNIT_BRDF_LUT);
    glBindTexture(GL_TEXTURE_2D, m_brdfLUT);
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace RenderEngine
}  // namespace Sleak
