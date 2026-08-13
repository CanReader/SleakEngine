#include <UI/UI.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include "../../include/private/Graphics/ResourceManager.hpp"
#include "../../include/private/Graphics/Renderer.hpp"
#include <Core/Application.hpp>
#include <Runtime/Texture.hpp>
#include <stb_image.h>
#include <cstdarg>
#include <unordered_map>

namespace Sleak::UI {

void BeginPanel(const char* name, float x, float y, float bgAlpha, int flags) {
    // Only set position if explicitly provided (non-zero),
    // so SetNextWindowPos() called before BeginPanel is not overridden
    if (x != 0.0f || y != 0.0f) {
        ImGuiCond posCond = (flags & PanelFlags_NoMove) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImGui::SetNextWindowPos(ImVec2(x, y), posCond);
    }
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

void BeginChildSized(const char* name, float width, float height) {
    ImGui::BeginChild(name, {width, height}, ImGuiChildFlags_AutoResizeY);
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

void DrawLine(float x1, float y1, float x2, float y2,
              float r, float g, float b, float a, float thickness) {
    ImGui::GetForegroundDrawList()->AddLine(
        ImVec2(x1, y1), ImVec2(x2, y2), ImColor(r, g, b, a), thickness);
}

void DrawRect(float x, float y, float w, float h,
              float r, float g, float b, float a, float thickness,
              float rounding) {
    ImGui::GetForegroundDrawList()->AddRect(
        ImVec2(x, y), ImVec2(x + w, y + h), ImColor(r, g, b, a), rounding, 0, thickness);
}

void DrawFilledRect(float x, float y, float w, float h,
                    float r, float g, float b, float a,
                    float rounding) {
    ImGui::GetForegroundDrawList()->AddRectFilled(
        ImVec2(x, y), ImVec2(x + w, y + h), ImColor(r, g, b, a), rounding);
}

void DrawText(const char* text, float x, float y,
              float r, float g, float b, float a) {
    ImGui::GetForegroundDrawList()->AddText(
        ImVec2(x, y), ImColor(r, g, b, a), text);
}

bool InputText(const char* label, char* buf, size_t bufSize) {
    return ImGui::InputText(label, buf, bufSize);
}

bool InputTextString(const char* label, std::string* str) {
    return ImGui::InputText(label, str);
}

bool ButtonSized(const char* label, float width, float height) {
    return ImGui::Button(label, ImVec2(width, height));
}

bool Selectable(const char* label, bool selected) {
    return ImGui::Selectable(label, selected);
}

void SetNextItemWidth(float width) {
    ImGui::SetNextItemWidth(width);
}

void Spacing() {
    ImGui::Spacing();
}

void Dummy(float width, float height) {
    ImGui::Dummy(ImVec2(width, height));
}

void PushStyleColor(int idx, float r, float g, float b, float a) {
    ImGui::PushStyleColor(static_cast<ImGuiCol>(idx), ImVec4(r, g, b, a));
}

void PopStyleColor(int count) {
    ImGui::PopStyleColor(count);
}

void PushStyleVar(int idx, float val) {
    ImGui::PushStyleVar(static_cast<ImGuiStyleVar>(idx), val);
}

void PushStyleVarVec(int idx, float x, float y) {
    ImGui::PushStyleVar(static_cast<ImGuiStyleVar>(idx), ImVec2(x, y));
}

void PopStyleVar(int count) {
    ImGui::PopStyleVar(count);
}

bool BeginListBox(const char* label, float width, float height) {
    return ImGui::BeginListBox(label, ImVec2(width, height));
}

void EndListBox() {
    ImGui::EndListBox();
}

void SetCursorPosX(float x) {
    ImGui::SetCursorPosX(x);
}

void SetCursorPosY(float y) {
    ImGui::SetCursorPosY(y);
}

float GetCursorPosY() {
    return ImGui::GetCursorPosY();
}

float GetContentRegionAvailWidth() {
    return ImGui::GetContentRegionAvail().x;
}

void SetNextWindowPos(float x, float y, bool always) {
    ImGui::SetNextWindowPos(ImVec2(x, y), always ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
}

void SetNextWindowSize(float w, float h, bool always) {
    ImGui::SetNextWindowSize(ImVec2(w, h), always ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
}

void ProgressBar(float fraction, float width, float height, const char* overlay) {
    ImGui::ProgressBar(fraction, ImVec2(width, height), overlay);
}

void TextWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::TextWrappedV(fmt, args);
    va_end(args);
}

void Image(uint64_t textureID, float width, float height) {
    // OpenGL textures are vertically flipped relative to ImGui's expectation
    bool flipY = false;
    auto* app = Application::GetInstance();
    if (app && app->GetRenderer() &&
        app->GetRenderer()->GetType() == RenderEngine::RendererType::OpenGL)
        flipY = true;

    ImVec2 uv0 = flipY ? ImVec2(0, 1) : ImVec2(0, 0);
    ImVec2 uv1 = flipY ? ImVec2(1, 0) : ImVec2(1, 1);
    ImGui::Image(static_cast<ImTextureID>(textureID), ImVec2(width, height), uv0, uv1);
}

void DrawImage(uint64_t textureID, float x, float y, float width, float height) {
    bool flipY = false;
    auto* app = Application::GetInstance();
    if (app && app->GetRenderer() &&
        app->GetRenderer()->GetType() == RenderEngine::RendererType::OpenGL)
        flipY = true;

    ImVec2 uv0 = flipY ? ImVec2(0, 1) : ImVec2(0, 0);
    ImVec2 uv1 = flipY ? ImVec2(1, 0) : ImVec2(1, 1);
    ImGui::GetForegroundDrawList()->AddImage(
        static_cast<ImTextureID>(textureID),
        ImVec2(x, y), ImVec2(x + width, y + height), uv0, uv1);
}

static std::unordered_map<std::string, Sleak::Texture*> s_uiTextures;

uint64_t LoadTextureForUI(const std::string& filePath, float* outWidth, float* outHeight) {
    auto it = s_uiTextures.find(filePath);
    if (it != s_uiTextures.end()) {
        if (outWidth) *outWidth = static_cast<float>(it->second->GetWidth());
        if (outHeight) *outHeight = static_cast<float>(it->second->GetHeight());
        return it->second->GetImGuiTextureID();
    }

    auto* tex = RenderEngine::ResourceManager::CreateTexture(filePath);
    if (!tex) return 0;

    s_uiTextures[filePath] = tex;
    if (outWidth) *outWidth = static_cast<float>(tex->GetWidth());
    if (outHeight) *outHeight = static_cast<float>(tex->GetHeight());
    return tex->GetImGuiTextureID();
}

void ShutdownTextureCache() {
    // Must run before device teardown — cached textures own VkImage/memory
    for (auto& [path, tex] : s_uiTextures) delete tex;
    s_uiTextures.clear();
}

Sleak::Texture* CreateTextureFromPixels(uint32_t width, uint32_t height, const void* rgbaPixels, uint32_t maxMipLevels) {
    return RenderEngine::ResourceManager::CreateTextureFromMemory(
        rgbaPixels, width, height, TextureFormat::RGBA8, maxMipLevels);
}

unsigned char* LoadImagePixels(const char* path, int* w, int* h) {
    int channels;
    return stbi_load(path, w, h, &channels, 4); // force RGBA
}

void FreeImagePixels(unsigned char* pixels) {
    stbi_image_free(pixels);
}

}  // namespace Sleak::UI
