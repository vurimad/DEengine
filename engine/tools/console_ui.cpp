#include "tools/console_ui.h"

#include "core/log.h"
#include "core/profile.h"
#include "core/text.h"
#include "platform/vfs.h"

#include <imgui.h>
#include <imgui_internal.h> // keeping the console over the editor's panels

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace de::tools {

namespace {

constexpr const char* kHistoryPath = "user:/console.history";
constexpr size_t kMostSuggestions = 8;
constexpr ImVec4 kBackground(0.055f, 0.06f, 0.07f, 0.92f);
constexpr ImVec4 kEchoInk(0.58f, 0.64f, 0.72f, 1.0f);
constexpr ImVec4 kErrorInk(1.0f, 0.48f, 0.42f, 1.0f);
constexpr ImVec4 kWarningInk(1.0f, 0.8f, 0.36f, 1.0f);
constexpr ImVec4 kInfoInk(0.88f, 0.88f, 0.88f, 1.0f);
constexpr ImVec4 kDimInk(0.55f, 0.57f, 0.6f, 1.0f);
constexpr ImVec4 kUsageInk(0.52f, 0.68f, 0.92f, 1.0f);
constexpr ImVec4 kHelpInk(0.68f, 0.7f, 0.74f, 1.0f);
// The picked row's, bright on its highlight.
constexpr ImVec4 kPickedNameInk(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kPickedUsageInk(0.78f, 0.88f, 1.0f, 1.0f);
constexpr ImVec4 kPickedHelpInk(0.9f, 0.92f, 0.95f, 1.0f);
constexpr ImVec4 kLogGround(0.043f, 0.046f, 0.053f, 1.0f); // a log reads as the engine talking, not a panel
constexpr float kMostHelpWidth = 30.0f;                    // a suggestion's help, in font heights, before it's cut

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    for (size_t at = 0; at + part.size() <= text.size(); ++at) {
        size_t i = 0;
        while (i < part.size() && lower(text[at + i]) == lower(part[i])) {
            ++i;
        }
        if (i == part.size()) {
            return true;
        }
    }
    return false;
}

// The text, cut with "..." where it would pass width.
void text_cut_to(const char* text, float width, ImVec4 ink) {
    if (ImGui::CalcTextSize(text).x <= width) {
        ImGui::TextColored(ink, "%s", text);
        return;
    }
    const float dots = ImGui::CalcTextSize("...").x;
    size_t length = std::strlen(text);
    while (length > 0 && ImGui::CalcTextSize(text, text + length).x + dots > width) {
        --length;
    }
    ImGui::TextColored(ink, "%.*s...", static_cast<int>(length), text);
}

void draw_line(const console::Line& line) {
    if (line.severity == console::Severity::Echo) {
        ImGui::TextColored(kEchoInk, "> %s", line.text.c_str());
        return;
    }
    const ImVec4 ink = line.severity == console::Severity::Error     ? kErrorInk
                       : line.severity == console::Severity::Warning ? kWarningInk
                                                                     : kInfoInk;
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    ImGui::TextUnformatted(line.text.c_str());
    ImGui::PopStyleColor();
}

} // namespace

int step_suggestion(int picked, size_t count, bool up) {
    if (count == 0) {
        return -1;
    }
    const int rows = static_cast<int>(count);
    const int lit = std::clamp(picked, 0, rows - 1);
    return up ? (lit + 1) % rows : (lit + rows - 1) % rows;
}

ConsoleUi::ConsoleUi(console::Console& console)
    : console_(console), history_(core::heap_allocator(core::MemoryTag::Tools)),
      matching_(core::heap_allocator(core::MemoryTag::Tools)) {
    matching_.reserve(console::kLogLines);
}

void ConsoleUi::load_history(const platform::Vfs& vfs) {
    if (!vfs.exists(kHistoryPath)) {
        return; // the first run
    }
    auto file = vfs.read_whole_file(kHistoryPath, core::heap_allocator(core::MemoryTag::Tools));
    if (!file) {
        DE_LOG_WARN("console", "can't read %s: %s", kHistoryPath, file.error().message);
        return;
    }
    std::string_view text(reinterpret_cast<const char*>(file->data()), file->size());
    history_.clear();
    while (!text.empty()) {
        const std::string_view line = core::trim(core::next_line(text));
        if (!line.empty() && line.size() <= core::FixedString<255>::capacity()) {
            if (history_.size() == kConsoleHistory) {
                history_.remove_ordered(0);
            }
            history_.push(Typed{.text = core::FixedString<255>(line)});
        }
    }
}

void ConsoleUi::save_history(const platform::Vfs& vfs) const {
    if (history_.empty()) {
        return;
    }
    core::Array<char> text(core::heap_allocator(core::MemoryTag::Tools));
    for (const Typed& typed : history_) {
        for (const char c : typed.text.view()) {
            text.push(c);
        }
        text.push('\n');
    }
    if (const auto written = vfs.write_file(kHistoryPath, std::as_bytes(text.span())); !written) {
        DE_LOG_WARN("console", "can't keep what was typed in %s: %s", kHistoryPath, written.error().message);
    }
}

void ConsoleUi::set_open(bool open) {
    open_ = open;
    window_line_.take_focus = open;
    window_log_.at_end = true;
}

size_t ConsoleUi::suggestions(std::string_view typed, std::span<console::Entry> out) const {
    typed = core::trim(typed);
    if (typed.empty() || typed.find(' ') != std::string_view::npos) {
        return 0;
    }
    return console_.complete(typed, out);
}

int ConsoleUi::on_input(ImGuiInputTextCallbackData* data) {
    const auto& typing = *static_cast<const Typing*>(data->UserData);
    ConsoleUi& self = *typing.ui;
    InputLine& line = *typing.line;
    console::Entry list[kMostSuggestions];
    const size_t count = self.suggestions(std::string_view(data->Buf, static_cast<size_t>(data->BufTextLen)), list);
    const auto replace = [data](std::string_view text) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, text.data(), text.data() + text.size());
    };
    switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackCharFilter:
        // The key that opens and closes the console window types nothing.
        return data->EventChar == '`' || data->EventChar == '~' ? 1 : 0;
    case ImGuiInputTextFlags_CallbackCompletion:
        // Tab: the suggestion picked, or the first; a space after it, for its arguments.
        if (count > 0) {
            const size_t pick = line.suggestion >= 0 && std::cmp_less(line.suggestion, count)
                                    ? static_cast<size_t>(line.suggestion)
                                    : 0;
            replace(list[pick].name.view());
            data->InsertChars(data->CursorPos, " ");
            line.suggestion = -1;
        }
        return 0;
    case ImGuiInputTextFlags_CallbackHistory: {
        const bool up = data->EventKey == ImGuiKey_UpArrow;
        // While a name is being completed, the arrows pick among its suggestions (shown bottom-up, as the list is).
        if (count > 0) {
            line.suggestion = step_suggestion(line.suggestion, count, up);
            return 0;
        }
        // Otherwise they walk back through what was typed before.
        if (self.history_.empty()) {
            return 0;
        }
        const int newest = static_cast<int>(self.history_.size()) - 1;
        if (up) {
            line.history_at = line.history_at < 0 ? newest : std::max(line.history_at - 1, 0);
        } else if (line.history_at >= 0) {
            line.history_at = line.history_at >= newest ? -1 : line.history_at + 1;
        }
        replace(line.history_at >= 0 ? self.history_[static_cast<size_t>(line.history_at)].text.view()
                                     : std::string_view());
        return 0;
    }
    case ImGuiInputTextFlags_CallbackEdit:
        // Typing starts the suggestions and the history over.
        line.suggestion = -1;
        line.history_at = -1;
        line.shown = false;
        return 0;
    default:
        return 0;
    }
}

