#include "console/console.h"

#include "core/assert.h"
#include "core/log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <system_error>

namespace de::console {

namespace {

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool starts_ignoring_case(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && same_ignoring_case(text.substr(0, prefix.size()), prefix);
}

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    for (size_t at = 0; at + part.size() <= text.size(); ++at) {
        if (same_ignoring_case(text.substr(at, part.size()), part)) {
            return true;
        }
    }
    return false;
}

bool name_before(std::string_view a, std::string_view b) {
    const size_t length = std::min(a.size(), b.size());
    for (size_t i = 0; i < length; ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return lower(a[i]) < lower(b[i]);
        }
    }
    return a.size() < b.size();
}

bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The next statement of a line: up to a ';' outside quotes.
std::string_view next_statement(std::string_view& text) {
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') {
            quoted = !quoted;
        } else if (text[i] == ';' && !quoted) {
            const std::string_view statement = text.substr(0, i);
            text = text.substr(i + 1);
            return statement;
        }
    }
    const std::string_view statement = text;
    text = {};
    return statement;
}

// A statement's words, quotes grouping them; false if it has more than words can hold, or a quote is left open.
bool split_words(std::string_view statement, std::span<std::string_view> words, size_t& count) {
    count = 0;
    size_t at = 0;
    while (at < statement.size()) {
        while (at < statement.size() && is_blank(statement[at])) {
            ++at;
        }
        if (at == statement.size()) {
            break;
        }
        if (count == words.size()) {
            return false;
        }
        if (statement[at] == '"') {
            const size_t close = statement.find('"', at + 1);
            if (close == std::string_view::npos) {
                return false;
            }
            words[count++] = statement.substr(at + 1, close - at - 1);
            at = close + 1;
        } else {
            const size_t start = at;
            while (at < statement.size() && !is_blank(statement[at])) {
                ++at;
            }
            words[count++] = statement.substr(start, at - start);
        }
    }
    return true;
}

} // namespace

void Output::say(Severity severity, std::string_view text) {
    // A line a line: what has several is split.
    while (true) {
        const size_t newline = text.find('\n');
        Line& line = lines_.emplace();
        line.severity = severity;
        line.text.assign(text.substr(0, std::min({newline, text.size(), kLineLength})));
        if (newline == std::string_view::npos) {
            return;
        }
        text = text.substr(newline + 1);
    }
}

bool Args::parse_bool(std::string_view word, bool& value) {
    constexpr std::string_view kTrue[] = {"1", "on", "true", "yes"};
    constexpr std::string_view kFalse[] = {"0", "off", "false", "no"};
    for (const std::string_view yes : kTrue) {
        if (same_ignoring_case(word, yes)) {
            value = true;
            return true;
        }
    }
    for (const std::string_view no : kFalse) {
        if (same_ignoring_case(word, no)) {
            value = false;
            return true;
        }
    }
    return false;
}

