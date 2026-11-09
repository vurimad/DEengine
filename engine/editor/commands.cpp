#include "editor/commands.h"

#include "core/hash.h"
#include "core/profile.h"
#include "world/transform.h"

#include <cstring>
#include <utility>

namespace de::editor {

namespace {

// The component's bytes on the entity, or empty if it doesn't have it (or it's a tag).
std::span<const std::byte> component_bytes(flecs::entity entity, flecs::id_t component) {
    const flecs::world_t* world = entity.world().c_ptr();
    const ecs_type_info_t* info = ecs_get_type_info(world, component);
    const void* value = ecs_get_id(world, entity.id(), component);
    if (info == nullptr || value == nullptr || info->size <= 0) {
        return {};
    }
    return {static_cast<const std::byte*>(value), static_cast<size_t>(info->size)};
}

uint64_t saved_key(flecs::entity_t entity, flecs::id_t component) {
    return core::mix64(entity ^ core::mix64(component));
}

} // namespace

void apply_component(flecs::entity entity, flecs::id_t component, std::span<const std::byte> value) {
    const flecs::world ecs = entity.world();
    if (component == ecs.component<world::Transform>().id() && value.size() == sizeof(world::Transform)) {
        world::Transform transform;
        std::memcpy(&transform, value.data(), sizeof(transform));
        world::place(entity, transform);
        return;
    }
    ecs_set_id(ecs.c_ptr(), entity.id(), component, value.size(), value.data());
}

bool CommandStack::set_component(flecs::entity entity, flecs::id_t component, std::span<const std::byte> value,
                                 uint64_t merge_key) {
    DE_PROFILE_ZONE("CommandStack::set_component");
    if (!entity.is_alive()) {
        return false;
    }
    const std::span<const std::byte> current = component_bytes(entity, component);
    if (current.empty() || current.size() != value.size()) {
        return false;
    }
    // The first edit keeps what was there: the saved value.
    if (const uint64_t key = saved_key(entity.id(), component); !saved_.contains(key)) {
        core::Array<std::byte> saved(*allocator_);
        saved.assign(current);
        saved_.set(key, std::move(saved));
    }
    // Another set in the same run (the same drag): the command's after moves on; its before stays.
    if (merge_key != 0 && merge_key == open_merge_key_ && done_ == commands_.size() && done_ > 0) {
        Command& last = commands_[done_ - 1];
        if (last.entity == entity.id() && last.component == component && last.merge_key == merge_key) {
            last.after.assign(value);
            apply_component(entity, component, value);
            ++changes_;
            return true;
        }
    }
    // A new command: whatever was undone can't be redone any more.
    while (commands_.size() > done_) {
        commands_.pop();
    }
    if (commands_.size() == kMaxCommands) {
        commands_.remove_ordered(0);
        --done_;
    }
    Command command{.entity = entity.id(),
                    .component = component,
                    .merge_key = merge_key,
                    .before = core::Array<std::byte>(*allocator_),
                    .after = core::Array<std::byte>(*allocator_)};
    command.before.assign(current); // before apply_component: it may move the component's storage
    command.after.assign(value);
    apply_component(entity, component, value);
    commands_.push(std::move(command));
    done_ = commands_.size();
    open_merge_key_ = merge_key;
    ++changes_;
    return true;
}

bool CommandStack::undo(const flecs::world& ecs) {
    seal();
    if (!can_undo()) {
        return false;
    }
    const Command& command = commands_[done_ - 1];
    const flecs::entity entity = ecs.entity(command.entity);
    if (!ecs.is_alive(command.entity) || component_bytes(entity, command.component).size() != command.before.size()) {
        commands_.remove_ordered(--done_); // its entity went (a cell reloaded): nothing to put back
        return false;
    }
    apply_component(entity, command.component, command.before.span());
    --done_;
    ++changes_;
    return true;
}

bool CommandStack::redo(const flecs::world& ecs) {
    seal();
    if (!can_redo()) {
        return false;
    }
    const Command& command = commands_[done_];
    const flecs::entity entity = ecs.entity(command.entity);
    if (!ecs.is_alive(command.entity) || component_bytes(entity, command.component).size() != command.after.size()) {
        commands_.remove_ordered(done_);
        return false;
    }
    apply_component(entity, command.component, command.after.span());
    ++done_;
    ++changes_;
    return true;
}

const char* CommandStack::undo_name(const flecs::world& ecs) const {
    return can_undo() ? ecs_get_name(ecs.c_ptr(), commands_[done_ - 1].component) : nullptr;
}

const char* CommandStack::redo_name(const flecs::world& ecs) const {
    return can_redo() ? ecs_get_name(ecs.c_ptr(), commands_[done_].component) : nullptr;
}

const std::byte* CommandStack::saved_value(flecs::entity_t entity, flecs::id_t component) const {
    const core::Array<std::byte>* saved = saved_.find(saved_key(entity, component));
    return saved != nullptr ? saved->data() : nullptr;
}

void CommandStack::clear() {
    saved_.clear();
    commands_.clear();
    done_ = 0;
    open_merge_key_ = 0;
}

void CommandStack::remap(flecs::entity_t from, flecs::entity_t to) {
    for (Command& command : commands_) {
        if (command.entity != from) {
            continue;
        }
        command.entity = to;
        // Its saved value goes with it (each saved value has the command its first edit made).
        if (core::Array<std::byte>* saved = saved_.find(saved_key(from, command.component)); saved != nullptr) {
            core::Array<std::byte> moved = std::move(*saved);
            static_cast<void>(saved_.remove(saved_key(from, command.component)));
            saved_.set(saved_key(to, command.component), std::move(moved));
        }
    }
}

void CommandStack::entities(core::Array<flecs::entity_t>& out) const {
    out.clear();
    for (const Command& command : commands_) {
        out.push(command.entity);
    }
}

} // namespace de::editor
