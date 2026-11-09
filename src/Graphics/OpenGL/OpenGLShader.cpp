#include "../../include/private/Graphics/OpenGL/OpenGLShader.hpp"

#include <Core/Logger.hpp>
#include <filesystem>
#include <string_view>

#include "../../include/private/Graphics/Common/ShaderIncludes.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

constexpr std::string_view GL_STEM_SUFFIX = "_gl";
// Runtime copy of every shader root's include folder
constexpr const char* GL_INCLUDE_ROOT = "assets/shaders/include";

/// Returns the file name up to its first '.', minus a trailing _gl.
std::string StemFromPath(const std::string& path) {
    std::string stem = std::filesystem::path(path).filename().string();
    stem = stem.substr(0, stem.find('.'));
    if (stem.ends_with(GL_STEM_SUFFIX)) {
        stem.resize(stem.size() - GL_STEM_SUFFIX.size());
    }
    return stem;
}

/// Returns the info log of a shader or program object.
std::string ReadInfoLog(GLuint object, PFNGLGETSHADERIVPROC getParam,
                        PFNGLGETSHADERINFOLOGPROC getLog) {
    GLint length = 0;
    getParam(object, GL_INFO_LOG_LENGTH, &length);
    if (length <= 0) return {};
    std::string log(static_cast<size_t>(length), '\0');
    GLsizei written = 0;
    getLog(object, length, &written, log.data());
    log.resize(static_cast<size_t>(written));
    while (!log.empty() && (log.back() == '\n' || log.back() == '\r')) {
        log.pop_back();
    }
    return log;
}

// Program made current through OpenGLShader
GLuint g_boundProgram = 0;

#ifdef SLEAK_HOT_RELOAD
/// Returns the live shader registry, leaked to survive static teardown.
std::unordered_set<OpenGLShader*>& LiveShaders() {
    static auto* shaders = new std::unordered_set<OpenGLShader*>();
    return *shaders;
}
#endif

}  // namespace

OpenGLShader::OpenGLShader() {
#ifdef SLEAK_HOT_RELOAD
    LiveShaders().insert(this);
#endif
}

OpenGLShader::~OpenGLShader() {
#ifdef SLEAK_HOT_RELOAD
    LiveShaders().erase(this);
#endif
    if (m_program != 0) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
}

#ifdef SLEAK_HOT_RELOAD
const std::unordered_set<OpenGLShader*>& OpenGLShader::GetLiveShaders() {
    return LiveShaders();
}
#endif

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
    m_vertPath = vertPath;
    m_fragPath = fragPath;
    m_vertStem = StemFromPath(vertPath);
    m_fragStem = StemFromPath(fragPath);
    return BuildProgram();
}

bool OpenGLShader::Reload() {
    if (m_vertPath.empty() || m_fragPath.empty()) return false;
    return BuildProgram();
}

bool OpenGLShader::BuildProgram() {
    std::vector<std::string> vertFiles;
    std::vector<std::string> fragFiles;
    const std::string vertSource = ReadFile(m_vertPath, vertFiles);
    const std::string fragSource = ReadFile(m_fragPath, fragFiles);

    if (vertSource.empty() || fragSource.empty()) {
        SLEAK_ERROR("Failed to read shader files: {} and {}", m_vertPath,
                    m_fragPath);
        return false;
    }

    GLuint vertShader = 0;
    GLuint fragShader = 0;
    if (!CompileShaderSource(vertSource, GL_VERTEX_SHADER, vertFiles,
                             vertShader)) {
        return false;
    }
    if (!CompileShaderSource(fragSource, GL_FRAGMENT_SHADER, fragFiles,
                             fragShader)) {
        glDeleteShader(vertShader);
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);
    glLinkProgram(program);
    glDetachShader(program, vertShader);
    glDetachShader(program, fragShader);
    glDeleteShader(vertShader);
    glDeleteShader(fragShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        SLEAK_ERROR("Shader program linking failed ({} + {}): {}", m_vertPath,
                    m_fragPath,
                    ReadInfoLog(program, glGetProgramiv, glGetProgramInfoLog));
        glDeleteProgram(program);
        return false;
    }

    if (m_program != 0) glDeleteProgram(m_program);
    m_program = program;
    return true;
}

void OpenGLShader::bind() {
    if (m_program != 0) {
        glUseProgram(m_program);
        g_boundProgram = m_program;
    }
}

/// Program the last bind() or BindProgram() made current.
GLuint OpenGLShader::BoundProgram() { return g_boundProgram; }

/// Makes a program current and records it.
void OpenGLShader::BindProgram(GLuint program) {
    glUseProgram(program);
    g_boundProgram = program;
}

bool OpenGLShader::CompileShaderSource(const std::string& source, GLenum type,
                                       const std::vector<std::string>& files,
                                       GLuint& shader) {
    shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        SLEAK_ERROR(
            "Shader compilation failed ({}): {}", files.front(),
            MapShaderLog(ReadInfoLog(shader, glGetShaderiv, glGetShaderInfoLog),
                         files));
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

std::string OpenGLShader::ReadFile(const std::string& path,
                                   std::vector<std::string>& files) {
    ExpandedShader expanded =
        ExpandShaderIncludes(path, {std::filesystem::path(GL_INCLUDE_ROOT)});
    if (!expanded.error.empty()) {
        SLEAK_ERROR("Cannot read shader {}: {}", path, expanded.error);
        return "";
    }
    files = std::move(expanded.files);
    return std::move(expanded.source);
}

}  // namespace RenderEngine
}  // namespace Sleak
