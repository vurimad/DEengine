#include "tools/stats_panel.h"

#include "core/assert.h"
#include "core/profile.h"
#include "core/text.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace de::tools {

namespace {

double mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// The overlay's inks: it has no background, so each is drawn over a dark edge.
constexpr ImU32 kTextInk = IM_COL32(236, 236, 236, 255);
constexpr ImU32 kHeadingInk = IM_COL32(170, 178, 190, 255);
constexpr ImU32 kGoodInk = IM_COL32(140, 230, 140, 255);
constexpr ImU32 kBadInk = IM_COL32(255, 160, 100, 255);
constexpr ImU32 kWarnInk = IM_COL32(255, 205, 80, 255);
constexpr ImU32 kErrorInk = IM_COL32(255, 105, 105, 255);
constexpr ImU32 kEdgeInk = IM_COL32(0, 0, 0, 190);

// Text that reads over any picture: a dark edge a pixel round it, then the text.
void edged_text(ImU32 color, const char* text) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    constexpr ImVec2 kAround[] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    for (const ImVec2 offset : kAround) {
        draw->AddText(ImVec2(at.x + offset.x, at.y + offset.y), kEdgeInk, text);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

template <typename... Args>
void line(ImU32 color, const char* format, Args... args) {
    char text[192];
    static_cast<void>(std::snprintf(text, sizeof(text), format, args...));
    edged_text(color, text);
}

void heading(const char* text) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.25f));
    edged_text(kHeadingInk, text);
}

// The frame times as a line (oldest on the left, from next round), over a dark edge, with no box behind it.
void frame_graph(std::span<const float> frame_ms, size_t next, float top_ms) {
    const ImVec2 size(ImGui::GetFontSize() * 24.0f, ImGui::GetFontSize() * 3.2f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImVec2 points[kFrameHistory];
    const size_t count = frame_ms.size();
    for (size_t i = 0; i < count; ++i) {
        const float ms = frame_ms[(next + i) % count];
        const float x = at.x + (size.x * static_cast<float>(i) / static_cast<float>(count - 1));
        const float y = at.y + size.y - (size.y * std::clamp(ms / top_ms, 0.0f, 1.0f));
        points[i] = ImVec2(x, y);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddPolyline(points, static_cast<int>(count), kEdgeInk, ImDrawFlags_None, 3.0f);
    draw->AddPolyline(points, static_cast<int>(count), kTextInk, ImDrawFlags_None, 1.25f);
}

} // namespace

bool parse_stats_sections(std::string_view text, uint32_t& sections) {
    static constexpr StatsSectionName kGroups[] = {{.name = "fps", .bits = kStatsFrame},
                                                   {.name = "default", .bits = kStatsDefault},
                                                   {.name = "all", .bits = kStatsAll}};
    const auto named = [](std::string_view word) -> uint32_t {
        const std::span<const StatsSectionName> lists[] = {kStatsSectionNames, kGroups};
        for (const std::span<const StatsSectionName> names : lists) {
            const auto found = std::ranges::find(names, word, &StatsSectionName::name);
            if (found != names.end()) {
                return found->bits;
            }
        }
        return 0;
    };
    uint32_t parsed = 0;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string_view word = core::trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
        if (word.empty()) {
            continue;
        }
        const uint32_t bits = named(word);
        if (bits == 0) {
            return false;
        }
        parsed |= bits;
    }
    sections = parsed;
    return true;
}

size_t late_frames(std::span<const float> frame_ms) {
    DE_ASSERT(frame_ms.size() <= kFrameHistory);
    const size_t count = frame_ms.size();
    if (count == 0) {
        return 0;
    }
    float sorted[kFrameHistory];
    std::copy_n(frame_ms.begin(), count, sorted);
    float* const lower_quartile = sorted + (count - 1) / 4;
    std::nth_element(sorted, lower_quartile, sorted + count);
    const float limit = *lower_quartile * 1.5f;
    size_t late = 0;
    for (const float ms : frame_ms) {
        late += ms > limit ? 1 : 0;
    }
    return late;
}

