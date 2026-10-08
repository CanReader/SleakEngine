#ifndef _RENDER_VIEW_HPP_
#define _RENDER_VIEW_HPP_

#include <Camera/ViewFrustum.hpp>
#include <Core/OSDef.hpp>
#include <Math/Matrix.hpp>
#include <Math/Vector.hpp>

namespace Sleak {

/// Snapshot of one camera for one frame: view, projection, eye position
/// and the frustum culling tests against.
///
/// SceneBase builds one from its active camera after the update pass
/// and hands it to the render submission step, so cameras that are not
/// the active one never touch what gets drawn. It is a plain value, so
/// rendering a second view means building a second RenderView.
///
/// Renderer code that has no scene to ask reads the view the frame is
/// being submitted with through GetCurrent().
///
/// @see Camera, SceneBase, CullingSystem
/// @ingroup camera
struct ENGINE_API RenderView {
    Math::Matrix4 view = Math::Matrix4::Identity();
    Math::Matrix4 projection = Math::Matrix4::Identity();
    Math::Matrix4 viewProjection = Math::Matrix4::Identity();
    Math::Vector3D position = Math::Vector3D(0, 0, 0);
    ViewFrustum frustum{};

    /// Builds a view from row-vector matrices, deriving viewProjection
    /// and the frustum.
    static RenderView Create(const Math::Matrix4& view,
                             const Math::Matrix4& projection,
                             const Math::Vector3D& position);

    /// The view the current frame is being submitted with.
    static const RenderView& GetCurrent();
    /// Makes view the one GetCurrent() returns until the next call.
    static void SetCurrent(const RenderView& view);
};

}  // namespace Sleak

#endif
