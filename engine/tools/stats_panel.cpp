#include "tools/stats_panel.h"

#include <imgui.h>

#include <algorithm>

namespace de::tools {

namespace {

double mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // namespace

void StatsPanel::record(const FrameStats& stats) {
    frame_ms_[next_] = stats.frame_ms;
    next_ = (next_ + 1) % kHistory;
    count_ = std::min(count_ + 1, kHistory);
    frames_with_allocations_ += stats.heap_allocations > 0 ? 1 : 0;
    last_ = stats;
}

void StatsPanel::draw(bool* open) const {
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Frame stats", open)) {
        ImGui::End();
        return;
    }

    // Frame time over the history: average, worst, and the graph (oldest on the left).
    float total = 0.0f;
    float worst = 0.0f;
    for (size_t i = 0; i < count_; ++i) {
        total += frame_ms_[i];
        worst = std::max(worst, frame_ms_[i]);
    }
    const float average = count_ > 0 ? total / static_cast<float>(count_) : 0.0f;
    ImGui::Text("%.0f fps  %.2f ms  (worst %.2f ms over %zu frames)", average > 0.0f ? 1000.0f / average : 0.0f,
                average, worst, count_);
    ImGui::PlotLines("##frame ms", frame_ms_, static_cast<int>(kHistory), static_cast<int>(next_), nullptr, 0.0f,
                     std::max(worst * 1.2f, 1.0f / 0.12f), ImVec2(-1.0f, 60.0f));
    ImGui::TextColored(last_.heap_allocations == 0 ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f) : ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                       "heap allocations: %llu this frame (%llu frames with any)",
                       static_cast<unsigned long long>(last_.heap_allocations),
                       static_cast<unsigned long long>(frames_with_allocations_));

    if (ImGui::CollapsingHeader("GPU memory", ImGuiTreeNodeFlags_DefaultOpen)) {
        const rhi::GpuMemoryStats& memory = last_.memory;
        ImGui::Text("%.1f MiB in resources, %.1f MiB waiting to be freed", mib(memory.total_bytes()),
                    mib(memory.pending_destroy_bytes));
        ImGui::Text("GPU-local %.0f of %.0f MiB (%s)", mib(memory.device_local_used_bytes),
                    mib(memory.device_local_budget_bytes), memory.budget_from_os ? "OS budget" : "estimated");
        if (ImGui::BeginTable("categories", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (size_t i = 0; i < static_cast<size_t>(rhi::GpuMemoryCategory::Count); ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rhi::gpu_memory_category_name(static_cast<rhi::GpuMemoryCategory>(i)));
                ImGui::TableNextColumn();
                ImGui::Text("%.1f MiB", mib(memory.bytes[i]));
                ImGui::TableNextColumn();
                ImGui::Text("%u", memory.counts[i]);
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Render graph", ImGuiTreeNodeFlags_DefaultOpen)) {
        const render::RenderGraphStats& graph = last_.graph;
        ImGui::Text("%u passes (%u culled, %u skipped), %u barriers", graph.passes, graph.culled_passes,
                    graph.skipped_passes, graph.barriers);
        ImGui::Text("pool: %u textures, %u buffers (%u made in total)", graph.pooled_textures, graph.pooled_buffers,
                    graph.pool_creations);
    }

    if (ImGui::CollapsingHeader("Uploads")) {
        const rhi::UploaderStats& uploads = last_.uploads;
        ImGui::Text("%s queue; %u queued (%.2f MiB)", uploads.transfer_queue ? "transfer" : "graphics",
                    uploads.queued_uploads, mib(uploads.queued_bytes));
        ImGui::Text("staging %.2f of %.0f MiB in use", mib(uploads.staging_bytes_in_use), mib(uploads.staging_bytes));
        ImGui::Text("%.1f MiB uploaded; %u with own staging, %u dropped", mib(uploads.uploaded_bytes),
                    uploads.dedicated_staging, uploads.dropped_uploads);
    }

    if (ImGui::CollapsingHeader("Device")) {
        const rhi::DeviceStats& device = last_.device;
        ImGui::Text("frame %llu", static_cast<unsigned long long>(device.frame_index));
        ImGui::Text("%u textures, %u buffers, %u pipelines live; %u destroys pending", device.live_textures,
                    device.live_buffers, device.live_pipelines, device.pending_destroys);
        ImGui::Text("bindless: %u sampled, %u storage entries", device.bindless_textures,
                    device.bindless_storage_textures);
        ImGui::TextColored(device.validation_errors == 0 ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f)
                                                         : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "validation: %u errors, %u warnings", device.validation_errors, device.validation_warnings);
        ImGui::Text("swapchain recreated %u times", device.swapchain_recreations);
    }
    ImGui::End();
}

} // namespace de::tools
