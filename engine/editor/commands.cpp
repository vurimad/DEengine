#include "editor/commands.h"

#include "core/hash.h"
#include "core/profile.h"
#include "world/cell.h"
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
    // Another set in the same run (the same drag): the entity's command in it takes the new after; its before stays.
    if (merge_key != 0 && merge_key == open_merge_key_ && done_ == commands_.size()) {
        for (size_t i = done_; i > 0; --i) {
            Command& earlier = commands_[i - 1];
            if (earlier.group != run_group_ || earlier.merge_key != merge_key) {
                break; // past the run
            }
            if (earlier.entity == entity.id() && earlier.component == component) {
                earlier.after.assign(value);
                apply_component(entity, component, value);
                ++changes_;
                return true;
            }
        }
    }
    if (merge_key != 0 && merge_key != open_merge_key_) {
        run_group_ = ++next_group_; // a new run: a group of its own
    }
    Command command{.kind = Kind::Set,
                    .entity = entity.id(),
                    .component = component,
                    .merge_key = merge_key,
                    .before = core::Array<std::byte>(*allocator_),
                    .after = core::Array<std::byte>(*allocator_),
                    .record = core::Array<EntityRecord>(*allocator_)};
    command.before.assign(current); // before apply_component: it may move the component's storage
    command.after.assign(value);
    if (merge_key != 0) {
        command.group = run_group_;
    }
    apply_component(entity, component, value);
    push(std::move(command));
    open_merge_key_ = merge_key;
    return true;
}

void CommandStack::push(Command&& command) {
    // A new command: whatever was undone can't be redone any more.
    while (commands_.size() > done_) {
        commands_.pop();
    }
    if (commands_.size() == kMaxCommands) {
        commands_.remove_ordered(0);
        --done_;
    }
    if (open_group_ != 0) {
        command.group = open_group_;
    }
    commands_.push(std::move(command));
    done_ = commands_.size();
    open_merge_key_ = 0;
    ++changes_;
}

void CommandStack::created(flecs::entity entity) {
    if (maker_ == nullptr || !entity.is_alive()) {
        return;
    }
    Command command{.kind = Kind::Create,
                    .entity = entity.id(),
                    .before = core::Array<std::byte>(*allocator_),
                    .after = core::Array<std::byte>(*allocator_),
                    .record = core::Array<EntityRecord>(*allocator_)};
    maker_->record(entity, command.record.push(EntityRecord{}));
    command.record[0].was = entity.id();
    push(std::move(command));
}

bool CommandStack::remove(flecs::entity entity) {
    if (maker_ == nullptr || !entity.is_alive()) {
        return false;
    }
    Command command{.kind = Kind::Remove,
                    .entity = entity.id(),
                    .before = core::Array<std::byte>(*allocator_),
                    .after = core::Array<std::byte>(*allocator_),
                    .record = core::Array<EntityRecord>(*allocator_)};
    take_away(command, entity.world());
    push(std::move(command));
    return true;
}

void CommandStack::take_away(Command& command, const flecs::world& ecs) {
    if (!ecs.is_alive(command.entity)) {
        return;
    }
    // The entity, then what's attached to it (flecs::Parent), each after what it hangs on: as they are now, since later
    // commands may have changed them.
    command.record.clear();
    const flecs::entity root = ecs.entity(command.entity);
    maker_->record(root, command.record.push(EntityRecord{}));
    command.record[0].was = root.id();
    for (size_t at = 0; at < command.record.size(); ++at) {
        const flecs::entity_t holder = command.record[at].was;
        ecs.each([&](flecs::entity attached, const flecs::Parent& parent) {
            if (parent.value != holder) {
                return;
            }
            EntityRecord& record = command.record.push(EntityRecord{});
            maker_->record(attached, record);
            record.was = attached.id();
            record.attached_to = static_cast<uint32_t>(at);
        });
    }
    // Away, the ones furthest down first.
    for (size_t i = command.record.size(); i-- > 0;) {
        if (ecs.is_alive(command.record[i].was)) {
            maker_->unmake(ecs.entity(command.record[i].was));
        }
    }
}

