# Events and Input {#events_and_input}

This page describes the event dispatch system and the state of hardware
input handling in SleakEngine.

| Type | Role |
| :--- | :--- |
| `Sleak::EventDispatcher` | All-static dispatch hub; holds the handler registry and delivers events. |
| `Sleak::Event` | Base class for every event; carries type, name, category flags, and a `Handled` flag. |
| `Sleak::Delegate<Args...>` | `std::function` wrapper the registry stores handlers as. |
| `Sleak::Events::*` | Window and application events (resize, open, fullscreen, close, tick). |
| `Sleak::Events::Input::*` | Keyboard and mouse events. |
| `Sleak::Input::KEY_CODE` / `MOUSE_CODE` | Key and mouse button enumerations. |
| `Sleak::Input::InputManager` / `Keyboard` | Declared polling API with no implementation; see section 2. |

\dot
digraph events {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  sdl   [label="SDL_PollEvent\nWindow::Update()"];
  disp  [label="Sleak::DispatchEvent<T>(args...)"];
  hub   [label="EventDispatcher::DispatchEvent<T>", fillcolor="#22d3ee22", color="#22d3ee"];
  reg   [label="handler registry\nmap<EventType, vector<IDelegate>>"];
  del   [label="EventDelegate<T>::Execute()"];
  h1    [label="YourScene::OnKeyPressed"];
  h2    [label="FirstPersonController::OnMouseMoved"];
  sdl -> disp [label="construct event"];
  disp -> hub;
  hub -> reg [label="look up EventType"];
  reg -> del [label="dynamic_pointer_cast"];
  del -> h1;
  del -> h2;
}
\enddot

One SDL poll becomes a typed engine event, and the dispatcher hands it to
every handler registered for that event type before returning.

---

## 1. Events (`Sleak::EventDispatcher`)

The event dispatch hub is `Sleak::EventDispatcher`
(`include/public/Events/Event.hpp`), an all-static class backed by an
`unordered_map<EventType, vector<shared_ptr<IDelegate>>>`:

```cpp
template<typename T, typename EventT>
static std::string RegisterEventHandler(T* instance, void (T::*memberFunction)(const EventT&));

template<typename EventT>
static std::string RegisterEventCallback(std::function<void(const EventT&)> callback);

static void UnregisterEvent(EventType type, std::string id);
static void UnregisterEvents(EventType type);
static void UnregisterAllEvents();

template<typename EventT>
static void DispatchEvent(const EventT& event);
```

A free helper, `Sleak::DispatchEvent<T>(Args&&... args)`, constructs a `T`
and forwards it to `EventDispatcher::DispatchEvent`. `src/Core/Window.cpp`
calls this to turn raw SDL input into engine events; game code registers
handlers with `EventDispatcher::RegisterEventHandler(this, &Class::Method)`,
as used throughout `src/Scene/FirstPersonController.cpp`.

`Sleak::Event` (also `Events/Event.hpp`) is the base class:
`GetEventType()`, `GetName()`, `GetCategoryFlags()`, `ToString()`, and a
public `Handled` flag. Real subclasses:

- `Sleak::Events` (`Events/ApplicationEvent.hpp`): `WindowResizeEvent`,
  `WindowOpenEvent`, `WindowFullScreen`, `WindowFocusEvent`,
  `WindowLostFocusEvent`, `WindowMovedEvent`, `WindowCloseEvent`,
  `TickEvent`, `UpdateEvent`, `RenderEvent`.
- `Sleak::Events::Input` (`Events/KeyboardEvent.hpp`): `KeyEvent` (base),
  `KeyPressedEvent` (has `IsRepeat()`), `KeyReleasedEvent`, `KeyTypedEvent`
  (UTF-8 in `GetText()`, sent while SDL text input is active).
- `Sleak::Events::Input` (`Events/MouseEvent.hpp`): `MouseMovedEvent`,
  `MouseScrolledEvent`, `MouseButtonEvent` (base),
  `MouseButtonPressedEvent`, `MouseButtonReleasedEvent`.
- `Sleak::Events::Input` (`Events/GamepadEvent.hpp`):
  `GamepadConnectedEvent`, `GamepadDisconnectedEvent`, both carrying the
  `Input::Gamepad` index.

Underneath `EventDispatcher`, `Sleak::Delegate<Args...>`
(`include/public/Events/Delegate.hpp`) wraps a `std::function<void(Args...)>`.
It always returns `void` and is built on `std::function`,
`std::shared_ptr`, and `std::vector`, so it is not allocation-free.