void StatsPanel::record(const FrameStats& stats) {
    DE_PROFILE_ZONE("stats panel record");
    frame_ms_[next_] = stats.frame_ms;
    next_ = (next_ + 1) % kFrameHistory;
    count_ = std::min(count_ + 1, kFrameHistory);
    frames_with_allocations_ += stats.heap_allocations > 0 ? 1 : 0;
    last_ = stats;
    // The times are the graph's until its next begin(): kept as a copy (one per frame).
    const size_t gpu_count = std::min(stats.gpu_passes.size(), std::size(gpu_passes_));
    std::copy_n(stats.gpu_passes.data(), gpu_count, gpu_passes_);
    last_.gpu_passes = std::span<const render::PassGpuTime>(gpu_passes_, gpu_count);
}

namespace {

// The overlay's spacing, the same in its window and in a panel's.
void push_overlay_style() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 1.0f));
}

constexpr int kOverlayStyleVars = 3;
// No background at all: the text is edged so it reads on bright sky as on dark ground; and it takes no clicks.
constexpr ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;

float overlay_margin() {
    return std::floor(ImGui::GetFontSize() * 0.6f);
}

} // namespace

void StatsPanel::draw_overlay(float x, float y, float width, float height) {
    DE_PROFILE_ZONE("stats panel");
    const float margin = overlay_margin();
    ImGui::SetNextWindowPos(ImVec2(x + margin, y + margin));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(std::max(width - (2.0f * margin), 64.0f),
                                                                   std::max(height - (2.0f * margin), 64.0f)));
    push_overlay_style();
    if (ImGui::Begin("##frame stats", nullptr, kOverlayFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
        draw_contents();
    }
    ImGui::End();
    ImGui::PopStyleVar(kOverlayStyleVars);
}

void StatsPanel::draw_in_window(float x, float y, float width, float height) {
    DE_PROFILE_ZONE("stats panel");
    // A child of the window being drawn: under whatever is over that window, clipped to it.
    const float margin = overlay_margin();
    ImGui::SetCursorScreenPos(ImVec2(x + margin, y + margin));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(std::max(width - (2.0f * margin), 64.0f),
                                                                   std::max(height - (2.0f * margin), 64.0f)));
    push_overlay_style();
    if (ImGui::BeginChild("##frame stats", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY, kOverlayFlags)) {
        draw_contents();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(kOverlayStyleVars);
}

