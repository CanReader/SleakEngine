# UI System {#ui_system}

`Sleak::UI` (`include/public/UI/UI.hpp`) is the engine's entire UI surface:
a namespace of free functions, no classes, no widget tree, no retained
state. It wraps Dear ImGui, which the engine vendors and links privately,
so a game builds its HUD, settings screens, and debug panels out of these
calls and never sees an ImGui type.

| Type | Role |
| :--- | :--- |
| `Sleak::UI::PanelFlags` | Window chrome and interactivity bits passed to `BeginPanel`. |
| `Sleak::UI::StyleColor` | Color slot indices for `PushStyleColor`, mirroring `ImGuiCol_`. |
| `Sleak::UI::StyleVar` | Style variable indices for `PushStyleVar`, mirroring `ImGuiStyleVar_`. |
| `Sleak::DebugOverlay` | The engine's own camera and performance panels, built on these same calls. |

---

## 1. Why ImGui Is Not Exposed

Dear ImGui lives in `vendors/imgui` and is compiled into the Engine target,
but its include directory is listed under `PRIVATE` in the engine's
`target_include_directories` block. A game that writes `#include <imgui.h>`
is reaching past a boundary CMake declared closed. On Linux the header may
still resolve through an incremental build's leftover include paths and
appear to work; a clean build, or any build on Windows, fails.

The private linkage is deliberate. ImGui's API changes between versions,
and four render backends plus the SDL3 platform backend
(`imgui_impl_sdl3`, `imgui_impl_vulkan`, `imgui_impl_opengl3`, and on
Windows `imgui_impl_dx11` and `imgui_impl_dx12`) are initialized and torn
down by the engine's renderers. Games that called ImGui directly would
depend on which backend happened to be active.

When a widget you want is missing, add it to `UI.hpp` and `src/UI/UI.cpp`
rather than routing around them. Most wrappers are one line.

---

## 2. Panels

```cpp
void BeginPanel(const char* name, float x, float y,
                float bgAlpha = 0.4f,
                int flags = PanelFlags_NoTitleBar | PanelFlags_AutoResize |
                            PanelFlags_NoMove | PanelFlags_NoInput |
                            PanelFlags_NoFocusOnAppear);
void EndPanel();
```

The default flags describe a passive HUD element: no title bar, sized to
its content, fixed in place, and transparent to the mouse. Clear
`PanelFlags_NoInput` for anything the player clicks, such as a settings
screen. The other flags are `PanelFlags_None`, `PanelFlags_NoTitleBar`,
`PanelFlags_AutoResize`, `PanelFlags_NoMove`, `PanelFlags_NoInput`, and
`PanelFlags_NoFocusOnAppear`.

`BeginPanel` applies the `x, y` position only when at least one of them is
non-zero, which leaves `SetNextWindowPos` free to place a panel itself:

```cpp
SetNextWindowPos(GetViewportWidth() * 0.5f - 200.0f, 80.0f);
SetNextWindowSize(400.0f, 0.0f);
BeginPanel("settings", 0.0f, 0.0f, 0.9f, PanelFlags_AutoResize);
// widgets
EndPanel();
```

Every `BeginPanel` needs its matching `EndPanel`, including on early-out
paths. The same pairing rule applies to `BeginGroup`/`EndGroup`,
`BeginChild`/`BeginChildSized` with `EndChild`, and
`BeginListBox`/`EndListBox`.

---

## 3. Widgets

Text takes printf-style format specifiers, not the fmt-style `{}`
placeholders the logging macros use. The two conventions sit side by side in
the same file, so it is an easy slip to make.

```cpp
void Text(const char* fmt, ...);
void TextColored(float r, float g, float b, float a, const char* fmt, ...);
void TextDisabled(const char* fmt, ...);
void TextWrapped(const char* fmt, ...);
```

Interactive widgets return `true` on the frame their value changes or the
button is pressed:

| Call | Behavior |
| :--- | :--- |
| `Button(label)`, `ButtonSized(label, w, h)` | True on the frame it is clicked. |
| `Checkbox(label, bool*)` | Writes through the pointer, true when toggled. |
| `DragFloat(label, float*, speed, min, max)` | Drag-to-edit scalar; `min == max` leaves it unclamped. |
| `Combo(label, int*, items[], count)` | Dropdown over a C array of strings. |
| `Selectable(label, selected)` | Selectable row, typically inside a list box. |
| `InputText(label, char*, bufSize)` | Fixed-buffer text field. |
| `InputTextString(label, std::string*)` | Text field backed by a `std::string`. |
| `ProgressBar(fraction, width, height, overlay)` | Non-interactive fill bar. |

