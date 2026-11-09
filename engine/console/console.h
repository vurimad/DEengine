#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/log.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string_view>

namespace de::console {

inline constexpr size_t kMaxWords = 16;      // a command's name and its arguments
inline constexpr size_t kLineLength = 255;   // a line the console says
inline constexpr size_t kLogLines = 512;     // what it keeps of what it said
inline constexpr size_t kNameLength = 63;    // "r.fog_low_camera", "camera.goto"
inline constexpr size_t kTextLength = 191;   // a usage or a help line
inline constexpr size_t kLogFeedLines = 256; // engine log lines waiting for the main thread

enum class Severity : uint8_t {
    Info,
    Warning,
    Error,
    Echo, // a command as it was run
};

struct Line {
    Severity severity = Severity::Info;
    core::FixedString<kLineLength> text;
};

// What a command says back, line by line: into the console's log, and to whoever ran it (the agents' socket).
class Output {
public:
    explicit Output(core::Array<Line>& lines) : lines_(lines) {}

    void say(Severity severity, std::string_view text);
    template <typename... Args>
    void info(const char* format, Args... args) {
        say_formatted(Severity::Info, format, args...);
    }
    template <typename... Args>
    void error(const char* format, Args... args) {
        say_formatted(Severity::Error, format, args...);
        failed_ = true;
    }
    bool failed() const { return failed_; }

private:
    template <typename... Args>
    void say_formatted(Severity severity, const char* format, Args... args) {
        if constexpr (sizeof...(Args) == 0) {
            say(severity, format);
        } else {
            char text[kLineLength + 1];
            static_cast<void>(std::snprintf(text, sizeof(text), format, args...));
            say(severity, text);
        }
    }

    core::Array<Line>& lines_;
    bool failed_ = false;
};

// A command's words after its name, as typed: quotes group words ("a b"), and are gone.
class Args {
public:
    explicit Args(std::span<const std::string_view> words) : words_(words) {}

    size_t size() const { return words_.size(); }
    bool empty() const { return words_.empty(); }
    std::string_view operator[](size_t index) const { return words_[index]; }

    // False (the value untouched) when the word isn't one: 1 0 on off true false yes no.
    static bool parse_bool(std::string_view word, bool& value);
    static bool parse_float(std::string_view word, float& value);
    static bool parse_double(std::string_view word, double& value);
    static bool parse_int(std::string_view word, int32_t& value);

private:
    std::span<const std::string_view> words_;
};

using Handler = void (*)(void* user, const Args& args, Output& out);

// A name the console knows, for completing and listing.
struct Entry {
    core::FixedString<kNameLength> name;
    core::FixedString<kTextLength> usage; // the arguments it takes: "<section>", "[on | off]"
    core::FixedString<kTextLength> help;
};

// The engine's console (docs/design.md, AI-friendly: the command API): named commands and variables, run from text,
// "stat gpu" or "r.taa 0; camera.goto 10 2 40". The console window (tools/console_ui.h), the command line (--exec)
// and the agents' socket (tools/agent_server.h) all run them the same way. Lines are split on ';' outside quotes;
// a line's first word names a command or a variable, the rest are its arguments. A variable alone says its value,
// with a value sets it. What runs says back lines, kept in a log (the last kLogLines). Main thread only.
class Console {
public:
    explicit Console(core::Allocator& allocator);

    // A command: usage names its arguments, help says what it does. user is handed back to the handler; remove()
    // forgets everything added with it (an application's commands, when it shuts down).
    void add_command(std::string_view name, std::string_view usage, std::string_view help, Handler handler, void* user);
    // Variables where they live: the console reads and writes them there.
    void add_variable(std::string_view name, std::string_view help, bool* value, void* user = nullptr);
    void add_variable(std::string_view name, std::string_view help, int32_t* value, int32_t min, int32_t max,
                      void* user = nullptr);
    void add_variable(std::string_view name, std::string_view help, float* value, float min, float max,
                      void* user = nullptr);
    void remove(const void* user);

    // Runs a line: false if anything in it failed (an unknown name, a bad argument). What it says goes into the log,
    // after the line itself, and into said if it's there.
    bool execute(std::string_view text, core::Array<Line>* said = nullptr);

    // Names that start with prefix (any case), then those that have it elsewhere; alphabetical within each, at most
    // out's capacity. How many.
    size_t complete(std::string_view prefix, std::span<Entry> out) const;
    size_t size() const { return entries_.size(); }

    // The log, oldest first.
    size_t log_size() const { return log_count_; }
    const Line& log_line(size_t index) const;
    // A change counter: it moves whenever the log does.
    uint64_t log_revision() const { return log_revision_; }
    void print(Severity severity, std::string_view text);
    void clear_log();

private:
    enum class Kind : uint8_t { Command, Bool, Int, Float };
    struct Item {
        Entry entry;
        Kind kind = Kind::Command;
        Handler handler = nullptr;
        void* user = nullptr;
        void* value = nullptr;
        float min = 0.0f;
        float max = 0.0f;
    };

    Item& add(std::string_view name, Kind kind, void* user);
    const Item* find(std::string_view name) const;
    void run(std::span<const std::string_view> words, Output& out);
    static void help(void* self, const Args& args, Output& out);
    void keep(std::span<const Line> lines);

    core::Array<Item> entries_;
    core::Array<Line> scratch_; // one execute()'s lines
    core::Array<Line> printed_; // one print()'s lines
    core::Array<Line> log_;     // kLogLines, a ring
    size_t log_start_ = 0;
    size_t log_count_ = 0;
    uint64_t log_revision_ = 0;
};

// The engine's log into a console's (the editor's Output panel, the console window): a log sink, written on whatever
// thread logs, holding up to kLogFeedLines until the main thread takes them in (take(), once a frame). What the console
// logs itself (its "console" channel) it has already. More lines than it holds between takes, and the oldest wait in
// the log file only. Add it with core::add_log_sink, and remove it before it goes.
class LogFeed final : public core::LogSink {
public:
    explicit LogFeed(core::Allocator& allocator);
    void write(const core::LogMessage& message) override;
    void take(Console& console);

private:
    std::mutex mutex_;
    core::Array<Line> pending_; // kLogFeedLines, a ring
    core::Array<Line> taken_;   // take()'s copy, said outside the lock
    size_t start_ = 0;
    size_t count_ = 0;
    size_t dropped_ = 0;
};

} // namespace de::console
