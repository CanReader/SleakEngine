#ifndef OPENGLSHADER_HPP_
#define OPENGLSHADER_HPP_

#include "../Shader.hpp"
#include <glad/glad.h>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API OpenGLShader : public Shader {
public:
    OpenGLShader();
    ~OpenGLShader() override;

    bool compile(const std::string& shaderPath) override;
    bool compile(const std::string& vert, const std::string& frag) override;
    void bind() override;

    GLuint GetProgram() const { return m_program; }

private:
    bool CompileShaderSource(const std::string& source, GLenum type,
                             GLuint& shader);
    std::string ReadFile(const std::string& path);

    GLuint m_program = 0;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLSHADER_HPP_
