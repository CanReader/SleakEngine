#ifndef OPENGLSHADER_HPP_
#define OPENGLSHADER_HPP_

#include <glad/glad.h>

#include <string>
#include <vector>

#include "../Common/Shader.hpp"
#ifdef SLEAK_HOT_RELOAD
#include <unordered_set>
#endif

namespace Sleak {
namespace RenderEngine {

/// GLSL shader program compiled from either a combined source file or separate vertex/fragment files.
class ENGINE_API OpenGLShader : public Shader {
public:
    OpenGLShader();
    ~OpenGLShader() override;
    OpenGLShader(const OpenGLShader&) = delete;
    OpenGLShader& operator=(const OpenGLShader&) = delete;

    /// Splits a combined shader file into vertex/fragment stages and links them.
    bool compile(const std::string& shaderPath) override;
    /// Compiles and links separate vertex and fragment shader files.
    bool compile(const std::string& vert, const std::string& frag) override;
    void bind() override;

    GLuint GetProgram() const { return m_program; }
    /// Program the last bind() or BindProgram() made current.
    static GLuint BoundProgram();
    /// Makes a program current and records it; 0 unbinds.
    static void BindProgram(GLuint program);

    /// Rebuilds the program from its files, keeping the old one on failure.
    bool Reload();
    /// Returns the stem of the vertex source, e.g. gbuffer for gbuffer_gl.vert.
    const std::string& GetStem() const { return m_vertStem; }
    /// Returns true when the vertex or fragment source has the given stem.
    bool UsesStem(const std::string& stem) const {
        return stem == m_vertStem || stem == m_fragStem;
    }

#ifdef SLEAK_HOT_RELOAD
    /// Returns every live OpenGLShader instance.
    static const std::unordered_set<OpenGLShader*>& GetLiveShaders();
#endif

   private:
    /// Compiles one stage; files names the #line source strings in errors.
    bool CompileShaderSource(const std::string& source, GLenum type,
                             const std::vector<std::string>& files,
                             GLuint& shader);
    /// Links a new program from the stored paths and swaps it in on success.
    bool BuildProgram();
    /// Reads a stage with its #include lines inlined; empty on failure.
    std::string ReadFile(const std::string& path,
                         std::vector<std::string>& files);

    GLuint m_program = 0;
    std::string m_vertPath;
    std::string m_fragPath;
    std::string m_vertStem;
    std::string m_fragStem;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLSHADER_HPP_