bool Args::parse_double(std::string_view word, double& value) {
    char text[64];
    if (word.empty() || word.size() >= sizeof(text)) {
        return false;
    }
    std::ranges::copy(word, text);
    text[word.size()] = '\0';
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end != text + word.size() || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool Args::parse_float(std::string_view word, float& value) {
    double parsed = 0.0;
    if (!parse_double(word, parsed)) {
        return false;
    }
    value = static_cast<float>(parsed);
    return true;
}

bool Args::parse_int(std::string_view word, int32_t& value) {
    int32_t parsed = 0;
    const std::from_chars_result result = std::from_chars(word.data(), word.data() + word.size(), parsed);
    if (word.empty() || result.ec != std::errc() || result.ptr != word.data() + word.size()) {
        return false;
    }
    value = parsed;
    return true;
}

Console::Console(core::Allocator& allocator)
    : entries_(allocator), scratch_(allocator), printed_(allocator), log_(allocator) {
    log_.resize(kLogLines);
    add_command("help", "[name]", "List every command and variable, or explain one.", &help, this);
}

Console::Item& Console::add(std::string_view name, Kind kind, void* user) {
    DE_ASSERT_MSG(!name.empty() && name.size() <= kNameLength, "a console name of 1 to 63 characters");
    // Kept in order: listing and completing need no sort.
    size_t at = 0;
    while (at < entries_.size() && name_before(entries_[at].entry.name.view(), name)) {
        ++at;
    }
    if (at < entries_.size() && same_ignoring_case(entries_[at].entry.name.view(), name)) {
        DE_LOG_WARN("console", "'%.*s' added again: the new one replaces the old", static_cast<int>(name.size()),
                    name.data());
    } else {
        entries_.push(Item{});
        for (size_t i = entries_.size() - 1; i > at; --i) {
            entries_[i] = entries_[i - 1];
        }
    }
    Item& item = entries_[at];
    item = Item{};
    item.entry.name.assign(name);
    item.kind = kind;
    item.user = user;
    return item;
}

void Console::add_command(std::string_view name, std::string_view usage, std::string_view help, Handler handler,
                          void* user) {
    Item& item = add(name, Kind::Command, user);
    item.entry.usage.assign(usage.substr(0, kTextLength));
    item.entry.help.assign(help.substr(0, kTextLength));
    item.handler = handler;
}

void Console::add_variable(std::string_view name, std::string_view help, bool* value, void* user) {
    Item& item = add(name, Kind::Bool, user);
    item.entry.usage.assign("[on | off]");
    item.entry.help.assign(help.substr(0, kTextLength));
    item.value = value;
}

void Console::add_variable(std::string_view name, std::string_view help, int32_t* value, int32_t min, int32_t max,
                           void* user) {
    Item& item = add(name, Kind::Int, user);
    char usage[64];
    static_cast<void>(std::snprintf(usage, sizeof(usage), "[%d to %d]", min, max));
    item.entry.usage.assign(usage);
    item.entry.help.assign(help.substr(0, kTextLength));
    item.value = value;
    item.min = static_cast<float>(min);
    item.max = static_cast<float>(max);
}

void Console::add_variable(std::string_view name, std::string_view help, float* value, float min, float max,
                           void* user) {
    Item& item = add(name, Kind::Float, user);
    char usage[64];
    static_cast<void>(
        std::snprintf(usage, sizeof(usage), "[%g to %g]", static_cast<double>(min), static_cast<double>(max)));
    item.entry.usage.assign(usage);
    item.entry.help.assign(help.substr(0, kTextLength));
    item.value = value;
    item.min = min;
    item.max = max;
}

void Console::remove(const void* user) {
    for (size_t i = entries_.size(); i-- > 0;) {
        if (entries_[i].user == user && user != nullptr) {
            entries_.remove_ordered(i);
        }
    }
}

const Console::Item* Console::find(std::string_view name) const {
    for (const Item& item : entries_) {
        if (same_ignoring_case(item.entry.name.view(), name)) {
            return &item;
        }
    }
    return nullptr;
}

bool Console::execute(std::string_view text, core::Array<Line>* said) {
    scratch_.clear();
    Output out(scratch_);
    std::string_view rest = text;
    while (!rest.empty()) {
        std::string_view statement = next_statement(rest);
        while (!statement.empty() && is_blank(statement.front())) {
            statement.remove_prefix(1);
        }
        while (!statement.empty() && is_blank(statement.back())) {
            statement.remove_suffix(1);
        }
        std::string_view words[kMaxWords];
        size_t count = 0;
        if (!split_words(statement, words, count)) {
            out.say(Severity::Echo, statement);
            out.error("Too many words, or a quote left open (at most %zu words)", kMaxWords);
            continue;
        }
        if (count == 0) {
            continue;
        }
        out.say(Severity::Echo, statement);
        run(std::span<const std::string_view>(words, count), out);
    }
    keep(scratch_.span());
    // What ran, from wherever (the window, --exec, an agent), and what failed, in the engine's log.
    for (const Line& line : scratch_) {
        if (line.severity == Severity::Echo) {
            DE_LOG_INFO("console", "> %s", line.text.c_str());
        } else if (line.severity == Severity::Error) {
            DE_LOG_WARN("console", "%s", line.text.c_str());
        }
    }
    if (said != nullptr) {
        for (const Line& line : scratch_) {
            said->push(line);
        }
    }
    return !out.failed();
}

void Console::run(std::span<const std::string_view> words, Output& out) {
    const std::string_view name = words[0];
    const Item* item = find(name);
    if (item == nullptr) {
        Entry close_names[4];
        size_t count = complete(name.substr(0, std::min<size_t>(name.size(), 3)), close_names);
        if (count == 0) {
            count = complete(name.substr(0, 1), std::span<Entry>(close_names, 2));
        }
        if (count == 0) {
            out.error("Unknown command '%.*s' (help lists what there is)", static_cast<int>(name.size()), name.data());
            return;
        }
        char list[kLineLength + 1] = {};
        size_t length = 0;
        for (size_t i = 0; i < count; ++i) {
            const int written = std::snprintf(list + length, sizeof(list) - length, "%s%s", i == 0 ? "" : ", ",
                                              close_names[i].name.c_str());
            length = std::min(length + static_cast<size_t>(std::max(written, 0)), sizeof(list) - 1);
        }
        out.error("Unknown command '%.*s'. Did you mean: %s?", static_cast<int>(name.size()), name.data(), list);
        return;
    }
    const Args args(words.subspan(1));
    const char* label = item->entry.name.c_str();
    switch (item->kind) {
    case Kind::Command:
        item->handler(item->user, args, out);
        return;
    case Kind::Bool: {
        auto& value = *static_cast<bool*>(item->value);
        if (!args.empty() && !Args::parse_bool(args[0], value)) {
            out.error("%s takes on or off (1, 0, true, false), not '%.*s'", label, static_cast<int>(args[0].size()),
                      args[0].data());
            return;
        }
        out.info("%s = %s", label, value ? "on" : "off");
        return;
    }
    case Kind::Int: {
        auto& value = *static_cast<int32_t*>(item->value);
        int32_t parsed = 0;
        if (!args.empty()) {
            if (!Args::parse_int(args[0], parsed) || static_cast<float>(parsed) < item->min ||
                static_cast<float>(parsed) > item->max) {
                out.error("%s takes a whole number from %d to %d, not '%.*s'", label, static_cast<int>(item->min),
                          static_cast<int>(item->max), static_cast<int>(args[0].size()), args[0].data());
                return;
            }
            value = parsed;
        }
        out.info("%s = %d", label, value);
        return;
    }
    case Kind::Float: {
        auto& value = *static_cast<float*>(item->value);
        float parsed = 0.0f;
        if (!args.empty()) {
            if (!Args::parse_float(args[0], parsed) || parsed < item->min || parsed > item->max) {
                out.error("%s takes a number from %g to %g, not '%.*s'", label, static_cast<double>(item->min),
                          static_cast<double>(item->max), static_cast<int>(args[0].size()), args[0].data());
                return;
            }
            value = parsed;
        }
        out.info("%s = %g", label, static_cast<double>(value));
        return;
    }
    }
}

void Console::help(void* self, const Args& args, Output& out) {
    const auto& console = *static_cast<const Console*>(self);
    if (!args.empty()) {
        const Item* item = console.find(args[0]);
        if (item == nullptr) {
            out.error("No command or variable '%.*s'", static_cast<int>(args[0].size()), args[0].data());
            return;
        }
        out.info("%s %s", item->entry.name.c_str(), item->entry.usage.c_str());
        out.info("  %s", item->entry.help.c_str());
        return;
    }
    for (const Item& item : console.entries_) {
        out.info("%-20s %-18s %s", item.entry.name.c_str(), item.entry.usage.c_str(), item.entry.help.c_str());
    }
}

size_t Console::complete(std::string_view prefix, std::span<Entry> out) const {
    size_t count = 0;
    for (const Item& item : entries_) {
        if (count < out.size() && starts_ignoring_case(item.entry.name.view(), prefix)) {
            out[count++] = item.entry;
        }
    }
    for (const Item& item : entries_) {
        if (count < out.size() && !starts_ignoring_case(item.entry.name.view(), prefix) &&
            contains_ignoring_case(item.entry.name.view(), prefix)) {
            out[count++] = item.entry;
        }
    }
    return count;
}

void Console::keep(std::span<const Line> lines) {
    for (const Line& line : lines) {
        log_[(log_start_ + log_count_) % kLogLines] = line;
        if (log_count_ < kLogLines) {
            ++log_count_;
        } else {
            log_start_ = (log_start_ + 1) % kLogLines;
        }
    }
    if (!lines.empty()) {
        ++log_revision_;
    }
}

void Console::clear_log() {
    log_start_ = 0;
    log_count_ = 0;
    ++log_revision_;
}

const Line& Console::log_line(size_t index) const {
    DE_ASSERT(index < log_count_);
    return log_[(log_start_ + index) % kLogLines];
}

void Console::print(Severity severity, std::string_view text) {
    printed_.clear(); // its room stays: the log feed's lines don't allocate once it has grown
    Output out(printed_);
    out.say(severity, text);
    keep(printed_.span());
}

LogFeed::LogFeed(core::Allocator& allocator) : pending_(allocator), taken_(allocator) {
    pending_.resize(kLogFeedLines);
    taken_.reserve(kLogFeedLines);
}

void LogFeed::write(const core::LogMessage& message) {
    if (std::string_view(message.channel) == "console") {
        return; // the console has these already, as it said them
    }
    Line line;
    line.severity = message.level >= core::LogLevel::Error     ? Severity::Error
                    : message.level == core::LogLevel::Warning ? Severity::Warning
                                                               : Severity::Info;
    char text[kLineLength + 1];
    static_cast<void>(std::snprintf(text, sizeof(text), "%s: %s", message.channel, message.text));
    line.text.assign(text);
    const std::scoped_lock lock(mutex_);
    if (count_ == kLogFeedLines) {
        start_ = (start_ + 1) % kLogFeedLines;
        --count_;
        ++dropped_;
    }
    pending_[(start_ + count_) % kLogFeedLines] = line;
    ++count_;
}

void LogFeed::take(Console& console) {
    // Copied out under the lock, said outside it: printing allocates, and a log line from in there would deadlock.
    taken_.clear();
    size_t dropped = 0;
    {
        const std::scoped_lock lock(mutex_);
        for (size_t i = 0; i < count_; ++i) {
            taken_.push(pending_[(start_ + i) % kLogFeedLines]); // reserved: no allocation under the lock
        }
        start_ = 0;
        count_ = 0;
        dropped = dropped_;
        dropped_ = 0;
    }
    if (dropped > 0) {
        char text[64];
        static_cast<void>(std::snprintf(text, sizeof(text), "(%zu log lines skipped: the log file has them)", dropped));
        console.print(Severity::Warning, text);
    }
    for (const Line& line : taken_) {
        console.print(line.severity, line.text.view());
    }
}

} // namespace de::console
