#pragma once

#include <cstddef>
#include <cstdint>

namespace de::core {

// The system an allocation belongs to (docs/design.md, Core library: every allocation is tagged).
// Each tag has its own heap allocator and its own row in the memory viewer.
enum class MemoryTag : uint8_t {
    General,
    Core,
    Platform,
    Render,
    Streaming,
    World,
    Physics,
    Animation,
    Navigation,
    Audio,
    Combat,
    AI,
    Script,
    UI,
    Editor,
    Tools,
    Count,
};

inline constexpr size_t kMemoryTagCount = static_cast<size_t>(MemoryTag::Count);

constexpr const char* memory_tag_name(MemoryTag tag) {
    switch (tag) {
    case MemoryTag::General:
        return "general";
    case MemoryTag::Core:
        return "core";
    case MemoryTag::Platform:
        return "platform";
    case MemoryTag::Render:
        return "render";
    case MemoryTag::Streaming:
        return "streaming";
    case MemoryTag::World:
        return "world";
    case MemoryTag::Physics:
        return "physics";
    case MemoryTag::Animation:
        return "animation";
    case MemoryTag::Navigation:
        return "navigation";
    case MemoryTag::Audio:
        return "audio";
    case MemoryTag::Combat:
        return "combat";
    case MemoryTag::AI:
        return "ai";
    case MemoryTag::Script:
        return "script";
    case MemoryTag::UI:
        return "ui";
    case MemoryTag::Editor:
        return "editor";
    case MemoryTag::Tools:
        return "tools";
    case MemoryTag::Count:
        break;
    }
    return "unknown";
}

} // namespace de::core
