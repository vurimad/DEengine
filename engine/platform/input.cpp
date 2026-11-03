#include "platform/input.h"

#include "core/log.h"
#include "core/profile.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace de::platform {

namespace {

using ParsedBinding = Input::BindingSource;

// "prefix:name" -> name, if binding starts with prefix.
const char* name_after(const char* binding, const char* prefix) {
    const size_t length = std::strlen(prefix);
    if (std::strncmp(binding, prefix, length) == 0 && binding[length] == ':') {
        return binding + length + 1;
    }
    return nullptr;
}

bool parse_binding(const char* binding, ParsedBinding& out) {
    if (const char* name = name_after(binding, "key")) {
        const SDL_Scancode scancode = SDL_GetScancodeFromName(name);
        out = {.source = InputSource::Key, .code = static_cast<int32_t>(scancode)};
        return scancode != SDL_SCANCODE_UNKNOWN;
    }
    if (const char* name = name_after(binding, "mouse")) {
        struct NamedCode {
            const char* name;
            InputSource source;
            int32_t code;
        };
        static constexpr NamedCode kMouse[] = {
            {.name = "left", .source = InputSource::MouseButton, .code = SDL_BUTTON_LEFT},
            {.name = "middle", .source = InputSource::MouseButton, .code = SDL_BUTTON_MIDDLE},
            {.name = "right", .source = InputSource::MouseButton, .code = SDL_BUTTON_RIGHT},
            {.name = "x1", .source = InputSource::MouseButton, .code = SDL_BUTTON_X1},
            {.name = "x2", .source = InputSource::MouseButton, .code = SDL_BUTTON_X2},
            {.name = "x", .source = InputSource::MouseMotionX, .code = 0},
            {.name = "y", .source = InputSource::MouseMotionY, .code = 0},
        };
        for (const NamedCode& entry : kMouse) {
            if (std::strcmp(name, entry.name) == 0) {
                out = {.source = entry.source, .code = entry.code};
                return true;
            }
        }
        return false;
    }
    if (const char* name = name_after(binding, "gamepad")) {
        const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name);
        if (button != SDL_GAMEPAD_BUTTON_INVALID) {
            out = {.source = InputSource::GamepadButton, .code = static_cast<int32_t>(button)};
            return true;
        }
        const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name);
        if (axis != SDL_GAMEPAD_AXIS_INVALID) {
            out = {.source = InputSource::GamepadAxis, .code = static_cast<int32_t>(axis)};
            return true;
        }
        return false;
    }
    return false;
}

bool is_motion(InputSource source) {
    return source == InputSource::MouseMotionX || source == InputSource::MouseMotionY;
}

bool is_gamepad(InputSource source) {
    return source == InputSource::GamepadButton || source == InputSource::GamepadAxis;
}

// Stick values inside the deadzone read as zero; the rest is rescaled so output starts at 0.
float apply_deadzone(float value, float deadzone) {
    const float magnitude = std::fabs(value);
    if (magnitude < deadzone) {
        return 0.0f;
    }
    const float scaled = std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone));
    return value < 0.0f ? -scaled : scaled;
}

} // namespace

Input::Input(core::Allocator& allocator)
    : allocator_(&allocator), actions_(allocator), bindings_(allocator), actions_by_name_(allocator),
      bindings_by_source_(allocator) {}

uint64_t Input::source_key(InputSource source, int32_t code) {
    return (static_cast<uint64_t>(source) << 32U) | static_cast<uint32_t>(code);
}

ActionId Input::add_action(core::StringId name, ActionKind kind) {
    if (const uint32_t* existing = actions_by_name_.find(name)) {
        DE_ASSERT_MSG(actions_[*existing].kind == kind, "action re-added with a different kind");
        return ActionId{.index = *existing};
    }
    const auto index = static_cast<uint32_t>(actions_.size());
    Action& action = actions_.emplace(*allocator_);
    action.name = name;
    action.kind = kind;
    actions_by_name_.set(name, index);
    return ActionId{.index = index};
}

ActionId Input::find_action(core::StringId name) const {
    const uint32_t* index = actions_by_name_.find(name);
    return index != nullptr ? ActionId{.index = *index} : ActionId{};
}

core::Result<Input::BindingSource> Input::resolve_binding(const char* binding) {
    ParsedBinding parsed;
    if (binding == nullptr || !parse_binding(binding, parsed)) {
        return core::fail(core::ErrorCode::InvalidArgument, "unknown input binding");
    }
    return parsed;
}