void CommandStack::make_again(Command& command) {
    if (command.record.empty()) {
        return;
    }
    // Each made where it stood, then put on what it hangs on (made before it), where it is.
    core::Array<flecs::entity_t> made(*allocator_);
    for (const EntityRecord& record : command.record) {
        const flecs::entity entity = maker_->make(record);
        if (record.attached_to < made.size()) {
            world::attach(entity, entity.world().entity(made[record.attached_to]));
        }
        made.push(entity.id());
    }
    // Every command naming one names the new one (this one, by its first).
    for (size_t i = 0; i < command.record.size(); ++i) {
        const flecs::entity_t was = i == 0 ? command.entity : command.record[i].was;
        remap(was, made[i]);
        command.record[i].was = made[i];
    }
    made_.push(made[0]);
}

void CommandStack::each_taken_member(const flecs::world& ecs,
                                     core::FunctionRef<void(const EntityRecord& record)> visit) const {
    for (size_t i = 0; i < commands_.size(); ++i) {
        const Command& command = commands_[i];
        if (command.kind == Kind::Set || command.record.empty()) {
            continue;
        }
        // Taken away now: a Remove that's done, or a Create that's undone; with what was attached to it.
        const bool away = command.kind == Kind::Remove ? i < done_ : i >= done_;
        if (!away) {
            continue;
        }
        for (const EntityRecord& record : command.record) {
            if (record.cell == 0 || !ecs.is_alive(record.cell) || ecs.is_alive(record.was)) {
                continue;
            }
            const auto* contents = ecs.entity(record.cell).try_get<world::CellContents>();
            if (contents != nullptr && contents->revision == record.revision) {
                visit(record);
            }
        }
    }
}

const char* CommandStack::name_of(const Command& command, const flecs::world& ecs) const {
    switch (command.kind) {
    case Kind::Create:
        return "Create";
    case Kind::Remove:
        return "Delete";
    case Kind::Set:
        break;
    }
    return ecs_get_name(ecs.c_ptr(), command.component);
}

bool CommandStack::undo_one(const flecs::world& ecs) {
    Command& command = commands_[done_ - 1];
    if (command.kind != Kind::Set) {
        if (maker_ == nullptr) {
            return false;
        }
        if (command.kind == Kind::Create) {
            take_away(command, ecs);
        } else {
            make_again(command);
        }
        --done_;
        ++changes_;
        return true;
    }
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

bool CommandStack::redo_one(const flecs::world& ecs) {
    Command& command = commands_[done_];
    if (command.kind != Kind::Set) {
        if (maker_ == nullptr) {
            return false;
        }
        if (command.kind == Kind::Create) {
            make_again(command);
        } else {
            take_away(command, ecs);
        }
        ++done_;
        ++changes_;
        return true;
    }
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

bool CommandStack::undo(const flecs::world& ecs) {
    seal();
    made_.clear();
    if (!can_undo()) {
        return false;
    }
    const uint64_t group = commands_[done_ - 1].group;
    bool any = undo_one(ecs);
    while (group != 0 && can_undo() && commands_[done_ - 1].group == group) {
        any = undo_one(ecs) || any;
    }
    return any;
}

bool CommandStack::redo(const flecs::world& ecs) {
    seal();
    made_.clear();
    if (!can_redo()) {
        return false;
    }
    const uint64_t group = commands_[done_].group;
    bool any = redo_one(ecs);
    while (group != 0 && can_redo() && commands_[done_].group == group) {
        any = redo_one(ecs) || any;
    }
    return any;
}

const char* CommandStack::undo_name(const flecs::world& ecs) const {
    return can_undo() ? name_of(commands_[done_ - 1], ecs) : nullptr;
}

const char* CommandStack::redo_name(const flecs::world& ecs) const {
    return can_redo() ? name_of(commands_[done_], ecs) : nullptr;
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
    // What a command took away with its entity names them too.
    for (Command& command : commands_) {
        for (EntityRecord& record : command.record) {
            if (record.was == from) {
                record.was = to;
            }
        }
    }
    for (Command& command : commands_) {
        if (command.entity != from) {
            continue;
        }
        command.entity = to;
        if (command.kind != Kind::Set) {
            continue;
        }
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
