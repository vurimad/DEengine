#include "console/console.h"
#include "tools/console_ui.h"

#include <doctest/doctest.h>

#include <string_view>

using de::console::Args;
using de::console::Console;
using de::console::Entry;
using de::console::Line;
using de::console::Output;
using de::console::Severity;

namespace {

de::core::Allocator& tools_heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Tools);
}

// What a command saw: its arguments, joined.
struct Seen {
    int calls = 0;
    char words[128] = {};
};

void record(void* user, const Args& args, Output& out) {
    auto& seen = *static_cast<Seen*>(user);
    ++seen.calls;
    size_t length = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const int written = std::snprintf(seen.words + length, sizeof(seen.words) - length, "%s[%.*s]",
                                          i == 0 ? "" : " ", static_cast<int>(args[i].size()), args[i].data());
        length += static_cast<size_t>(written);
    }
    out.info("ran with %zu", args.size());
}

void refuse(void* /*user*/, const Args& /*args*/, Output& out) {
    out.error("no");
}

std::string_view last_line(const Console& console) {
    return console.log_line(console.log_size() - 1).text.view();
}

} // namespace

TEST_CASE("console: a command runs with its words, quotes grouping them, statements split on ';'") {
    Console console(tools_heap());
    Seen seen;
    console.add_command("camera.goto", "<x> <y> <z>", "moves the camera", &record, &seen);
    CHECK(console.execute("camera.goto 1 2.5 \"a b\""));
    CHECK(seen.calls == 1);
    CHECK((std::string_view(seen.words) == "[1] [2.5] [a b]"));
    // Names in any case; two statements, a ';' in quotes kept.
    CHECK(console.execute("CAMERA.GOTO x; camera.goto \"y;z\""));
    CHECK(seen.calls == 3);
    CHECK((std::string_view(seen.words) == "[y;z]"));
    // What it says is kept, after the line itself.
    de::core::Array<Line> said(tools_heap());
    CHECK(console.execute("camera.goto", &said));
    REQUIRE(said.size() == 2);
    CHECK(said[0].severity == Severity::Echo);
    CHECK((said[0].text.view() == "camera.goto"));
    CHECK((said[1].text.view() == "ran with 0"));
    CHECK((last_line(console) == "ran with 0"));
    // An open quote is refused, and nothing runs.
    CHECK_FALSE(console.execute("camera.goto \"open"));
    CHECK(seen.calls == 4);
}

TEST_CASE("console: variables say their value, take a new one, and refuse what they can't hold") {
    Console console(tools_heap());
    bool taa = false;
    int32_t samples = 4;
    float exposure = 1.0f;
    console.add_variable("r.taa", "temporal anti-aliasing", &taa);
    console.add_variable("r.samples", "samples", &samples, 1, 16);
    console.add_variable("r.exposure", "exposure", &exposure, -4.0f, 4.0f);
    CHECK(console.execute("r.taa"));
    CHECK((last_line(console) == "r.taa = off"));
    CHECK(console.execute("r.taa on; r.samples 8; r.exposure -1.5"));
    CHECK(taa);
    CHECK(samples == 8);
    CHECK(exposure == doctest::Approx(-1.5f));
    CHECK_FALSE(console.execute("r.samples 99"));
    CHECK(samples == 8);
    CHECK_FALSE(console.execute("r.taa maybe"));
    CHECK(taa);
    CHECK(console.log_line(console.log_size() - 1).severity == Severity::Error);
}

TEST_CASE("console: an unknown name fails with what it might have been; help says what there is") {
    Console console(tools_heap());
    Seen seen;
    console.add_command("stat", "[section]", "the frame stats", &record, &seen);
    console.add_command("stats", "<sections>", "exactly these", &record, &seen);
    console.add_command("fail", "", "always fails", &refuse, nullptr);
    CHECK_FALSE(console.execute("sta"));
    CHECK((last_line(console).find("Did you mean: stat, stats") != std::string_view::npos));
    CHECK_FALSE(console.execute("fail"));
    CHECK(console.execute("help stat"));
    CHECK((last_line(console).find("the frame stats") != std::string_view::npos));
    CHECK(console.execute("help"));
    CHECK(console.log_size() > 3);
}