void StatsPanel::draw_contents() {
    const auto showing = [this](uint32_t section) { return (sections_ & section) != 0; };

    // Frame time over the history: average, worst, and the graph (oldest on the left).
    float total = 0.0f;
    float worst = 0.0f;
    for (size_t i = 0; i < count_; ++i) {
        total += frame_ms_[i];
        worst = std::max(worst, frame_ms_[i]);
    }
    const float average = count_ > 0 ? total / static_cast<float>(count_) : 0.0f;
    if (showing(kStatsFrame)) {
        line(kTextInk, "%.0f fps  %.2f ms  (worst %.2f ms over %zu frames)", average > 0.0f ? 1000.0f / average : 0.0f,
             average, worst, count_);
        if (draws_until_late_ == 0) {
            late_ = late_frames(std::span<const float>(frame_ms_, count_));
            draws_until_late_ = 32;
        }
        --draws_until_late_;
        line(kTextInk, "%zu late (loop clock, over 1.5x the usual)", late_);
    }
    if (showing(kStatsGraph)) {
        frame_graph(std::span<const float>(frame_ms_, kFrameHistory), next_, std::max(worst * 1.2f, 1.0f / 0.12f));
    }
    if (showing(kStatsAllocations)) {
        line(last_.heap_allocations == 0 ? kGoodInk : kBadInk,
             "heap allocations: %llu this frame (%llu frames with any)",
             static_cast<unsigned long long>(last_.heap_allocations),
             static_cast<unsigned long long>(frames_with_allocations_));
    }
    if (showing(kStatsCamera) && last_.has_camera) {
        const Vec3 forward = last_.camera_rotation * Vec3(0.0f, 0.0f, -1.0f);
        line(kTextInk, "camera %.1f %.1f %.1f  yaw %.1f  pitch %.1f deg", last_.camera_position.x,
             last_.camera_position.y, last_.camera_position.z,
             static_cast<double>(glm::degrees(std::atan2(-forward.x, -forward.z))),
             static_cast<double>(glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)))));
        if (last_.has_exposure) {
            line(kTextInk, "exposure %+.1f EV  (going to %+.1f)", static_cast<double>(last_.exposure_ev),
                 static_cast<double>(last_.exposure_target_ev));
        }
    }
    if (showing(kStatsCull) && last_.cull_frozen) {
        line(kWarnInk, "cull frozen (F4): %u shadow draws, %u triangles", last_.visible_shadow_draws,
             last_.visible_shadow_triangles);
    }

    // Only where there are effects: nothing to say of a world without any.
    if (showing(kStatsParticles) && last_.particle_effects > 0) {
        heading("Particles");
        line(kTextInk, "%u effects, %u emitters near, %u in view", last_.particle_effects, last_.particle_emitters,
             last_.particle_emitters_drawn);
        line(kTextInk, "about %u alive, %u of %u blocks of the pool", last_.particles, last_.particle_blocks,
             last_.particle_pool_blocks);
        if (last_.particle_emitters_starved > 0) {
            line(kWarnInk, "%u emitters wait: the pool has no room for them", last_.particle_emitters_starved);
        }
    }

    // Only where there are lights.
    if (showing(kStatsLights) && last_.lights > 0) {
        heading("Lights");
        line(kTextInk, "%u local lights, %u lighting the view, %u with shadows (%u faces)", last_.lights,
             last_.lights_lit, last_.lights_shadowed, last_.shadow_faces);
        if (last_.lights_dropped > 0) {
            line(kWarnInk, "%u more reach the view than a frame lights with: the farthest are unlit",
                 last_.lights_dropped);
        }
    }

    // Only where rays are traced.
    if (showing(kStatsRays) && (last_.trace_meshes + last_.trace_waiting + last_.trace_instances) > 0) {
        heading("Rays");
        line(kTextInk, "%u meshes of %.1f thousand triangles, %u instance slots in the top level", last_.trace_meshes,
             static_cast<double>(last_.trace_triangles) / 1000.0, last_.trace_instances);
        if (last_.trace_waiting > 0) {
            line(kTextInk, "%u meshes wait for their bottom level", last_.trace_waiting);
        }
        if (last_.path_tracer_samples > 0) {
            line(last_.path_tracer_bounces == 0 ? kWarnInk : kTextInk,
                 "path traced: %u samples a pixel, %s; started again %u times, last for: %s", last_.path_tracer_samples,
                 last_.path_tracer_bounces == 0 ? "direct light only (0 bounces)" : "light bounces",
                 last_.path_tracer_restarts, last_.path_tracer_restart);
        }
        if (last_.light_boxes > 0) {
            line(kTextInk, "%u light boxes of %u voxels, %.0f%% baked", last_.light_boxes, last_.light_box_voxels,
                 static_cast<double>(last_.light_boxes_baked * 100.0f));
        }
        if (last_.compare_pixels > 0) {
            line(kTextInk,
                 "compared with path traced: %.2f stops off on average; %.0f%% off by over 10%%, %.0f%% "
                 "by over 25%%",
                 static_cast<double>(last_.compare_mean_stops), static_cast<double>(last_.compare_over_10 * 100.0f),
                 static_cast<double>(last_.compare_over_25 * 100.0f));
            line(kTextInk, "  %.0f%% too dark by 10%% (blue), %.0f%% too bright (red)",
                 static_cast<double>(last_.compare_darker_10 * 100.0f),
                 static_cast<double>(last_.compare_brighter_10 * 100.0f));
        }
    }

    if (showing(kStatsMemory)) {
        heading("GPU memory");
        const rhi::GpuMemoryStats& memory = last_.memory;
        line(kTextInk, "%.1f MiB in resources, %.1f MiB waiting to be freed", mib(memory.total_bytes()),
             mib(memory.pending_destroy_bytes));
        line(kTextInk, "GPU-local %.0f of %.0f MiB (%s)", mib(memory.device_local_used_bytes),
             mib(memory.device_local_budget_bytes), memory.budget_from_os ? "OS budget" : "estimated");
        // Columns as wide as their widest text, in whatever font the overlay has.
        float name_width = 0.0f;
        for (size_t i = 0; i < static_cast<size_t>(rhi::GpuMemoryCategory::Count); ++i) {
            name_width =
                std::max(name_width,
                         ImGui::CalcTextSize(rhi::gpu_memory_category_name(static_cast<rhi::GpuMemoryCategory>(i))).x);
        }
        const float gap = ImGui::GetFontSize();
        const float columns[3] = {0.0f, name_width + gap, name_width + gap + ImGui::CalcTextSize("0000.0 MiB").x + gap};
        for (size_t i = 0; i < static_cast<size_t>(rhi::GpuMemoryCategory::Count); ++i) {
            char size[32];
            char count[16];
            static_cast<void>(std::snprintf(size, sizeof(size), "%.1f MiB", mib(memory.bytes[i])));
            static_cast<void>(std::snprintf(count, sizeof(count), "%u", memory.counts[i]));
            const char* cells[3] = {rhi::gpu_memory_category_name(static_cast<rhi::GpuMemoryCategory>(i)), size, count};
            const float left = ImGui::GetCursorPosX();
            for (int c = 0; c < 3; ++c) {
                if (c > 0) {
                    ImGui::SameLine(left + columns[c]);
                }
                edged_text(kTextInk, cells[c]);
            }
        }
    }
    if (showing(kStatsGpu)) {
        heading("GPU time");
        if (last_.gpu_passes.empty()) {
            edged_text(kTextInk, "no GPU timestamps");
        } else {
            line(kTextInk, "%.2f ms, first pass to last", static_cast<double>(last_.gpu_frame_ms));
            for (const render::PassGpuTime& pass : last_.gpu_passes) {
                line(kTextInk, "%6.3f ms  %s", static_cast<double>(pass.ms), pass.name.c_str());
            }
        }
    }
    if (showing(kStatsRenderGraph)) {
        heading("Render graph");
        const render::RenderGraphStats& graph = last_.graph;
        line(kTextInk, "%u passes (%u culled, %u skipped), %u barriers", graph.passes, graph.culled_passes,
             graph.skipped_passes, graph.barriers);
        line(kTextInk, "pool: %u textures, %u buffers (%u made in total)", graph.pooled_textures, graph.pooled_buffers,
             graph.pool_creations);
    }
    if (showing(kStatsUploads)) {
        heading("Uploads");
        const rhi::UploaderStats& uploads = last_.uploads;
        line(kTextInk, "%s queue; %u queued (%.2f MiB)", uploads.transfer_queue ? "transfer" : "graphics",
             uploads.queued_uploads, mib(uploads.queued_bytes));
        line(kTextInk, "staging %.2f of %.0f MiB in use", mib(uploads.staging_bytes_in_use),
             mib(uploads.staging_bytes));
        line(kTextInk, "%.1f MiB uploaded; %u with own staging, %u dropped", mib(uploads.uploaded_bytes),
             uploads.dedicated_staging, uploads.dropped_uploads);
    }
    if (showing(kStatsDevice)) {
        heading("Device");
        const rhi::DeviceStats& device = last_.device;
        line(kTextInk, "frame %llu", static_cast<unsigned long long>(device.frame_index));
        line(kTextInk, "%u textures, %u buffers, %u pipelines live; %u destroys pending", device.live_textures,
             device.live_buffers, device.live_pipelines, device.pending_destroys);
        line(kTextInk, "bindless: %u sampled, %u storage entries", device.bindless_textures,
             device.bindless_storage_textures);
        line(device.validation_errors == 0 ? kGoodInk : kErrorInk, "validation: %u errors, %u warnings",
             device.validation_errors, device.validation_warnings);
        line(kTextInk, "swapchain recreated %u times", device.swapchain_recreations);
    }
}

} // namespace de::tools
