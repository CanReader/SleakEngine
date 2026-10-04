#include <Camera/RenderView.hpp>

namespace Sleak {

namespace {
RenderView s_currentView;
}

RenderView RenderView::Create(const Math::Matrix4& view,
                              const Math::Matrix4& projection,
                              const Math::Vector3D& position) {
    RenderView rv;
    rv.view = view;
    rv.projection = projection;
    rv.viewProjection = view * projection;
    rv.position = position;
    rv.frustum.ExtractFromVP(rv.viewProjection);
    return rv;
}

const RenderView& RenderView::GetCurrent() { return s_currentView; }

void RenderView::SetCurrent(const RenderView& view) { s_currentView = view; }

}  // namespace Sleak
