#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/hash_map.h"
#include "core/inline_array.h"
#include "core/result.h"
#include "core/string_id.h"

#include <cstdint>
#include <string_view>

namespace de::platform {

enum class ActionKind : uint8_t {
    Button, // pressed / held / released: attack_light, roll, use_item
    Axis,   // a signed value: move_x, camera_y
};

// An action resolved once at load time; reading its state through the handle is an array index.
struct ActionId {
    uint32_t index = UINT32_MAX;

    constexpr bool is_valid() const { return index != UINT32_MAX; }
    constexpr bool operator==(const ActionId&) const = default;
};

// One action's state for the current simulation tick (docs/design.md, Input).
struct ActionState {
    // Axis: the sum of its bindings, clamped to [-1, 1]; mouse motion adds pixels x scale on top.
    // Button: 1 while down, else 0.
    float value = 0.0f;
    bool down = false;            // held when the tick was sampled
    bool pressed = false;         // went down since the previous sample: a tap between two ticks is never lost
    bool released = false;        // went up since the previous sample
    uint64_t pressed_time_ns = 0; // first press since the previous sample (platform::time_ns clock)
};

// What a binding reads.
enum class InputSource : uint8_t {
    Key,
    MouseButton,
    MouseMotionX,
    MouseMotionY,
    GamepadButton,
    GamepadAxis,
};

// One decoded device event. pump_events creates these from OS events; tests create them directly.
// code numbering is internal to the platform layer: game code only ever sees actions.
struct RawInput {
    InputSource source = InputSource::Key;
    int32_t code = 0;
    float value = 0.0f; // buttons: 1 down / 0 up; gamepad axes: [-1, 1]; mouse motion: delta in pixels
    uint64_t timestamp_ns = 0;
    uint32_t device = 0; // which gamepad sent it (an OS instance id); 0 for keyboard and mouse
};

// Maps physical inputs to named actions: game code reads actions, never device codes
// (docs/design.md, Input). Bindings are text, so they can live in data files:
//     "key:Space"  "key:W"  "mouse:left"  "mouse:x"  "gamepad:a"  "gamepad:righttrigger"  "gamepad:leftx"
// Key names are SDL scancode names; gamepad names are SDL gamepad button and axis names.
//
// Device events are applied as they arrive (pump_events, once per rendered frame). sample()
// closes the window of events for one simulation tick and publishes one ActionState per action.
//
// One gamepad drives the actions at a time (single player): events from other pads are ignored
// until the active pad is idle and another pad presses a button or pushes a stick. An idle pad's
// stick noise, or the duplicate pad Steam Input exposes, can't overwrite the pad in use.
//
// Pushing and sampling never allocate. Not thread-safe: use it from the game thread.
struct BindTextOptions {
    bool replace = false;      // an action's first line in the text drops the bindings it had (a player's file)
    bool warn_unknown = false; // log lines naming actions that aren't added (typos), not just count them
};

class Input {
public:
    // Stick values below the deadzone read as zero. 0.3 by default: worn sticks drift (the dev pad
    // rests past 0.15), and a camera that turns by itself is worse than a little dead travel. A
    // player can still set their own (deengine --deadzone, an options-menu setting later).
    static constexpr float kDefaultStickDeadzone = 0.3f;
    static constexpr float kMaxStickDeadzone = 0.9f;
    static constexpr float kTriggerThreshold = 0.5f; // an axis bound to a button goes down past this...
    static constexpr float kTriggerRelease = 0.4f;   // ...and up below this, so resting near 0.5 can't flicker

    // Where a binding name points: for tools that validate binding files, and for tests that
    // build RawInput events. Fails on an unknown name.
    struct BindingSource {
        InputSource source = InputSource::Key;
        int32_t code = 0;
    };
    static core::Result<BindingSource> resolve_binding(const char* binding);

    Input() : Input(core::heap_allocator(core::MemoryTag::Platform)) {}
    explicit Input(core::Allocator& allocator);

    // Adding the same name twice returns the existing action (its kind must match).
    ActionId add_action(core::StringId name, ActionKind kind);

    // Invalid if no action has that name.
    ActionId find_action(core::StringId name) const;

    // Binds an input to an action. scale weights the input's share of an axis (key:S on move_y
    // with -1); on a button bound to an axis it picks the direction that counts as pressed.
    // Fails on an unknown binding name, or on mouse motion bound to a button.
    core::Result<void> bind(ActionId action, const char* binding, float scale = 1.0f);

    // Removes an action's bindings, letting go of what they hold.
    void unbind_all(ActionId action);

    // Binds from text, one binding a line: the action's name, the binding (in quotes when it has
    // spaces), and an optional scale. '#' starts a comment; a UTF-8 byte order mark is ignored.
    //     move_y              key:S                -1
    //     free_camera.fast    "key:Left Shift"
    // Actions must have been added; a line naming an action that isn't (a dev tool's, in a ship
    // build) is skipped. Returns how many were. Every line is checked before any is bound: a
    // malformed line or an unknown binding fails, naming the line, and changes nothing.
    core::Result<uint32_t> bind_text(std::string_view text, const BindTextOptions& options = {});

    // Applies one device event.
    void push(const RawInput& input);

    // Releases everything that is held, e.g. when the window loses focus, so nothing stays stuck down.
    void release_all(uint64_t timestamp_ns);

    // Takes effect from the next stick event. 0 disables it (to measure drift).
    void set_stick_deadzone(float deadzone);
    float stick_deadzone() const { return stick_deadzone_; }

    // A gamepad was unplugged: if it was the active one, its held inputs are released.
    void remove_gamepad(uint32_t device, uint64_t timestamp_ns);

    // Ends the event window for one simulation tick: publishes the states read by state(), then
    // clears the edges and the accumulated mouse motion.
    void sample();

    const ActionState& state(ActionId action) const;

    size_t action_count() const { return actions_.size(); }

private:
    using BindingList = core::InlineArray<uint32_t, 4>; // indices into bindings_

    struct Action {
        explicit Action(core::Allocator& allocator) : bindings(allocator) {}

        core::StringId name;
        ActionKind kind = ActionKind::Button;
        BindingList bindings;
        uint32_t bindings_down = 0; // button actions: how many bindings are held
        ActionState pending;        // edges since the last sample
        ActionState published;      // what state() returns
    };

    struct Binding {
        uint32_t action = 0;
        InputSource source = InputSource::Key;
        int32_t code = 0;
        float scale = 1.0f;
        float value = 0.0f;  // latest value: 0/1 for buttons, deadzoned [-1, 1] for gamepad axes
        float motion = 0.0f; // mouse motion accumulated since the last sample
        bool down = false;
    };

    static uint64_t source_key(InputSource source, int32_t code);

    void update_binding(Binding& binding, float value, uint64_t timestamp_ns);

    // Decides whether a gamepad event may drive actions, switching the active pad if it can.
    bool accept_gamepad_event(const RawInput& input);
    bool active_gamepad_idle() const;
    void release_gamepad(uint64_t timestamp_ns);

    core::Allocator* allocator_;
    core::Array<Action> actions_;
    core::Array<Binding> bindings_;
    core::HashMap<core::StringId, uint32_t> actions_by_name_;
    core::HashMap<uint64_t, BindingList> bindings_by_source_;
    uint32_t active_gamepad_ = 0;
    float stick_deadzone_ = kDefaultStickDeadzone;
};

} // namespace de::platform
