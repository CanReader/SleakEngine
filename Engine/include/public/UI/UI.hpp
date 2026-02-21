#pragma once

#include <Core/OSDef.hpp>

namespace Sleak::UI {

enum PanelFlags : int {
    PanelFlags_None            = 0,
    PanelFlags_NoTitleBar      = 1 << 0,
    PanelFlags_AutoResize      = 1 << 1,
    PanelFlags_NoMove          = 1 << 2,
    PanelFlags_NoInput         = 1 << 3,
    PanelFlags_NoFocusOnAppear = 1 << 4,
};

ENGINE_API void BeginPanel(const char* name, float x, float y,
                           float bgAlpha = 0.4f,
                           int flags = PanelFlags_NoTitleBar |
                                       PanelFlags_AutoResize |
                                       PanelFlags_NoMove |
                                       PanelFlags_NoInput |
                                       PanelFlags_NoFocusOnAppear);
ENGINE_API void EndPanel();
ENGINE_API void Text(const char* fmt, ...);

}  // namespace Sleak::UI
