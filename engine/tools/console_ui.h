#pragma once

#include "console/console.h"
#include "core/array.h"
#include "core/fixed_string.h"

#include <cstdint>
#include <span>
#include <string_view>

struct ImGuiInputTextCallbackData;

namespace de::platform {
class Vfs;
}

namespace de::tools {

inline constexpr size_t kConsoleHistory = 64; // lines typed, kept between runs

// The suggestion an arrow picks of count: from the one lit (picked, or the first while none is: -1), up the list or
// down, round past either end.
int step_suggestion(int picked, size_t count, bool up);

// The console's faces (dev builds), drawn with Dear ImGui:
// - its window, as Unreal's: the key left of 1 (`) opens it over the bottom of the game's picture, in the editor's
//   Playground or over a game window, and closes it again (or Escape, with nothing typed);
// - its pieces for panels of their own: the log (the editor's Output panel) and a command line (the editor's status
//   bar).
// What's typed runs in the console (console/console.h) when Enter is pressed. As a name is typed, the names it could be
// show over the line with what they take and do: Tab takes the first (or the one picked with the arrows), and the
// arrows walk back through what was typed before when nothing is being completed; every command line shares that
// history. While one has the keyboard, the game sees no keys (the framework lets go of every action while ImGui takes
// text).
class ConsoleUi {
public:
    explicit ConsoleUi(console::Console& console);

    // What was typed in earlier runs: user:/console.history, a line each.
    void load_history(const platform::Vfs& vfs);
    void save_history(const platform::Vfs& vfs) const;

    bool is_open() const { return open_; }
    void set_open(bool open);
    // The window, over the game's picture: x, y its top-left corner in the UI's coordinates.
    void draw(float x, float y, float width, float height);

    // The log in a panel's space, the lines with filter in them (any case) if it isn't empty; it follows new lines
    // while it's scrolled to the end.
    void draw_log_panel(float width, float height, std::string_view filter);
    // A command line in a panel, its text its own.
    void draw_command_line(float width, const char* hint);
    // Text in that command line, and the keyboard on it (for pictures of it: the editor's --cmd).
    void set_command_line(std::string_view text);
    // Whether the keyboard is on that command line (last frame): for its panel to show it.
    bool command_line_active() const { return panel_line_.active; }
    void clear_log() { console_.clear_log(); }

private:
    struct Typed {
        core::FixedString<255> text;
    };
    struct InputLine {
        char text[256] = {};
        int history_at = -1; // walking back through the history: how far, -1 not
        int suggestion = -1; // the suggestion picked with the arrows, -1 none
        bool take_focus = false;
        bool escaped = false; // Escape let go of it with nothing typed (the window closes)
        bool shown = false;   // its suggestions show without the keyboard on it (set_command_line), until it's edited
        bool active = false;  // the keyboard was on it last frame
    };
    struct LogView {
        uint64_t seen_revision = ~uint64_t{0};
        bool at_end = true; // last frame: new lines scroll it on
    };
    struct Typing {
        ConsoleUi* ui = nullptr;
        InputLine* line = nullptr;
    };

    static int on_input(ImGuiInputTextCallbackData* data);
    // The line, its suggestions over it; true when Enter ran what was on it.
    bool input(InputLine& line, const char* id, float width, const char* hint);
    void run(InputLine& line);
    void draw_log(LogView& view, float width, float height, std::string_view filter);
    // The names what's typed could be (none once it has a space: the name is done).
    size_t suggestions(std::string_view typed, std::span<console::Entry> out) const;

    console::Console& console_;
    core::Array<Typed> history_;
    core::Array<uint16_t> matching_; // the log lines a filter keeps, this frame
    InputLine window_line_;
    InputLine panel_line_;
    LogView window_log_;
    LogView panel_log_;
    bool open_ = false;
};

} // namespace de::tools
