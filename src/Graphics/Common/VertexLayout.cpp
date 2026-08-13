#include <Runtime/VertexLayout.hpp>
#include <deque>
#include <mutex>

namespace Sleak {

namespace {
    // deque: stable addresses across push_back
    std::deque<VertexLayoutDesc> s_layouts;
    std::mutex                   s_mutex;
}

VertexFormatHandle VertexFormatRegistry::Register(const VertexLayoutDesc& desc) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_layouts.push_back(desc);
    return static_cast<VertexFormatHandle>(s_layouts.size());
}

const VertexLayoutDesc* VertexFormatRegistry::Get(VertexFormatHandle handle) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (handle == 0 || handle > s_layouts.size()) {
        return nullptr;
    }
    return &s_layouts[handle - 1];
}

void VertexFormatRegistry::Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_layouts.clear();
}

} // namespace Sleak
