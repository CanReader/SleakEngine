#ifndef OPENGLBUFFER_HPP_
#define OPENGLBUFFER_HPP_

#include "../BufferBase.hpp"
#include <glad/glad.h>

namespace Sleak {
namespace RenderEngine {

class ENGINE_API OpenGLBuffer : public BufferBase {
public:
    OpenGLBuffer(uint32_t size, BufferType type);
    ~OpenGLBuffer() override;

    bool Initialize(void* data) override;
    void Update() override;
    void Update(void* data, size_t size) override;
    void Cleanup() override;

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
