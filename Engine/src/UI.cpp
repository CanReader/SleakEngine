#include <UI/UI.hpp>
#include <imgui.h>
#include <cstdarg>

namespace Sleak::UI {

void BeginPanel(const char* name, float x, float y, float bgAlpha, int flags) {
    ImGuiCond posCond = (flags & PanelFlags_NoMove) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), posCond);
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

void TextColored(float r, float g, float b, float a, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::TextColoredV(ImVec4(r, g, b, a), fmt, args);
    va_end(args);
}

void TextDisabled(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::TextDisabledV(fmt, args);
    va_end(args);
}

bool Checkbox(const char* label, bool* value) {
    return ImGui::Checkbox(label, value);
}

bool Button(const char* label) {
    return ImGui::Button(label);
}

bool DragFloat(const char* label, float* value, float speed, float min, float max) {
    return ImGui::DragFloat(label, value, speed, min, max);
}

bool Combo(const char* label, int* current, const char* const items[], int count) {
    return ImGui::Combo(label, current, items, count);
}

void Separator() {
    ImGui::Separator();
}

void SameLine() {
    ImGui::SameLine();
}

void BeginGroup() {
    ImGui::BeginGroup();
}

void EndGroup() {
    ImGui::EndGroup();
}

void BeginChild(const char* name) {
    ImGui::BeginChild(name, {0, 0}, ImGuiChildFlags_AutoResizeY);
}

void EndChild() {
    ImGui::EndChild();
}

float GetViewportWidth() {
    return ImGui::GetMainViewport()->Size.x;
}

float GetViewportHeight() {
    return ImGui::GetMainViewport()->Size.y;
}

}  // namespace Sleak::UI
