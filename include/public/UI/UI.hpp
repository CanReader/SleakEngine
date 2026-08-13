#pragma once

#include <Core/OSDef.hpp>
#include <string>

namespace Sleak { class Texture; }

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
ENGINE_API void BeginChildSized(const char* name, float width, float height = 0.0f);
ENGINE_API void EndChild();

ENGINE_API float GetViewportWidth();
ENGINE_API float GetViewportHeight();

ENGINE_API void DrawLine(float x1, float y1, float x2, float y2,
                         float r, float g, float b, float a = 1.0f, float thickness = 1.0f);
ENGINE_API void DrawRect(float x, float y, float w, float h,
                         float r, float g, float b, float a = 1.0f, float thickness = 1.0f,
                         float rounding = 0.0f);
ENGINE_API void DrawFilledRect(float x, float y, float w, float h,
                               float r, float g, float b, float a = 1.0f,
                               float rounding = 0.0f);
ENGINE_API void DrawText(const char* text, float x, float y,
                         float r, float g, float b, float a = 1.0f);

ENGINE_API bool InputText(const char* label, char* buf, size_t bufSize);
ENGINE_API bool InputTextString(const char* label, std::string* str);
ENGINE_API bool ButtonSized(const char* label, float width, float height = 0.0f);
ENGINE_API bool Selectable(const char* label, bool selected);
ENGINE_API void SetNextItemWidth(float width);
ENGINE_API void Spacing();
ENGINE_API void Dummy(float width, float height);
ENGINE_API void PushStyleColor(int idx, float r, float g, float b, float a);
ENGINE_API void PopStyleColor(int count = 1);
ENGINE_API void PushStyleVar(int idx, float val);
ENGINE_API void PushStyleVarVec(int idx, float x, float y);
ENGINE_API void PopStyleVar(int count = 1);
ENGINE_API bool BeginListBox(const char* label, float width, float height);
ENGINE_API void EndListBox();
ENGINE_API void SetCursorPosX(float x);
ENGINE_API void SetCursorPosY(float y);
ENGINE_API float GetCursorPosY();
ENGINE_API float GetContentRegionAvailWidth();
ENGINE_API void SetNextWindowPos(float x, float y, bool always = false);
ENGINE_API void SetNextWindowSize(float w, float h, bool always = false);
ENGINE_API void ProgressBar(float fraction, float width = -1.0f, float height = 0.0f, const char* overlay = nullptr);
ENGINE_API void TextWrapped(const char* fmt, ...);

// Texture display
ENGINE_API void Image(uint64_t textureID, float width, float height);
ENGINE_API void DrawImage(uint64_t textureID, float x, float y, float width, float height);
ENGINE_API uint64_t LoadTextureForUI(const std::string& filePath, float* outWidth = nullptr, float* outHeight = nullptr);
ENGINE_API void ShutdownTextureCache();  // engine calls at teardown

// Create a texture from raw RGBA pixel data (for runtime atlas building etc.)
ENGINE_API Sleak::Texture* CreateTextureFromPixels(uint32_t width, uint32_t height, const void* rgbaPixels, uint32_t maxMipLevels = 0);

// Load image file into RGBA pixels (caller must free with FreeImagePixels)
ENGINE_API unsigned char* LoadImagePixels(const char* path, int* w, int* h);
ENGINE_API void FreeImagePixels(unsigned char* pixels);

// Style color indices (mirrors ImGuiCol_)
enum StyleColor : int {
    StyleColor_Text = 0,
    StyleColor_WindowBg = 2,
    StyleColor_ChildBg = 3,
    StyleColor_Button = 21,
    StyleColor_ButtonHovered = 22,
    StyleColor_ButtonActive = 23,
    StyleColor_FrameBg = 7,
    StyleColor_Header = 24,
    StyleColor_HeaderHovered = 25,
    StyleColor_HeaderActive = 26,
    StyleColor_ScrollbarBg = 14,
};

// Style var indices (must match ImGuiStyleVar_)
enum StyleVar : int {
    StyleVar_WindowPadding = 2,    // ImVec2
    StyleVar_WindowRounding = 3,   // float
    StyleVar_FramePadding = 11,    // ImVec2
    StyleVar_FrameRounding = 12,   // float
    StyleVar_ItemSpacing = 14,     // ImVec2
};

}  // namespace Sleak::UI
