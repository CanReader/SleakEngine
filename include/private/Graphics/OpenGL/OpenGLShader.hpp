#ifndef OPENGLSHADER_HPP_
#define OPENGLSHADER_HPP_

#include "../Common/Shader.hpp"
#include <glad/glad.h>

namespace Sleak {
namespace RenderEngine {

/// GLSL shader program compiled from either a combined source file or separate vertex/fragment files.
class ENGINE_API OpenGLShader : public Shader {
public:
    OpenGLShader();
    ~OpenGLShader() override;

    /// Splits a combined shader file into vertex/fragment stages and links them.
    bool compile(const std::string& shaderPath) override;
    /// Compiles and links separate vertex and fragment shader files.
    bool compile(const std::string& vert, const std::string& frag) override;
    void bind() override;

    GLuint GetProgram() const { return m_program; }

private:
    /// Compiles a single shader stage from source text.
    bool CompileShaderSource(const std::string& source, GLenum type,
                             GLuint& shader);
    std::string ReadFile(const std::string& path);

    GLuint m_program = 0;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLSHADER_HPP_
