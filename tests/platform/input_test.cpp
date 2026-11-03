#include "core/log.h"
#include "core/memory_stats.h"
#include "core/string_id.h"
#include "platform/input.h"

#include <doctest/doctest.h>

#include <initializer_list>

using de::platform::ActionId;
using de::platform::ActionKind;
using de::platform::Input;
using de::platform::RawInput;
using namespace de::core::literals;

namespace {

// A device event for a binding name, as pump_events would produce it.
RawInput event(const char* binding, float value, uint64_t timestamp_ns = 0) {
    const auto resolved = Input::resolve_binding(binding);
    REQUIRE(resolved.ok());
    return RawInput{.source = resolved->source, .code = resolved->code, .value = value, .timestamp_ns = timestamp_ns};
}

ActionId button(Input& input, de::core::StringId name, const char* binding, float scale = 1.0f) {
    const ActionId action = input.add_action(name, ActionKind::Button);
    REQUIRE(input.bind(action, binding, scale).ok());
    return action;
}

} // namespace

TEST_CASE("a button is pressed, held, then released across samples") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "key:Space");

    input.push(event("key:Space", 1.0f));
    input.sample();
    CHECK(input.state(roll).pressed);
    CHECK(input.state(roll).down);
    CHECK(input.state(roll).value == 1.0f);

    input.sample();
    CHECK_FALSE(input.state(roll).pressed);
    CHECK(input.state(roll).down);

    input.push(event("key:Space", 0.0f));
    input.sample();
    CHECK(input.state(roll).released);
    CHECK_FALSE(input.state(roll).down);
}

TEST_CASE("a tap between two samples is not lost") {
    Input input;
    const ActionId attack = button(input, "attack_light"_sid, "mouse:left");
    input.push(event("mouse:left", 1.0f));
    input.push(event("mouse:left", 0.0f));
    input.sample();
    CHECK(input.state(attack).pressed);
    CHECK(input.state(attack).released);
    CHECK_FALSE(input.state(attack).down);
}

TEST_CASE("the press timestamp is the first press since the last sample") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "gamepad:b");
    input.push(event("gamepad:b", 1.0f, 100));
    input.push(event("gamepad:b", 0.0f, 150));
    input.push(event("gamepad:b", 1.0f, 200));
    input.sample();
    CHECK(input.state(roll).pressed_time_ns == 100);
}

TEST_CASE("any of an action's bindings holds it down") {
    Input input;
    const ActionId attack = button(input, "attack_light"_sid, "mouse:left");
    REQUIRE(input.bind(attack, "gamepad:rightshoulder").ok());

    input.push(event("mouse:left", 1.0f));
    input.push(event("gamepad:rightshoulder", 1.0f));
    input.sample();
    CHECK(input.state(attack).pressed);

    input.push(event("mouse:left", 0.0f));
    input.sample();
    CHECK(input.state(attack).down);
    CHECK_FALSE(input.state(attack).released);

    input.push(event("gamepad:rightshoulder", 0.0f));
    input.sample();
    CHECK(input.state(attack).released);
}

TEST_CASE("keys build an axis and opposite keys cancel") {
    Input input;
    const ActionId move_y = input.add_action("move_y"_sid, ActionKind::Axis);
    REQUIRE(input.bind(move_y, "key:W", 1.0f).ok());
    REQUIRE(input.bind(move_y, "key:S", -1.0f).ok());

    input.push(event("key:W", 1.0f));
    input.sample();
    CHECK(input.state(move_y).value == 1.0f);

    input.push(event("key:S", 1.0f));
    input.sample();
    CHECK(input.state(move_y).value == 0.0f);

    input.push(event("key:W", 0.0f));
    input.sample();
    CHECK(input.state(move_y).value == -1.0f);
}

TEST_CASE("sticks have a deadzone and keys plus stick clamp to one") {
    Input input;
    const ActionId move_x = input.add_action("move_x"_sid, ActionKind::Axis);
    REQUIRE(input.bind(move_x, "gamepad:leftx").ok());
    REQUIRE(input.bind(move_x, "key:D").ok());

    input.push(event("gamepad:leftx", 0.1f)); // inside the deadzone
    input.sample();
    CHECK(input.state(move_x).value == 0.0f);
    CHECK_FALSE(input.state(move_x).down);

    const float halfway = -(Input::kDefaultStickDeadzone + (1.0f - Input::kDefaultStickDeadzone) * 0.5f);
    input.push(event("gamepad:leftx", halfway));
    input.sample();
    CHECK(input.state(move_x).value == doctest::Approx(-0.5f));

    input.push(event("gamepad:leftx", 1.0f));
    input.push(event("key:D", 1.0f));
    input.sample();
    CHECK(input.state(move_x).value == 1.0f);
}

