#include "../../include/private/Graphics/OpenGL/OpenGLBuffer.hpp"
#include <Logger.hpp>
#include <cstring>

namespace Sleak {
namespace RenderEngine {

OpenGLBuffer::OpenGLBuffer(uint32_t size, BufferType type) {
    Size = size;
    Type = type;

    switch (type) {
        case BufferType::Vertex:
            m_target = GL_ARRAY_BUFFER;
            break;
        case BufferType::Index:
            m_target = GL_ELEMENT_ARRAY_BUFFER;
            break;
        case BufferType::Constant:
            m_target = GL_UNIFORM_BUFFER;
            break;
        default:
            m_target = GL_ARRAY_BUFFER;
            break;
    }
}

OpenGLBuffer::~OpenGLBuffer() {
    Cleanup();
}

bool OpenGLBuffer::Initialize(void* data) {
    if (Size == 0) return false;

    glGenBuffers(1, &m_buffer);
    glBindBuffer(m_target, m_buffer);

    GLenum usage = (Type == BufferType::Constant) ? GL_DYNAMIC_DRAW
                                                   : GL_STATIC_DRAW;
    glBufferData(m_target, Size, data, usage);
    glBindBuffer(m_target, 0);

    bIsInitialized = true;
    return true;
}

void OpenGLBuffer::Update() {
    // For constant buffers, bind the UBO to its assigned slot
    if (Type == BufferType::Constant && m_buffer != 0) {
        glBindBufferBase(GL_UNIFORM_BUFFER, Slot, m_buffer);
    }
}

void OpenGLBuffer::Update(void* data, size_t size) {
    if (!data || size == 0 || m_buffer == 0) return;

    glBindBuffer(m_target, m_buffer);
    glBufferSubData(m_target, 0, size, data);
    glBindBuffer(m_target, 0);
}

void OpenGLBuffer::Cleanup() {
    if (m_buffer != 0) {
        glDeleteBuffers(1, &m_buffer);
        m_buffer = 0;
    }
    bIsInitialized = false;
}

bool OpenGLBuffer::Map() {
    if (bIsMapped || m_buffer == 0) return bIsMapped;

    glBindBuffer(m_target, m_buffer);
    Data = glMapBuffer(m_target, GL_WRITE_ONLY);
    if (Data) {
        bIsMapped = true;
        return true;
    }
    glBindBuffer(m_target, 0);
    return false;
}

void OpenGLBuffer::Unmap() {
    if (!bIsMapped) return;

    glBindBuffer(m_target, m_buffer);
    glUnmapBuffer(m_target);
    glBindBuffer(m_target, 0);
    Data = nullptr;
    bIsMapped = false;
}

void* OpenGLBuffer::GetData() {
    return Data;
}

}  // namespace RenderEngine
}  // namespace Sleak