core::Result<void> Input::bind(ActionId action, const char* binding, float scale) {
    DE_ASSERT_MSG(action.is_valid() && action.index < actions_.size(), "bind() on an unknown action");
    const core::Result<BindingSource> resolved = resolve_binding(binding);
    if (!resolved) {
        DE_LOG_WARN("input", "unknown input binding '%s'", binding != nullptr ? binding : "(null)");
        return core::fail(resolved.error());
    }
    const BindingSource parsed = *resolved;
    Action& target = actions_[action.index];
    if (target.kind == ActionKind::Button && is_motion(parsed.source)) {
        DE_LOG_WARN("input", "mouse motion '%s' can only drive an axis action", binding);
        return core::fail(core::ErrorCode::InvalidArgument, "mouse motion can only drive an axis action");
    }

    const auto index = static_cast<uint32_t>(bindings_.size());
    bindings_.push(Binding{.action = action.index, .source = parsed.source, .code = parsed.code, .scale = scale});
    target.bindings.push(index);

    const uint64_t key = source_key(parsed.source, parsed.code);
    BindingList* list = bindings_by_source_.find(key);
    if (list == nullptr) {
        list = &bindings_by_source_.set(key, BindingList(*allocator_));
    }
    list->push(index);
    return {};
}

void Input::push(const RawInput& input) {
    BindingList* bound = bindings_by_source_.find(source_key(input.source, input.code));
    if (bound == nullptr) {
        return;
    }
    if (is_gamepad(input.source) && !accept_gamepad_event(input)) {
        return;
    }
    for (const uint32_t index : *bound) {
        Binding& binding = bindings_[index];
        if (is_motion(binding.source)) {
            binding.motion += input.value;
        } else {
            update_binding(binding, input.value, input.timestamp_ns);
        }
    }
}

void Input::update_binding(Binding& binding, float value, uint64_t timestamp_ns) {
    Action& action = actions_[binding.action];
    const bool is_axis_source = binding.source == InputSource::GamepadAxis;
    binding.value = is_axis_source ? apply_deadzone(value, stick_deadzone_) : value;

    // When does this binding count as down: buttons at 0.5; axes past the trigger threshold in the
    // direction their scale points, and back up only below the lower release threshold.
    const float pressure = value * binding.scale;
    const bool down = is_axis_source ? pressure >= (binding.down ? kTriggerRelease : kTriggerThreshold) : value >= 0.5f;
    if (down == binding.down) {
        return;
    }
    binding.down = down;
    if (action.kind != ActionKind::Button) {
        return;
    }
    if (down) {
        if (action.bindings_down++ == 0) {
            if (!action.pending.pressed) {
                action.pending.pressed_time_ns = timestamp_ns;
            }
            action.pending.pressed = true;
        }
    } else if (--action.bindings_down == 0) {
        action.pending.released = true;
    }
}

void Input::release_all(uint64_t timestamp_ns) {
    for (Binding& binding : bindings_) {
        binding.motion = 0.0f;
        if (!is_motion(binding.source)) {
            update_binding(binding, 0.0f, timestamp_ns);
        }
    }
}

void Input::set_stick_deadzone(float deadzone) {
    DE_VERIFY_MSG(deadzone >= 0.0f && deadzone <= kMaxStickDeadzone,
                  "stick deadzone must be in [0, kMaxStickDeadzone]");
    stick_deadzone_ = deadzone;
}

void Input::remove_gamepad(uint32_t device, uint64_t timestamp_ns) {
    if (device == active_gamepad_) {
        release_gamepad(timestamp_ns);
        active_gamepad_ = 0;
    }
}

bool Input::accept_gamepad_event(const RawInput& input) {
    if (input.device == active_gamepad_) {
        return true;
    }
    // Another pad only takes over with a real press or push, and only from an idle active pad.
    const bool deliberate =
        input.source == InputSource::GamepadButton ? input.value >= 0.5f : std::fabs(input.value) >= stick_deadzone_;
    if (!deliberate || !active_gamepad_idle()) {
        return false;
    }
    release_gamepad(input.timestamp_ns);
    active_gamepad_ = input.device;
    return true;
}