TEST_CASE("a raised deadzone hides a drifting stick") {
    Input input;
    const ActionId move_x = input.add_action("move_x"_sid, ActionKind::Axis);
    REQUIRE(input.bind(move_x, "gamepad:leftx").ok());

    input.push(event("gamepad:leftx", 0.2f)); // a worn stick resting at 0.2
    input.sample();
    CHECK(input.state(move_x).down); // the default deadzone (0.15) lets it through

    input.set_stick_deadzone(0.25f);
    input.push(event("gamepad:leftx", 0.2f));
    input.sample();
    CHECK_FALSE(input.state(move_x).down);
    CHECK(input.stick_deadzone() == 0.25f);

    input.push(event("gamepad:leftx", 1.0f)); // full deflection still reaches 1
    input.sample();
    CHECK(input.state(move_x).value == 1.0f);
}

TEST_CASE("a trigger drives a button past the threshold") {
    Input input;
    const ActionId heavy = button(input, "attack_heavy"_sid, "gamepad:righttrigger");
    input.push(event("gamepad:righttrigger", 0.3f));
    input.sample();
    CHECK_FALSE(input.state(heavy).down);
    input.push(event("gamepad:righttrigger", 0.8f));
    input.sample();
    CHECK(input.state(heavy).pressed);
}

TEST_CASE("a trigger resting near the threshold doesn't flicker") {
    Input input;
    const ActionId heavy = button(input, "attack_heavy"_sid, "gamepad:righttrigger");
    input.push(event("gamepad:righttrigger", 0.51f));
    input.sample();
    CHECK(input.state(heavy).pressed);

    // Noise around 0.5 stays above the release threshold: no new edges.
    for (const float noise : {0.49f, 0.51f, 0.48f, 0.52f}) {
        input.push(event("gamepad:righttrigger", noise));
        input.sample();
        CHECK(input.state(heavy).down);
        CHECK_FALSE(input.state(heavy).pressed);
        CHECK_FALSE(input.state(heavy).released);
    }

    input.push(event("gamepad:righttrigger", Input::kTriggerRelease - 0.05f));
    input.sample();
    CHECK(input.state(heavy).released);
}

TEST_CASE("an idle second gamepad's stick noise doesn't disturb the active one") {
    Input input;
    const ActionId move_x = input.add_action("move_x"_sid, ActionKind::Axis);
    REQUIRE(input.bind(move_x, "gamepad:leftx").ok());

    RawInput pad_a = event("gamepad:leftx", 1.0f);
    pad_a.device = 1;
    input.push(pad_a);
    input.sample();
    CHECK(input.state(move_x).value == 1.0f);

    RawInput pad_b_noise = event("gamepad:leftx", 0.02f);
    pad_b_noise.device = 2;
    input.push(pad_b_noise);
    input.sample();
    CHECK(input.state(move_x).value == 1.0f);
}

TEST_CASE("a duplicate gamepad can't steal input while the active one is held") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "gamepad:b");
    RawInput press = event("gamepad:b", 1.0f);
    RawInput lift = event("gamepad:b", 0.0f);

    press.device = 1;
    input.push(press);
    press.device = 2; // e.g. Steam Input's virtual copy of the same pad
    lift.device = 2;
    input.push(press);
    input.push(lift);
    input.sample();
    CHECK(input.state(roll).down);
    CHECK_FALSE(input.state(roll).released);
}

TEST_CASE("another gamepad takes over once the active one is idle") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "gamepad:b");
    RawInput press = event("gamepad:b", 1.0f);
    RawInput lift = event("gamepad:b", 0.0f);

    press.device = 1;
    lift.device = 1;
    input.push(press);
    input.push(lift);
    input.sample();

    press.device = 2;
    input.push(press);
    input.sample();
    CHECK(input.state(roll).pressed);
    CHECK(input.state(roll).down);
}

TEST_CASE("unplugging the active gamepad releases what it held") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "gamepad:b");
    RawInput press = event("gamepad:b", 1.0f);
    press.device = 7;
    input.push(press);
    input.sample();
    input.remove_gamepad(7, 1000);
    input.sample();
    CHECK(input.state(roll).released);
    CHECK_FALSE(input.state(roll).down);
}

TEST_CASE("a negative scale picks the stick direction that presses a button") {
    Input input;
    const ActionId menu_up = button(input, "menu_up"_sid, "gamepad:lefty", -1.0f); // up is negative y
    input.push(event("gamepad:lefty", 0.9f));
    input.sample();
    CHECK_FALSE(input.state(menu_up).down);
    input.push(event("gamepad:lefty", -0.9f));
    input.sample();
    CHECK(input.state(menu_up).pressed);
}

TEST_CASE("mouse motion accumulates over a sample, then resets") {
    Input input;
    const ActionId camera_x = input.add_action("camera_x"_sid, ActionKind::Axis);
    REQUIRE(input.bind(camera_x, "mouse:x", 0.1f).ok());
    input.push(event("mouse:x", 10.0f));
    input.push(event("mouse:x", 5.0f));
    input.sample();
    CHECK(input.state(camera_x).value == doctest::Approx(1.5f));
    input.sample();
    CHECK(input.state(camera_x).value == 0.0f);
}