void ConsoleUi::run(InputLine& line) {
    const std::string_view typed = core::trim(std::string_view(line.text));
    if (!typed.empty()) {
        if (history_.empty() || history_.back().text.view() != typed) {
            if (history_.size() == kConsoleHistory) {
                history_.remove_ordered(0);
            }
            history_.push(Typed{.text = core::FixedString<255>(typed.substr(0, core::FixedString<255>::capacity()))});
        }
        static_cast<void>(console_.execute(typed));
    }
    line.text[0] = '\0';
    line.history_at = -1;
    line.suggestion = -1;
    line.shown = false;
    window_log_.at_end = true;
    panel_log_.at_end = true;
}

bool ConsoleUi::input(InputLine& line, const char* id, float width, const char* hint) {
    ImGui::SetNextItemWidth(width);
    constexpr ImGuiInputTextFlags kInput =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
        ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackEdit;
    Typing typing{.ui = this, .line = &line};
    // Typing in it, its ground a step lighter.
    ImVec4 ground = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ground = ImVec4(std::min(ground.x + 0.05f, 1.0f), std::min(ground.y + 0.05f, 1.0f),
                    std::min(ground.z + 0.06f, 1.0f), ground.w);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ground);
    const bool entered = ImGui::InputTextWithHint(id, hint, line.text, sizeof(line.text), kInput, &on_input, &typing);
    ImGui::PopStyleColor();
    const bool active = ImGui::IsItemActive();
    const ImVec2 corner = ImGui::GetItemRectMin();
    line.active = active;
    // Whether it has the keyboard, at a glance: an accent frame while it does, a faint one under the mouse.
    const ImVec2 far_corner = ImGui::GetItemRectMax();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (active) {
        const float frame = std::max(1.0f, std::floor(ImGui::GetFontSize() * 0.13f));
        ImGui::GetWindowDrawList()->AddRect(corner, far_corner, ImGui::GetColorU32(ImGuiCol_CheckMark), rounding,
                                            ImDrawFlags_None, frame);
    } else if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRect(corner, far_corner, ImGui::GetColorU32(ImGuiCol_Border), rounding);
    }
    // With something typed, Escape has ImGui put back what was there; with nothing, it means away.
    line.escaped = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && line.text[0] == '\0';
    if (entered) {
        run(line);
        line.take_focus = true; // on to the next command
    }
    if (line.take_focus) {
        ImGui::SetKeyboardFocusHere(-1);
        line.take_focus = false;
    }

    // While it's being typed in: the names the text could be, over it, the first nearest it; what they take and do
    // beside them.
    console::Entry list[kMostSuggestions];
    const size_t count = active || line.shown ? suggestions(line.text, list) : 0;
    if (count == 0) {
        return entered;
    }
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(corner.x, corner.y - std::floor(font * 0.3f)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetStyle().PopupRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::floor(font * 0.45f), std::floor(font * 0.45f)));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::floor(font * 0.55f), std::floor(font * 0.28f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(std::floor(font * 0.5f), std::floor(font * 0.3f)));
    constexpr ImGuiWindowFlags kList = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_AlwaysAutoResize;
    char window[64];
    static_cast<void>(std::snprintf(window, sizeof(window), "##suggestions %s", id));
    if (ImGui::Begin(window, nullptr, kList)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const size_t picked =
            line.suggestion >= 0 && std::cmp_less(line.suggestion, count) ? static_cast<size_t>(line.suggestion) : 0;
        // Bottom-up, so the first is nearest the line, where the eye is: the name, what it takes, what it does.
        if (ImGui::BeginTable("##names", 3, ImGuiTableFlags_SizingFixedFit)) {
            for (size_t row = count; row-- > 0;) {
                const console::Entry& entry = list[row];
                const bool lit = row == picked;
                ImGui::TableNextRow();
                if (lit) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(lit ? kPickedNameInk : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s",
                                   entry.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(lit ? kPickedUsageInk : kUsageInk, "%s", entry.usage.c_str());
                ImGui::TableNextColumn();
                text_cut_to(entry.help.c_str(), font * kMostHelpWidth, lit ? kPickedHelpInk : kHelpInk);
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::TextColored(kDimInk, "Tab completes  \xc2\xb7  Up and Down choose  \xc2\xb7  Enter runs");
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor();
    return entered;
}

void ConsoleUi::draw_log(LogView& view, float width, float height, std::string_view filter) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kLogGround);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.7f, ImGui::GetFontSize() * 0.45f));
    const bool shown =
        ImGui::BeginChild("##log", ImVec2(width, std::max(height, 0.0f)), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!shown) {
        ImGui::EndChild();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, std::floor(ImGui::GetFontSize() * 0.3f)));
    if (console_.log_size() == 0) {
        ImGui::TextColored(kDimInk, "help lists the commands. Tab completes a name, the arrows go back through what "
                                    "was typed.");
    }
    // The lines a filter keeps, or all of them; only those on screen are drawn.
    matching_.clear();
    const bool filtered = !filter.empty();
    if (filtered) {
        for (size_t i = 0; i < console_.log_size(); ++i) {
            if (contains_ignoring_case(console_.log_line(i).text.view(), filter)) {
                matching_.push(static_cast<uint16_t>(i));
            }
        }
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(filtered ? matching_.size() : console_.log_size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const size_t index = filtered ? matching_[static_cast<size_t>(i)] : static_cast<size_t>(i);
            draw_line(console_.log_line(index));
        }
    }
    // New lines scroll it on while it's at the end; scrolled back, it stays where it was.
    if (console_.log_revision() != view.seen_revision && view.at_end) {
        ImGui::SetScrollHereY(1.0f);
    }
    view.seen_revision = console_.log_revision();
    view.at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void ConsoleUi::draw_log_panel(float width, float height, std::string_view filter) {
    draw_log(panel_log_, width, height, filter);
}

