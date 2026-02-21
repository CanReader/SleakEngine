#include <UI/UI.hpp>
#include <imgui.h>
#include <cstdarg>

namespace Sleak::UI {

void BeginPanel(const char* name, float x, float y, float bgAlpha, int flags) {
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(bgAlpha);

    ImGuiWindowFlags imFlags = 0;
    if (flags & PanelFlags_NoTitleBar)      imFlags |= ImGuiWindowFlags_NoTitleBar;
    if (flags & PanelFlags_AutoResize)      imFlags |= ImGuiWindowFlags_AlwaysAutoResize;
    if (flags & PanelFlags_NoMove)          imFlags |= ImGuiWindowFlags_NoMove;
    if (flags & PanelFlags_NoInput)         imFlags |= ImGuiWindowFlags_NoInputs;
    if (flags & PanelFlags_NoFocusOnAppear) imFlags |= ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::Begin(name, nullptr, imFlags);
}

void EndPanel() {
    ImGui::End();
}

void Text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

}  // namespace Sleak::UI