TEST_CASE("bad bindings are rejected") {
    const de::core::LogLevel saved = de::core::log_level();
    de::core::set_log_level(de::core::LogLevel::Error); // the rejections log warnings by design
    Input input;
    const ActionId action = input.add_action("roll"_sid, ActionKind::Button);
    CHECK_FALSE(input.bind(action, "key:NotAKey").ok());
    CHECK_FALSE(input.bind(action, "gamepad:nope").ok());
    CHECK_FALSE(input.bind(action, "mouse:z").ok());
    CHECK_FALSE(input.bind(action, "joystick:a").ok());
    CHECK_FALSE(input.bind(action, "Space").ok());
    CHECK_FALSE(input.bind(action, "mouse:x").ok()); // motion can't drive a button
    de::core::set_log_level(saved);
}

TEST_CASE("bindings read from text") {
    Input input;
    const ActionId roll = input.add_action("roll"_sid, ActionKind::Button);
    const ActionId move_x = input.add_action("move_x"_sid, ActionKind::Axis);
    const ActionId sprint = input.add_action("sprint"_sid, ActionKind::Button);
    uint32_t skipped = 99;
    const char* const text = "# a comment, then a blank line\n"
                             "\n"
                             "roll      key:Space\r\n"
                             "move_x    key:D\n"
                             "  move_x  key:A   -1   # left\n"
                             "sprint    \"key:Left Shift\"\n"
                             "jump      key:J\n"; // no such action: skipped
    REQUIRE(input.bind_text(text, &skipped).ok());
    CHECK(skipped == 1);
    input.push(event("key:Space", 1.0f));
    input.push(event("key:A", 1.0f));
    input.push(event("key:Left Shift", 1.0f));
    input.sample();
    CHECK(input.state(roll).pressed);
    CHECK(input.state(move_x).value == -1.0f);
    CHECK(input.state(sprint).down);
}

TEST_CASE("malformed bindings text is rejected") {
    const de::core::LogLevel saved = de::core::log_level();
    de::core::set_log_level(de::core::LogLevel::Fatal); // the rejections log errors by design
    Input input;
    input.add_action("roll"_sid, ActionKind::Button);
    input.add_action("move_x"_sid, ActionKind::Axis);
    CHECK_FALSE(input.bind_text("roll\n").ok());                       // no binding
    CHECK_FALSE(input.bind_text("move_x key:A -1 extra\n").ok());      // too many tokens
    CHECK_FALSE(input.bind_text("move_x key:A minus\n").ok());         // the scale isn't a number
    CHECK_FALSE(input.bind_text("move_x key:A 1.5x\n").ok());          // nor is this
    CHECK_FALSE(input.bind_text("roll key:NotAKey\n").ok());           // an unknown binding
    CHECK_FALSE(input.bind_text("roll key:Space\nroll Space\n").ok()); // stops at line 2
    de::core::set_log_level(saved);
}

TEST_CASE("release_all lets go of everything") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "key:Space");
    const ActionId move_x = input.add_action("move_x"_sid, ActionKind::Axis);
    REQUIRE(input.bind(move_x, "key:D").ok());

    input.push(event("key:Space", 1.0f));
    input.push(event("key:D", 1.0f));
    input.sample();
    input.release_all(500);
    input.sample();
    CHECK(input.state(roll).released);
    CHECK_FALSE(input.state(roll).down);
    CHECK(input.state(move_x).value == 0.0f);
}

TEST_CASE("actions are found by name and added once") {
    Input input;
    const ActionId roll = input.add_action("roll"_sid, ActionKind::Button);
    CHECK(input.add_action("roll"_sid, ActionKind::Button) == roll);
    CHECK(input.find_action("roll"_sid) == roll);
    CHECK_FALSE(input.find_action("jump"_sid).is_valid());
    CHECK(input.action_count() == 1);
}

TEST_CASE("unbound inputs are ignored") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "key:Space");
    input.push(event("key:Q", 1.0f));
    input.sample();
    CHECK_FALSE(input.state(roll).down);
}

TEST_CASE("pushing and sampling stay off the heap") {
    Input input;
    const ActionId roll = button(input, "roll"_sid, "key:Space");
    const ActionId move_x = input.add_action("move_x"_sid, ActionKind::Axis);
    REQUIRE(input.bind(move_x, "gamepad:leftx").ok());
    const RawInput press = event("key:Space", 1.0f);
    const RawInput stick = event("gamepad:leftx", 0.7f);

    const de::core::NoHeapAllocationScope no_alloc;
    for (int tick = 0; tick < 10; ++tick) {
        input.push(press);
        input.push(stick);
        input.sample();
    }
    CHECK(no_alloc.allocations() == 0);
    CHECK(input.state(roll).down);
}