void ConsoleUi::draw_command_line(float width, const char* hint) {
    static_cast<void>(input(panel_line_, "##command line", width, hint));
}

void ConsoleUi::set_command_line(std::string_view text) {
    const size_t length = std::min(text.size(), sizeof(panel_line_.text) - 1);
    std::memcpy(panel_line_.text, text.data(), length);
    panel_line_.text[length] = '\0';
    panel_line_.take_focus = true;
    panel_line_.shown = true;
}

void ConsoleUi::draw(float x, float y, float width, float height) {
    DE_PROFILE_ZONE("console ui");
    const ImGuiIO& io = ImGui::GetIO();
    // The key left of 1, unless another text field has the keyboard (its own text, then).
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false) && (open_ || !io.WantTextInput)) {
        set_open(!open_);
    }
    if (!open_) {
        return;
    }

    // Along the bottom of the picture: what was said, then the line being typed.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float input_height = ImGui::GetFrameHeight();
    const float panel_height = std::min(height * 0.45f, (ImGui::GetTextLineHeightWithSpacing() * 14.0f) + input_height +
                                                            (style.WindowPadding.y * 2.0f) + style.ItemSpacing.y);
    ImGui::SetNextWindowPos(ImVec2(x, y + height - panel_height));
    ImGui::SetNextWindowSize(ImVec2(width, panel_height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##console", nullptr, kPanel)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        draw_log(window_log_, 0.0f, ImGui::GetContentRegionAvail().y - input_height - style.ItemSpacing.y, {});
        static_cast<void>(input(window_line_, "##console line", -1.0f, "a command: help lists them"));
        if (window_line_.escaped) {
            open_ = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

} // namespace de::tools
