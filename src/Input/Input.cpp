#include <SDL3/SDL.h>

#include <Core/Logger.hpp>
#include <Input/Gamepad.hpp>
#include <Input/InputManager.hpp>
#include <Input/Keyboard.hpp>
#include <Input/Mouse.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../include/private/Input/InputBackend.hpp"

namespace Sleak {
namespace Input {

static_assert(static_cast<int>(GAMEPAD_BUTTON::COUNT) ==
              SDL_GAMEPAD_BUTTON_COUNT);
static_assert(static_cast<int>(GAMEPAD_BUTTON::TOUCHPAD) ==
              SDL_GAMEPAD_BUTTON_TOUCHPAD);
static_assert(static_cast<int>(GAMEPAD_AXIS::COUNT) == SDL_GAMEPAD_AXIS_COUNT);
static_assert(static_cast<int>(GAMEPAD_AXIS::RIGHT_TRIGGER) ==
              SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

namespace {

enum ButtonBits : uint8_t {
    kDown = 1 << 0,
    kPressed = 1 << 1,
    kReleased = 1 << 2,
    kWasDown = 1 << 3,
};

constexpr size_t kKeyCount = static_cast<size_t>(KEY_CODE::KEY__COUNT);
constexpr size_t kMouseButtonCount = 8;
constexpr size_t kPadButtonCount = static_cast<size_t>(GAMEPAD_BUTTON::COUNT);
constexpr size_t kPadAxisCount = static_cast<size_t>(GAMEPAD_AXIS::COUNT);

struct PadSlot {
    SDL_Gamepad* handle = nullptr;
    SDL_JoystickID id = 0;
    std::array<uint8_t, kPadButtonCount> buttons{};
    std::array<float, kPadAxisCount> axes{};
    std::array<float, kPadAxisCount> prevAxes{};
};

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const {
        return std::hash<std::string_view>{}(s);
    }
};

struct State {
    std::array<uint8_t, kKeyCount> keys{};
    std::array<uint8_t, kMouseButtonCount> mouse{};
    float mouseX = 0.0f, mouseY = 0.0f;
    float deltaX = 0.0f, deltaY = 0.0f;
    float wheelX = 0.0f, wheelY = 0.0f;
    std::array<PadSlot, Gamepad::MaxGamepads> pads{};
    float deadzone = 0.15f;

    std::unordered_map<std::string, InputAction, StringHash, std::equal_to<>>
        actions;

    std::vector<InputEventListener*> listeners;
    int listenerDepth = 0;
    bool listenersDirty = false;
};

State& S() {
    static State state;
    return state;
}

void Press(uint8_t& bits) {
    if (!(bits & kDown)) bits |= kDown | kPressed;
}

void Release(uint8_t& bits) {
    if (bits & kDown) {
        bits &= ~kDown;
        bits |= kReleased;
    }
}

void Roll(uint8_t& bits) { bits = (bits & kDown) ? (kDown | kWasDown) : 0; }

uint8_t KeyBits(KEY_CODE key) {
    auto i = static_cast<size_t>(key);
    return i < kKeyCount ? S().keys[i] : 0;
}

uint8_t MouseBits(MOUSE_CODE button) {
    auto i = static_cast<size_t>(button);
    return i < kMouseButtonCount ? S().mouse[i] : 0;
}

const PadSlot* Pad(int index) {
    if (index < 0 || index >= Gamepad::MaxGamepads) return nullptr;
    const PadSlot& pad = S().pads[index];
    return pad.handle ? &pad : nullptr;
}

PadSlot* PadById(SDL_JoystickID id) {
    for (auto& pad : S().pads)
        if (pad.handle && pad.id == id) return &pad;
    return nullptr;
}

uint8_t PadButtonBits(GAMEPAD_BUTTON button, int index) {
    const PadSlot* pad = Pad(index);
    auto i = static_cast<size_t>(button);
    return (pad && i < kPadButtonCount) ? pad->buttons[i] : 0;
}

float ApplyDeadzone(float value) {
    float dz = S().deadzone;
    float mag = std::fabs(value);
    if (mag <= dz) return 0.0f;
    return std::copysign((mag - dz) / (1.0f - dz), value);
}

float AxisContribution(float value, float threshold) {
    float v = threshold >= 0.0f ? value : -value;
    return v >= std::fabs(threshold) ? v : 0.0f;
}

template <typename Fn>
void ForEachListener(Fn&& fn) {
    State& s = S();
    ++s.listenerDepth;
    for (size_t i = 0; i < s.listeners.size(); ++i)
        if (s.listeners[i]) fn(*s.listeners[i]);
    if (--s.listenerDepth == 0 && s.listenersDirty) {
        std::erase(s.listeners, nullptr);
        s.listenersDirty = false;
    }
}

int OpenPad(SDL_JoystickID id) {
    State& s = S();
    if (PadById(id)) return -1;
    for (int i = 0; i < Gamepad::MaxGamepads; ++i) {
        PadSlot& pad = s.pads[i];
        if (pad.handle) continue;
        SDL_Gamepad* handle = SDL_OpenGamepad(id);
        if (!handle) {
            SLEAK_WARN("Failed to open gamepad: {}", SDL_GetError());
            return -1;
        }
        pad = PadSlot{};
        pad.handle = handle;
        pad.id = id;
        const char* name = SDL_GetGamepadName(handle);
        SLEAK_INFO("Gamepad {} connected: {}", i, name ? name : "unknown");
        return i;
    }
    SLEAK_WARN("Ignoring gamepad, all {} slots are in use",
               Gamepad::MaxGamepads);
    return -1;
}

int ClosePad(SDL_JoystickID id) {
    State& s = S();
    for (int i = 0; i < Gamepad::MaxGamepads; ++i) {
        PadSlot& pad = s.pads[i];
        if (!pad.handle || pad.id != id) continue;
        SDL_CloseGamepad(pad.handle);
        pad = PadSlot{};
        SLEAK_INFO("Gamepad {} disconnected", i);
        return i;
    }
    return -1;
}

}  // namespace

bool Keyboard::IsKeyPressed(KEY_CODE key) { return KeyBits(key) & kPressed; }
bool Keyboard::IsKeyHold(KEY_CODE key) { return KeyBits(key) & kDown; }
bool Keyboard::IsKeyReleased(KEY_CODE key) { return KeyBits(key) & kReleased; }

bool Mouse::IsButtonPressed(MOUSE_CODE button) {
    return MouseBits(button) & kPressed;
}
bool Mouse::IsButtonHold(MOUSE_CODE button) {
    return MouseBits(button) & kDown;
}
bool Mouse::IsButtonReleased(MOUSE_CODE button) {
    return MouseBits(button) & kReleased;
}

Math::Vector2D Mouse::GetPosition() {
    return Math::Vector2D(S().mouseX, S().mouseY);
}
Math::Vector2D Mouse::GetDelta() {
    return Math::Vector2D(S().deltaX, S().deltaY);
}
Math::Vector2D Mouse::GetWheel() {
    return Math::Vector2D(S().wheelX, S().wheelY);
}

void Mouse::SetCursorVisible(bool visible) {
    if (!(visible ? SDL_ShowCursor() : SDL_HideCursor()))
        SLEAK_ERROR("Failed to set cursor visibility: {}", SDL_GetError());
}

bool Gamepad::IsConnected(int index) { return Pad(index) != nullptr; }

int Gamepad::GetConnectedCount() {
    int count = 0;
    for (const auto& pad : S().pads) count += pad.handle ? 1 : 0;
    return count;
}

std::string Gamepad::GetName(int index) {
    const PadSlot* pad = Pad(index);
    const char* name = pad ? SDL_GetGamepadName(pad->handle) : nullptr;
    return name ? name : "";
}

bool Gamepad::IsButtonPressed(GAMEPAD_BUTTON button, int index) {
    return PadButtonBits(button, index) & kPressed;
}
bool Gamepad::IsButtonHold(GAMEPAD_BUTTON button, int index) {
    return PadButtonBits(button, index) & kDown;
}
bool Gamepad::IsButtonReleased(GAMEPAD_BUTTON button, int index) {
    return PadButtonBits(button, index) & kReleased;
}

float Gamepad::GetRawAxis(GAMEPAD_AXIS axis, int index) {
    const PadSlot* pad = Pad(index);
    auto i = static_cast<size_t>(axis);
    return (pad && i < kPadAxisCount) ? pad->axes[i] : 0.0f;
}

float Gamepad::GetAxis(GAMEPAD_AXIS axis, int index) {
    return ApplyDeadzone(GetRawAxis(axis, index));
}

void Gamepad::SetDeadzone(float deadzone) {
    S().deadzone = std::clamp(deadzone, 0.0f, 0.95f);
}
float Gamepad::GetDeadzone() { return S().deadzone; }

InputAction& InputAction::BindKey(KEY_CODE key) {
    keys.push_back(key);
    return *this;
}
InputAction& InputAction::BindMouseButton(MOUSE_CODE button) {
    mouseButtons.push_back(button);
    return *this;
}
InputAction& InputAction::BindGamepadButton(GAMEPAD_BUTTON button) {
    gamepadButtons.push_back(button);
    return *this;
}
InputAction& InputAction::BindGamepadAxis(GAMEPAD_AXIS axis, float threshold) {
    gamepadAxes.emplace_back(axis, threshold);
    return *this;
}
void InputAction::ClearBindings() {
    keys.clear();
    mouseButtons.clear();
    gamepadButtons.clear();
    gamepadAxes.clear();
}

namespace {

struct ActionState {
    bool down = false;
    bool wasDown = false;
    bool pressed = false;
    bool released = false;
    float value = 0.0f;
};

ActionState Evaluate(const InputAction& action) {
    ActionState st;
    auto digital = [&st](uint8_t bits) {
        st.down |= (bits & kDown) != 0;
        st.wasDown |= (bits & kWasDown) != 0;
        st.pressed |= (bits & kPressed) != 0;
        st.released |= (bits & kReleased) != 0;
        if (bits & kDown) st.value = 1.0f;
    };
    for (KEY_CODE key : action.keys) digital(KeyBits(key));
    for (MOUSE_CODE button : action.mouseButtons) digital(MouseBits(button));
    for (int p = 0; p < Gamepad::MaxGamepads; ++p) {
        const PadSlot* pad = Pad(p);
        if (!pad) continue;
        for (GAMEPAD_BUTTON button : action.gamepadButtons)
            digital(PadButtonBits(button, p));
        for (const auto& [axis, threshold] : action.gamepadAxes) {
            auto i = static_cast<size_t>(axis);
            if (i >= kPadAxisCount) continue;
            float now = AxisContribution(pad->axes[i], threshold);
            float before = AxisContribution(pad->prevAxes[i], threshold);
            st.value = std::max(st.value, now);
            st.down |= now > 0.0f;
            st.wasDown |= before > 0.0f;
            st.pressed |= now > 0.0f && before <= 0.0f;
            st.released |= now <= 0.0f && before > 0.0f;
        }
    }
    return st;
}

}  // namespace

bool InputAction::IsPressed() const {
    ActionState st = Evaluate(*this);
    return st.pressed && !st.wasDown;
}
bool InputAction::IsHold() const { return Evaluate(*this).down; }
bool InputAction::IsReleased() const {
    ActionState st = Evaluate(*this);
    return st.released && !st.down;
}
float InputAction::GetValue() const {
    return std::min(Evaluate(*this).value, 1.0f);
}

InputAction& InputManager::AddAction(std::string_view name) {
    auto& actions = S().actions;
    auto it = actions.find(name);
    if (it == actions.end())
        it = actions.emplace(std::string(name), InputAction(std::string(name)))
                 .first;
    return it->second;
}

InputAction* InputManager::GetAction(std::string_view name) {
    auto& actions = S().actions;
    auto it = actions.find(name);
    return it == actions.end() ? nullptr : &it->second;
}

void InputManager::RemoveAction(std::string_view name) {
    auto& actions = S().actions;
    auto it = actions.find(name);
    if (it != actions.end()) actions.erase(it);
}

void InputManager::ClearActions() { S().actions.clear(); }

bool InputManager::IsActionPressed(std::string_view name) {
    const InputAction* action = GetAction(name);
    return action && action->IsPressed();
}
bool InputManager::IsActionHold(std::string_view name) {
    const InputAction* action = GetAction(name);
    return action && action->IsHold();
}
bool InputManager::IsActionReleased(std::string_view name) {
    const InputAction* action = GetAction(name);
    return action && action->IsReleased();
}
float InputManager::GetActionValue(std::string_view name) {
    const InputAction* action = GetAction(name);
    return action ? action->GetValue() : 0.0f;
}

void InputManager::RegisterListener(InputEventListener* listener) {
    auto& listeners = S().listeners;
    if (listener && std::find(listeners.begin(), listeners.end(), listener) ==
                        listeners.end())
        listeners.push_back(listener);
}

void InputManager::UnregisterListener(InputEventListener* listener) {
    State& s = S();
    auto it = std::find(s.listeners.begin(), s.listeners.end(), listener);
    if (it == s.listeners.end()) return;
    if (s.listenerDepth > 0) {
        *it = nullptr;
        s.listenersDirty = true;
    } else {
        s.listeners.erase(it);
    }
}

namespace Backend {

void BeginFrame() {
    State& s = S();
    for (auto& bits : s.keys) Roll(bits);
    for (auto& bits : s.mouse) Roll(bits);
    for (auto& pad : s.pads) {
        for (auto& bits : pad.buttons) Roll(bits);
        pad.prevAxes = pad.axes;
    }
    s.deltaX = s.deltaY = 0.0f;
    s.wheelX = s.wheelY = 0.0f;
}

int ProcessEvent(const SDL_Event& event) {
    State& s = S();
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            auto code = static_cast<size_t>(event.key.scancode);
            if (code >= kKeyCount) break;
            bool down = event.type == SDL_EVENT_KEY_DOWN;
            if (down && event.key.repeat) break;
            down ? Press(s.keys[code]) : Release(s.keys[code]);

            KeyboardEvent raw;
            raw.keyCode = static_cast<KEY_CODE>(code);
            raw.Type = down ? KeyboardEventType::KeyPressed
                            : KeyboardEventType::KeyReleased;
            ForEachListener([&](InputEventListener& l) {
                l.onInputEvent(raw);
                down ? l.onKeyPress(raw) : l.onKeyRelease(raw);
            });
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (event.button.button < kMouseButtonCount) {
                uint8_t& bits = s.mouse[event.button.button];
                down ? Press(bits) : Release(bits);
            }
            s.mouseX = event.button.x;
            s.mouseY = event.button.y;

            MouseEvent raw;
            raw.mouseButton = event.button.button;
            raw.mouseX = static_cast<int>(event.button.x);
            raw.mouseY = static_cast<int>(event.button.y);
            ForEachListener([&](InputEventListener& l) {
                down ? l.onMousePress(raw) : l.onMouseRelease(raw);
            });
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            s.mouseX = event.motion.x;
            s.mouseY = event.motion.y;
            s.deltaX += event.motion.xrel;
            s.deltaY += event.motion.yrel;

            MouseEvent raw;
            raw.mouseX = static_cast<int>(event.motion.x);
            raw.mouseY = static_cast<int>(event.motion.y);
            raw.mouseXRel = static_cast<int>(event.motion.xrel);
            raw.mouseYRel = static_cast<int>(event.motion.yrel);
            ForEachListener([&](InputEventListener& l) { l.onMouseMove(raw); });
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            float flip =
                event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            s.wheelX += event.wheel.x * flip;
            s.wheelY += event.wheel.y * flip;
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
            return OpenPad(event.gdevice.which);
        case SDL_EVENT_GAMEPAD_REMOVED:
            return ClosePad(event.gdevice.which);
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            PadSlot* pad = PadById(event.gbutton.which);
            if (!pad || event.gbutton.button >= kPadButtonCount) break;
            bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            uint8_t& bits = pad->buttons[event.gbutton.button];
            down ? Press(bits) : Release(bits);

            GamepadEvent raw;
            raw.deviceID = static_cast<int>(pad - s.pads.data());
            raw.button = event.gbutton.button;
            ForEachListener([&](InputEventListener& l) {
                down ? l.onGamepadButtonPress(raw)
                     : l.onGamepadButtonRelease(raw);
            });
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            PadSlot* pad = PadById(event.gaxis.which);
            if (!pad || event.gaxis.axis >= kPadAxisCount) break;
            float value = std::max(event.gaxis.value / 32767.0f, -1.0f);
            pad->axes[event.gaxis.axis] = value;

            GamepadEvent raw;
            raw.deviceID = static_cast<int>(pad - s.pads.data());
            raw.axis = event.gaxis.axis;
            raw.axisValue = value;
            ForEachListener(
                [&](InputEventListener& l) { l.onGamepadAxisMotion(raw); });
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            ReleaseAll();
            break;
        default:
            break;
    }
    return -1;
}

void ReleaseAll() {
    State& s = S();
    for (auto& bits : s.keys) Release(bits);
    for (auto& bits : s.mouse) Release(bits);
}

void Shutdown() {
    for (auto& pad : S().pads) {
        if (pad.handle) SDL_CloseGamepad(pad.handle);
        pad = PadSlot{};
    }
}

}  // namespace Backend
}  // namespace Input
}  // namespace Sleak
