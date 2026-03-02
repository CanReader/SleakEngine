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
ENGINE_API void TextColored(float r, float g, float b, float a, const char* fmt, ...);
ENGINE_API void TextDisabled(const char* fmt, ...);

ENGINE_API bool Checkbox(const char* label, bool* value);
ENGINE_API bool Button(const char* label);
ENGINE_API bool DragFloat(const char* label, float* value, float speed = 1.0f, float min = 0.0f, float max = 0.0f);
ENGINE_API bool Combo(const char* label, int* current, const char* const items[], int count);

ENGINE_API void Separator();
ENGINE_API void SameLine();
ENGINE_API void BeginGroup();
ENGINE_API void EndGroup();
ENGINE_API void BeginChild(const char* name);
ENGINE_API void EndChild();

ENGINE_API float GetViewportWidth();
ENGINE_API float GetViewportHeight();

ENGINE_API void DrawLine(float x1, float y1, float x2, float y2,
                         float r, float g, float b, float a = 1.0f, float thickness = 1.0f);

}  // namespace Sleak::UI