Two macros in `Events/KeyboardEvent.hpp` test a key inside an event handler:

```cpp
#define if_key_press(key) if (!e.IsRepeat() && e.GetKeyCode() == Sleak::Input::KEY_CODE::key)
#define if_key_down(key)  if (e.GetKeyCode() == Sleak::Input::KEY_CODE::key)
```

`if_key_press` is edge-triggered (fires once per press, ignores repeats);
`if_key_down` is level-triggered (fires on every repeat as well).

---

## 2. Input

Next to the events there is a per-frame polling API in
`include/public/Input/`. `Window::Update()` resets it at the start of every
frame and then folds each SDL event into it, so anything that runs in the
scene update sees this frame's input. Game code never needs SDL for it.
Everything is static and main-thread only.

- `Input::Keyboard`: `IsKeyPressed` / `IsKeyHold` / `IsKeyReleased(KEY_CODE)`.
  Pressed and released are edges for this frame and survive a tap that goes
  down and up between two frames. OS key repeat is ignored.
- `Input::Mouse`: the same three for `MOUSE_CODE`, plus `GetPosition()`,
  `GetDelta()` (motion since last frame, also valid in relative mode) and
  `GetWheel()`. `SetCursorVisible()` shows or hides the cursor.
- `Input::Gamepad`: up to `MaxGamepads` (4) controllers through SDL3's
  gamepad API, with hot plug. Buttons use `GAMEPAD_BUTTON`, axes use
  `GAMEPAD_AXIS`. `GetAxis()` applies a per-axis deadzone (0.15 by default,
  `SetDeadzone()`), `GetRawAxis()` does not.
- `Input::InputManager`: named actions. `AddAction("Jump")` returns an
  `InputAction` you bind keys, mouse buttons, gamepad buttons and gamepad
  axes (with a threshold) to, then query with `IsActionPressed` /
  `IsActionHold` / `IsActionReleased` / `GetActionValue`. Pressed and
  released are for the action as a whole across all of its bindings.
  `RegisterListener` / `UnregisterListener` still exist for raw
  `InputEventListener` callbacks.

```cpp
using namespace Sleak::Input;
InputManager::AddAction("Jump")
    .BindKey(KEY_CODE::KEY__SPACE)
    .BindGamepadButton(GAMEPAD_BUTTON::SOUTH);

if (InputManager::IsActionPressed("Jump")) Jump();
Math::Vector2D look = Mouse::GetDelta();
```

When the window loses focus every held key and mouse button is released,
so nothing stays stuck down. `FirstPersonController` and
`FreeLookCameraController` read their movement keys and mouse delta from
this API. Key and mouse codes are `Sleak::Input::KEY_CODE` and
`Sleak::Input::MOUSE_CODE` (`include/public/Input/KeyCodes.hpp`).
`KEY_CODE` values line up with SDL scancodes, and `MOUSE_CODE::ButtonLeft`
equals SDL's `Button1` / `SDL_BUTTON_LEFT`.

---

## 3. Dispatch Semantics

Dispatch is synchronous and single-threaded. `DispatchEvent` runs every
matching handler on the calling thread before it returns, so an event
raised from `Window::Update()` is fully handled before the frame's scene
update begins. There is no event queue, no deferred delivery, and no
ordering guarantee between handlers beyond registration order.

`DispatchEvent` copies the handler vector before iterating it, so a handler
may register or unregister handlers while it runs without invalidating the
loop. Handlers added during dispatch do not receive the event currently
being delivered.

`RegisterEventHandler` returns a `std::string` id. Keep it and pass it to
`UnregisterEvent(type, id)` when the registering object dies. A handler
bound to a destroyed `this` is a dangling call, and the dispatcher has no
way to detect it. Unregister in the same place you destroy the object,
typically a scene's destructor or `OnDeactivate`.

---

## 4. Where to Look in the Source

| Question | File |
| :--- | :--- |
| How registration and dispatch work | `include/public/Events/Event.hpp` |
| How a handler is stored and invoked | `include/public/Events/Delegate.hpp` |
| Where SDL events become engine events | `src/Core/Window.cpp` (`Window::Update`) |
| Which events exist | `include/public/Events/ApplicationEvent.hpp`, `KeyboardEvent.hpp`, `MouseEvent.hpp` |
| Key and mouse code values | `include/public/Input/KeyCodes.hpp` |
| A real handler-based controller | `src/Scene/FirstPersonController.cpp` |
