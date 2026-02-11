#include "../../include/private/Graphics/OpenGL/OpenGLShader.hpp"
#include <Logger.hpp>
#include <fstream>
#include <sstream>

namespace Sleak {
namespace RenderEngine {

OpenGLShader::OpenGLShader() {}

OpenGLShader::~OpenGLShader() {
    if (m_program != 0) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
}

bool OpenGLShader::compile(const std::string& shaderPath) {
    // Strip .hlsl extension if present, then use _gl.vert/_gl.frag
    std::string basePath = shaderPath;
    auto hlslPos = basePath.rfind(".hlsl");
    if (hlslPos != std::string::npos) {
        basePath = basePath.substr(0, hlslPos);
    }
    return compile(basePath + "_gl.vert", basePath + "_gl.frag");
}

bool OpenGLShader::compile(const std::string& vertPath,
                            const std::string& fragPath) {
    std::string vertSource = ReadFile(vertPath);
    std::string fragSource = ReadFile(fragPath);

    if (vertSource.empty() || fragSource.empty()) {
        SLEAK_ERROR("Failed to read shader files: {} and {}", vertPath,
                     fragPath);
        return false;
    }

    GLuint vertShader = 0, fragShader = 0;

    if (!CompileShaderSource(vertSource, GL_VERTEX_SHADER, vertShader)) {
        return false;
    }
    if (!CompileShaderSource(fragSource, GL_FRAGMENT_SHADER, fragShader)) {
        glDeleteShader(vertShader);
        return false;
    }

    m_program = glCreateProgram();
    glAttachShader(m_program, vertShader);
    glAttachShader(m_program, fragShader);
    glLinkProgram(m_program);

    GLint success = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_program, 512, nullptr, infoLog);
        SLEAK_ERROR("Shader program linking failed: {}", infoLog);
        glDeleteProgram(m_program);
        m_program = 0;
        glDeleteShader(vertShader);
        glDeleteShader(fragShader);
        return false;
    }

    glDeleteShader(vertShader);
    glDeleteShader(fragShader);

    return true;
}

void OpenGLShader::bind() {
    if (m_program != 0) {
        glUseProgram(m_program);
    }
}

bool OpenGLShader::CompileShaderSource(const std::string& source,
                                        GLenum type, GLuint& shader) {
    shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        SLEAK_ERROR("Shader compilation failed: {}", infoLog);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

std::string OpenGLShader::ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        SLEAK_ERROR("Cannot open shader file: {}", path);
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

}  // namespace RenderEngine
}  // namespace Sleak
