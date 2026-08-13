#ifndef OPENGLBUFFER_HPP_
#define OPENGLBUFFER_HPP_

#include "../Common/BufferBase.hpp"
#include <glad/glad.h>

namespace Sleak {
namespace RenderEngine {

/// OpenGL VBO/EBO/UBO wrapper; target is derived from BufferType at construction.
class ENGINE_API OpenGLBuffer : public BufferBase {
public:
    OpenGLBuffer(uint32_t size, BufferType type);
    ~OpenGLBuffer() override;

    /// Creates the GL buffer object and uploads the initial payload, if any.
    bool Initialize(void* data) override;
    /// Re-uploads the last-mapped/updated data via glBufferSubData.
    void Update() override;
    /// Uploads new data, resizing the backing store if it grew.
    void Update(void* data, size_t size) override;
    void Cleanup() override;

    /// Maps the buffer for direct CPU writes via glMapBuffer.
    bool Map() override;
    void Unmap() override;

    void* GetData() override;

    GLuint GetGLBuffer() const { return m_buffer; }
    GLenum GetTarget() const { return m_target; }

private:
    GLuint m_buffer = 0;
    GLenum m_target = 0;
    void* m_mappedData = nullptr;
};

}  // namespace RenderEngine
}  // namespace Sleak

#endif  // OPENGLBUFFER_HPP_