bool Input::active_gamepad_idle() const {
    for (const Binding& binding : bindings_) {
        if (is_gamepad(binding.source) && (binding.down || binding.value != 0.0f)) {
            return false;
        }
    }
    return true;
}

void Input::release_gamepad(uint64_t timestamp_ns) {
    for (Binding& binding : bindings_) {
        if (is_gamepad(binding.source)) {
            update_binding(binding, 0.0f, timestamp_ns);
        }
    }
}

void Input::sample() {
    DE_PROFILE_ZONE("input_sample");
    for (Action& action : actions_) {
        ActionState& published = action.published;
        published.pressed = action.pending.pressed;
        published.released = action.pending.released;
        published.pressed_time_ns = action.pending.pressed_time_ns;

        if (action.kind == ActionKind::Button) {
            published.down = action.bindings_down > 0;
            published.value = published.down ? 1.0f : 0.0f;
        } else {
            float held = 0.0f;
            float motion = 0.0f;
            for (const uint32_t index : action.bindings) {
                Binding& binding = bindings_[index];
                if (is_motion(binding.source)) {
                    motion += binding.motion * binding.scale;
                    binding.motion = 0.0f;
                } else {
                    held += binding.value * binding.scale;
                }
            }
            published.value = std::clamp(held, -1.0f, 1.0f) + motion;
            published.down = published.value != 0.0f;
        }

        action.pending = ActionState{};
    }
}

const ActionState& Input::state(ActionId action) const {
    DE_ASSERT_MSG(action.is_valid() && action.index < actions_.size(), "state() of an unknown action");
    return actions_[action.index].published;
}

} // namespace de::platform

namespace de::platform {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// The next token of a line: quoted (spaces allowed) or up to the next space. Empty at the end.
std::string_view next_token(std::string_view& line) {
    while (!line.empty() && is_space(line.front())) {
        line.remove_prefix(1);
    }
    if (line.empty() || line.front() == '#') {
        line = {};
        return {};
    }
    if (line.front() == '"') {
        const size_t end = line.find('"', 1);
        const std::string_view token = line.substr(1, end == std::string_view::npos ? line.size() - 1 : end - 1);
        line.remove_prefix(end == std::string_view::npos ? line.size() : end + 1);
        return token;
    }
    size_t end = 0;
    while (end < line.size() && !is_space(line[end]) && line[end] != '#') {
        ++end;
    }
    const std::string_view token = line.substr(0, end);
    line.remove_prefix(end);
    return token;
}

} // namespace

core::Result<void> Input::bind_text(std::string_view text, uint32_t* skipped) {
    uint32_t line_number = 0;
    uint32_t unknown = 0;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++line_number;
        const std::string_view action_name = next_token(line);
        if (action_name.empty()) {
            continue; // blank or a comment
        }
        const std::string_view binding = next_token(line);
        const std::string_view scale_text = next_token(line);
        char binding_text[64] = {};
        char scale_buffer[32] = {};
        if (binding.empty() || binding.size() >= sizeof(binding_text) || scale_text.size() >= sizeof(scale_buffer) ||
            !next_token(line).empty()) {
            DE_LOG_ERROR("input", "bindings line %u: expected an action, a binding and an optional scale", line_number);
            return core::fail(core::ErrorCode::InvalidArgument, "malformed bindings line");
        }
        std::memcpy(binding_text, binding.data(), binding.size());
        float scale = 1.0f;
        if (!scale_text.empty()) {
            std::memcpy(scale_buffer, scale_text.data(), scale_text.size());
            char* parsed_end = nullptr;
            scale = std::strtof(scale_buffer, &parsed_end);
            if (parsed_end != scale_buffer + scale_text.size()) {
                DE_LOG_ERROR("input", "bindings line %u: '%s' isn't a number", line_number, scale_buffer);
                return core::fail(core::ErrorCode::InvalidArgument, "malformed bindings line");
            }
        }
        const ActionId action = find_action(core::StringId(action_name.data(), action_name.size()));
        if (!action.is_valid()) {
            ++unknown;
            continue;
        }
        if (const core::Result<void> bound = bind(action, binding_text, scale); !bound) {
            DE_LOG_ERROR("input", "bindings line %u: %s (%s)", line_number, bound.error().message, binding_text);
            return core::fail(bound.error());
        }
    }
    if (skipped != nullptr) {
        *skipped = unknown;
    }
    return {};
}

} // namespace de::platform