Layout and cursor control cover `Separator`, `SameLine`, `Spacing`,
`Dummy`, `SetNextItemWidth`, `SetCursorPosX`, `SetCursorPosY`,
`GetCursorPosY`, and `GetContentRegionAvailWidth`. `GetViewportWidth` and
`GetViewportHeight` give the current framebuffer size for anchoring panels
against a window that resizes.

Styling pushes and pops in matched pairs, using the numeric enums that
mirror ImGui's own:

```cpp
PushStyleColor(StyleColor_Button, 0.16f, 0.20f, 0.28f, 1.0f);
PushStyleVar(StyleVar_FrameRounding, 4.0f);
PushStyleVarVec(StyleVar_ItemSpacing, 8.0f, 6.0f);
// widgets
PopStyleVar(2);
PopStyleColor(1);
```

---

## 4. Drawing and Images

Beyond widgets, the wrapper exposes the raw draw list for HUD elements that
are shapes rather than controls. `DrawLine`, `DrawRect`, `DrawFilledRect`,
and `DrawText` all take screen coordinates and unclamped float color
components, and they draw independently of any panel. A hotbar, a crosshair,
or a health bar is usually cheaper to build from these than from widgets.

Textures reach the UI through a `uint64_t` id rather than a `Sleak::Texture`
pointer:

```cpp
uint64_t LoadTextureForUI(const std::string& filePath,
                          float* outWidth = nullptr, float* outHeight = nullptr);
void Image(uint64_t textureID, float width, float height);
void DrawImage(uint64_t textureID, float x, float y, float width, float height);
```

`LoadTextureForUI` caches by path, so calling it every frame for the same
file is a hash lookup rather than a reload. `Image` places the texture at
the current cursor position inside a panel; `DrawImage` puts it at absolute
screen coordinates through the draw list. The engine calls
`ShutdownTextureCache()` during teardown, so games do not.

For pixels a game generates at runtime, such as an atlas built from
individual tiles, `CreateTextureFromPixels(width, height, rgbaPixels,
maxMipLevels)` returns a `Sleak::Texture*` whose
`Texture::GetImGuiTextureID()` supplies the id these calls want.
`LoadImagePixels` and `FreeImagePixels` decode an image file to raw RGBA
without creating a GPU texture at all, which suits atlas assembly.

---

## 5. Where UI Calls Belong in the Frame

The renderer opens the ImGui frame inside `BeginRender` and submits it
inside `EndRender`. `Application::Run` sits between the two and calls, in
order, `Scene::FixedUpdate`, `Scene::Update`, `Scene::LateUpdate`,
`GameBase::Loop`, and then `DebugOverlay::Render`.

\dot
digraph uiframe {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  begin [label="Renderer::BeginRender\nImGui::NewFrame", fillcolor="#22d3ee22", color="#22d3ee"];
  scene [label="Scene::Update\nScene::LateUpdate"];
  loop  [label="GameBase::Loop"];
  ovl   [label="DebugOverlay::Render"];
  end   [label="Renderer::EndRender\nImGui::Render + present", fillcolor="#22d3ee22", color="#22d3ee"];
  begin -> scene -> loop -> ovl -> end;
  scene -> end [label="Sleak::UI calls land here", style=dashed];
  loop  -> end [label="or here", style=dashed];
}
\enddot

Any `Sleak::UI` call made outside that window is dropped or asserts inside
ImGui, so draw the HUD from `Scene::Update`, `Scene::LateUpdate`, or
`GameBase::Loop`. Drawing from an event handler works only because handlers
dispatch synchronously from the window poll inside the same span.

A typical game splits its interface into one function per panel and calls
them from `Scene::Update`: a HUD panel with crosshair and stats, a settings
panel gated behind a key, and a hotbar drawn entirely from `DrawFilledRect`
and `DrawImage`. None of them hold state between frames beyond the game's
own variables, which is the point of an immediate-mode API.

---

## 6. The Engine's Own Overlay

`Sleak::DebugOverlay` is a worked example that ships with the engine. It
draws a camera panel and a performance panel through the same wrapper, and
`Application` constructs one automatically but starts both panels disabled.
See @ref debug_tools for its configuration and the metrics behind it.

---

## 7. Where to Look in the Source

| Question | File |
| :--- | :--- |
| Every wrapper function and enum | `include/public/UI/UI.hpp` |
| What each wrapper maps to in ImGui | `src/UI/UI.cpp` |
| Why ImGui is unreachable from a game | `CMakeLists.txt`, the `PRIVATE` block of `target_include_directories` |
| Where the ImGui frame opens and closes | `BeginRender`/`EndRender` in each `src/Graphics/*/*Renderer.cpp` |
| The engine's own panels | `src/Debug/DebugOverlay.cpp` |
