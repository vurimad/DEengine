#include "app/frame_clock.h"
#include "app/framework.h"
#include "core/log.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <string_view>

namespace {

// A command line that outlives the FrameworkArgs parsed from it (they point into it, as into main's).
struct CommandLine {
    char* argv[16] = {};
    int argc = 0;

    explicit CommandLine(std::initializer_list<const char*> flags) {
        REQUIRE(flags.size() < 16);
        argv[argc++] = const_cast<char*>("deengine");
        for (const char* flag : flags) {
            argv[argc++] = const_cast<char*>(flag);
        }
    }

    bool parse(de::app::FrameworkArgs& out) { return de::app::parse_args(argc, argv, out); }
};

} // namespace

TEST_CASE("the framework's flags are parsed") {
    CommandLine line({"--frames", "120", "--no-vsync", "--deadzone", "0.3", "--world", "00000000000000ab"});
    de::app::FrameworkArgs args;
    REQUIRE(line.parse(args));
    CHECK(args.frames == 120);
    CHECK_FALSE(args.vsync);
    CHECK(args.deadzone == doctest::Approx(0.3f));
    // The application's own flags are read through the framework's.
    REQUIRE(de::app::flag_value(args, "--world") != nullptr);
    CHECK(de::app::has_flag(args, "--no-vsync"));
    CHECK(de::app::flag_value(args, "--game-dir") == nullptr);
}

TEST_CASE("a deadzone that isn't a number in range is rejected") {
    const de::core::LogLevel saved = de::core::log_level();
    de::core::set_log_level(de::core::LogLevel::Fatal); // the rejections log errors by design
    for (const char* value : {"nan", "-0.1", "0.95", "abc", "0.2x", ""}) {
        CommandLine line({"--deadzone", value});
        de::app::FrameworkArgs args;
        CHECK_MESSAGE(!line.parse(args), value);
    }
    de::core::set_log_level(saved);
}

TEST_CASE("settings are read by name, past comments and blanks") {
    const std::string_view text = "\xEF\xBB\xBF"
                                  "first 1   # right after a byte order mark\r\n"
                                  "# engine settings\n"
                                  "\n"
                                  "world_size  7   # not this one\n"
                                  "  world   76d76b87e8562965   # the test world\r\n"
                                  "empty\n"
                                  "# world 0000000000000000\n";
    CHECK((de::app::setting(text, "world") == "76d76b87e8562965"));
    CHECK((de::app::setting(text, "world_size") == "7"));
    CHECK((de::app::setting(text, "first") == "1"));
    CHECK(de::app::setting(text, "empty").empty());
    CHECK(de::app::setting(text, "missing").empty());
    CHECK(de::app::setting(text, "engine").empty()); // only in a comment
}

TEST_CASE("the frame clock evens out a rhythm, and hands out the time measured") {
    // A loop that alternates 1 and 7 ms, as two frames back to back do: a steady 4 ms, once it's full.
    de::app::FrameClock clock;
    for (int i = 0; i < 12; ++i) {
        static_cast<void>(clock.step(i % 2 == 0 ? 0.001 : 0.007));
    }
    for (int i = 0; i < 24; ++i) {
        CHECK(clock.step(i % 2 == 0 ? 0.001 : 0.007) == doctest::Approx(0.004));
    }

    // From the first frame: not diluted by a window that hasn't filled.
    de::app::FrameClock fresh;
    CHECK(fresh.step(0.004) == doctest::Approx(0.004));
    CHECK(fresh.step(0.006) == doctest::Approx(0.005));

    // A hitch is spread, not lost: over the frames that see it, the time adds up to what was measured.
    de::app::FrameClock hitched;
    for (int i = 0; i < 12; ++i) {
        static_cast<void>(hitched.step(0.004));
    }
    double handed_out = 0.0;
    double measured = 0.0;
    double largest = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double step = i == 0 ? 0.100 : 0.004;
        const double out = hitched.step(step);
        measured += step;
        handed_out += out;
        largest = out > largest ? out : largest;
    }
    CHECK(handed_out == doctest::Approx(measured));
    CHECK(largest < 0.015); // not the whole 100 ms at once
}