TEST_CASE("console: completion puts names that start with the text first, and remove() forgets an owner's") {
    Console console(tools_heap());
    Seen seen;
    int owner = 0;
    console.add_command("r.view", "[name]", "", &record, &owner);
    console.add_command("view.reset", "", "", &record, &seen);
    console.add_command("render.stats", "", "", &record, &seen);
    Entry found[8];
    REQUIRE(console.complete("r", found) >= 2);
    CHECK((found[0].name.view() == "r.view"));
    CHECK((found[1].name.view() == "render.stats"));
    // "view" starts one name and is inside another: the start first.
    REQUIRE(console.complete("view", found) == 2);
    CHECK((found[0].name.view() == "view.reset"));
    CHECK((found[1].name.view() == "r.view"));
    console.remove(&owner);
    CHECK(console.complete("r.v", found) == 0);
    CHECK_FALSE(console.execute("r.view"));
}

TEST_CASE("console: the log keeps the last lines, oldest first") {
    Console console(tools_heap());
    for (size_t i = 0; i < de::console::kLogLines + 10; ++i) {
        char text[32];
        static_cast<void>(std::snprintf(text, sizeof(text), "line %zu", i));
        console.print(Severity::Info, text);
    }
    CHECK(console.log_size() == de::console::kLogLines);
    CHECK((console.log_line(0).text.view() == "line 10"));
    CHECK((last_line(console) == "line 521"));
}

TEST_CASE("console: words read as booleans, numbers and whole numbers, or don't") {
    bool flag = false;
    CHECK(Args::parse_bool("On", flag));
    CHECK(flag);
    CHECK(Args::parse_bool("0", flag));
    CHECK_FALSE(flag);
    CHECK_FALSE(Args::parse_bool("2", flag));
    double number = 0.0;
    CHECK(Args::parse_double("-12.5", number));
    CHECK(number == doctest::Approx(-12.5));
    CHECK_FALSE(Args::parse_double("12m", number));
    int32_t whole = 0;
    CHECK(Args::parse_int("42", whole));
    CHECK(whole == 42);
    CHECK_FALSE(Args::parse_int("4.2", whole));
}

TEST_CASE("console: the engine's log comes in through the feed, the console's own lines once") {
    Console console(tools_heap());
    de::console::LogFeed feed(tools_heap());
    feed.write(
        {.level = de::core::LogLevel::Warning, .channel = "render", .text = "a shader failed", .file = "", .line = 0});
    feed.write({.level = de::core::LogLevel::Info, .channel = "console", .text = "> stat gpu", .file = "", .line = 0});
    CHECK(console.log_size() == 0); // nothing until the main thread takes them
    feed.take(console);
    REQUIRE(console.log_size() == 1);
    CHECK((last_line(console) == "render: a shader failed"));
    CHECK(console.log_line(0).severity == Severity::Warning);
    // More than it holds between takes: the newest stay, and it says some were skipped.
    for (size_t i = 0; i < de::console::kLogFeedLines + 5; ++i) {
        feed.write({.level = de::core::LogLevel::Info, .channel = "world", .text = "tick", .file = "", .line = 0});
    }
    feed.take(console);
    CHECK(console.log_size() == 2 + de::console::kLogFeedLines);
    CHECK(console.log_line(1).text.view().find("5 log lines skipped") != std::string_view::npos);
    console.clear_log();
    CHECK(console.log_size() == 0);
}

TEST_CASE("console window: the arrows move from the suggestion that's lit, the first one included") {
    using de::tools::step_suggestion;
    // Nothing picked yet lights the first (nearest the line): up moves off it at once.
    CHECK(step_suggestion(-1, 3, true) == 1);
    CHECK(step_suggestion(-1, 3, false) == 2); // round to the far end
    CHECK(step_suggestion(1, 3, true) == 2);
    CHECK(step_suggestion(2, 3, true) == 0);
    CHECK(step_suggestion(0, 3, false) == 2);
    CHECK(step_suggestion(-1, 1, true) == 0);
    CHECK(step_suggestion(0, 0, true) == -1);
}
