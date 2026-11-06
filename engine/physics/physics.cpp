#include "physics/physics.h"

#include "core/allocator.h"
#include "core/assert.h"
#include "core/hash_map.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/profile.h"
#include "world/transform.h"
#include "world/world.h"

// Jolt.h has to come first: it sets up what the others need.
// clang-format off
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <new>
#include <span>

namespace de::physics {

namespace {

using core::MemoryTag;
using world::Transform;
using world::WorldTransform;

// ---- Jolt's memory, logs and asserts ------------------------------------------------------------------------------

void* allocate_hook(size_t size) {
    return core::tagged_malloc(MemoryTag::Physics, size);
}

void* reallocate_hook(void* block, [[maybe_unused]] size_t old_size, size_t new_size) {
    return core::tagged_realloc(MemoryTag::Physics, block, new_size);
}

void free_hook(void* block) {
    core::tagged_free(MemoryTag::Physics, block);
}

// Over-aligned, with where the block really starts kept just before what's handed out.
void* aligned_allocate_hook(size_t size, size_t alignment) {
    const size_t needed = size + alignment + sizeof(void*);
    auto* const block = static_cast<std::byte*>(core::tagged_malloc(MemoryTag::Physics, needed));
    auto* const aligned = block + sizeof(void*) + core::alignment_padding(block + sizeof(void*), alignment);
    reinterpret_cast<void**>(aligned)[-1] = block;
    return aligned;
}

void aligned_free_hook(void* block) {
    if (block != nullptr) {
        core::tagged_free(MemoryTag::Physics, static_cast<void**>(block)[-1]);
    }
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp): the shape Jolt's hook has
void trace_hook(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    DE_LOG_INFO("physics", "%s", line);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_hook(const char* expression, const char* message, const char* file, JPH::uint line) {
    DE_LOG_ERROR("physics", "Jolt: %s (%s), %s:%u", expression, message != nullptr ? message : "", file, line);
    return true; // stop here
}
#endif

// Jolt's global setup, while any Physics exists. Physics objects are made and destroyed on one thread.
int jolt_users = 0;
alignas(JPH::Factory) std::byte factory_storage[sizeof(JPH::Factory)];

void acquire_jolt() {
    if (jolt_users++ > 0) {
        return;
    }
    JPH::Allocate = allocate_hook;
    JPH::Reallocate = reallocate_hook;
    JPH::Free = free_hook;
    JPH::AlignedAllocate = aligned_allocate_hook;
    JPH::AlignedFree = aligned_free_hook;
    JPH::Trace = trace_hook;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = assert_hook;
#endif
    JPH::Factory::sInstance = ::new (static_cast<void*>(factory_storage)) JPH::Factory();
    JPH::RegisterTypes();
}

void release_jolt() {
    if (--jolt_users > 0) {
        return;
    }
    JPH::UnregisterTypes();
    JPH::Factory::sInstance->~Factory();
    JPH::Factory::sInstance = nullptr;
}

// ---- Jolt's jobs, on ours -------------------------------------------------------------------------------------------

class CoreJobSystem final : public JPH::JobSystemWithBarrier {
public:
    explicit CoreJobSystem(core::JobSystem& jobs) : JobSystemWithBarrier(JPH::cMaxPhysicsBarriers), jobs_(jobs) {
        free_jobs_.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
    }
    ~CoreJobSystem() override = default;
    CoreJobSystem(const CoreJobSystem&) = delete;
    CoreJobSystem& operator=(const CoreJobSystem&) = delete;
    CoreJobSystem(CoreJobSystem&&) = delete;
    CoreJobSystem& operator=(CoreJobSystem&&) = delete;

    int GetMaxConcurrency() const override { return static_cast<int>(jobs_.thread_count()); }

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 dependencies) override {
        JPH::uint32 index = 0;
        do {
            index = free_jobs_.ConstructObject(name, color, this, function, dependencies);
        } while (index == FreeJobs::cInvalidObjectIndex); // all in use: others finish, as this thread can't help here
        Job* const job = &free_jobs_.Get(index);
        JobHandle handle(job); // keeps it from going before this returns
        if (dependencies == 0) {
            QueueJob(job);
        }
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        jobs_.submit(core::Job{.entry = &run_job, .context = job, .counter = nullptr, .name = "physics"});
    }

    // In one go, so one wake-up does for them all.
    void QueueJobs(Job** jobs, JPH::uint count) override {
        core::Job batch[32];
        for (JPH::uint first = 0; first < count;) {
            const JPH::uint chunk = std::min<JPH::uint>(count - first, 32);
            for (JPH::uint i = 0; i < chunk; ++i) {
                jobs[first + i]->AddRef();
                batch[i] =
                    core::Job{.entry = &run_job, .context = jobs[first + i], .counter = nullptr, .name = "physics"};
            }
            jobs_.submit(std::span<const core::Job>(batch, chunk));
            first += chunk;
        }
    }

    void FreeJob(Job* job) override { free_jobs_.DestructObject(job); }

private:
    using FreeJobs = JPH::FixedSizeFreeList<Job>;

    static void run_job(void* context) {
        Job* const job = static_cast<Job*>(context);
        job->Execute();
        job->Release();
    }

    core::JobSystem& jobs_;
    FreeJobs free_jobs_;
};

// ---- Layers: what's worth testing against what
// ------------------------------------------------------------------------

namespace object_layer {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::ObjectLayer kDebris = 2; // hits only what's static
constexpr JPH::ObjectLayer kStaticBig =
    3; // a kilometer across or more: it overlaps everything, and would spoil the tree
constexpr JPH::uint kCount = 4;
} // namespace object_layer

// Each object layer has a broad phase tree of its own, so what's only tested against the world isn't in the way.
JPH::ObjectLayer as_object_layer(JPH::BroadPhaseLayer layer) {
    return static_cast<JPH::BroadPhaseLayer::Type>(layer);
}

bool is_static(JPH::ObjectLayer layer) {
    return layer == object_layer::kStatic || layer == object_layer::kStaticBig;
}

bool layers_collide(JPH::ObjectLayer first, JPH::ObjectLayer second) {
    if (is_static(first)) {
        return !is_static(second);
    }
    if (first == object_layer::kDebris) {
        return is_static(second);
    }
    return second != object_layer::kDebris;
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return object_layer::kCount; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        static constexpr const char* kNames[] = {"static", "moving", "debris", "static big"};
        return kNames[as_object_layer(layer)];
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase) const override {
        return layers_collide(layer, as_object_layer(broad_phase));
    }
};

class ObjectVsObject final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override {
        return layers_collide(first, second);
    }
};

// What a query looks through: all but debris, unless it asked.
class QueryBroadPhase final : public JPH::BroadPhaseLayerFilter {
public:
    explicit QueryBroadPhase(bool debris) : debris_(debris) {}
    bool ShouldCollide(JPH::BroadPhaseLayer layer) const override {
        return debris_ || as_object_layer(layer) != object_layer::kDebris;
    }

private:
    bool debris_;
};

class QueryLayers final : public JPH::ObjectLayerFilter {
public:
    explicit QueryLayers(bool debris) : debris_(debris) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return debris_ || layer != object_layer::kDebris; }

private:
    bool debris_;
};

// ---- Between our types and Jolt's
// -------------------------------------------------------------------------------------

JPH::RVec3 to_jolt(const DVec3& value) {
    return {value.x, value.y, value.z};
}

JPH::Vec3 to_jolt(const Vec3& value) {
    return {value.x, value.y, value.z};
}

JPH::Quat to_jolt(const Quat& value) {
    return JPH::Quat(value.x, value.y, value.z, value.w).Normalized();
}

DVec3 to_dvec3(JPH::RVec3Arg value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

Vec3 to_vec3(JPH::Vec3Arg value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

Quat to_quat(JPH::QuatArg value) {
    return Quat::wxyz(value.GetW(), value.GetX(), value.GetY(), value.GetZ());
}

class IgnoreEntity final : public JPH::BodyFilter {
public:
    explicit IgnoreEntity(flecs::entity_t entity) : entity_(entity) {}
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return entity_ == 0 || body.GetUserData() != entity_;
    }

private:
    flecs::entity_t entity_;
};

// A shape's kind and size, in whole millimeters, which is what bodies share one by.
struct ShapeSize {
    uint32_t kind = 0;
    int32_t size[3] = {};
    bool operator==(const ShapeSize&) const = default;
};
constexpr uint32_t kCharacterShape = 100; // beside ShapeKind's

int32_t to_mm(float meters) {
    return std::max(static_cast<int32_t>(std::lround(static_cast<double>(meters) * 1000.0)), 1);
}

float to_m(int32_t millimeters) {
    return static_cast<float>(static_cast<double>(millimeters) / 1000.0);
}

uint64_t hash_of(const ShapeSize& size) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](uint64_t word) {
        hash ^= word;
        hash *= 0x100000001b3ULL;
        hash ^= hash >> 29U;
    };
    mix(size.kind);
    for (const int32_t axis : size.size) {
        mix(static_cast<uint32_t>(axis));
    }
    return hash;
}

ShapeSize size_of(const Collider& collider, const Vec3& scale) {
    const Vec3 size = glm::abs(scale);
    ShapeSize out{.kind = static_cast<uint32_t>(collider.shape)};
    switch (collider.shape) {
    case ShapeKind::Box:
        for (int axis = 0; axis < 3; ++axis) {
            out.size[axis] = to_mm(collider.half_extents_m[axis] * size[axis]);
        }
        break;
    case ShapeKind::Sphere:
        out.size[0] = to_mm(collider.radius_m * std::max({size.x, size.y, size.z}));
        break;
    case ShapeKind::Capsule:
        out.size[0] = to_mm(collider.radius_m * std::max(size.x, size.z));
        out.size[1] = to_mm(collider.half_height_m * size.y);
        break;
    }
    return out;
}

JPH::ShapeSettings::ShapeResult make_shape(const ShapeSize& size) {
    if (size.kind == kCharacterShape) {
        const float radius = to_m(size.size[0]);
        const float half_height = to_m(size.size[1]);
        // With its origin at its feet.
        return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, half_height + radius, 0.0f), JPH::Quat::sIdentity(),
                                                   new JPH::CapsuleShape(half_height, radius))
            .Create();
    }
    switch (static_cast<ShapeKind>(size.kind)) {
    case ShapeKind::Box: {
        const JPH::Vec3 half(to_m(size.size[0]), to_m(size.size[1]), to_m(size.size[2]));
        return JPH::BoxShapeSettings(half, std::min(JPH::cDefaultConvexRadius, half.ReduceMin())).Create();
    }
    case ShapeKind::Sphere:
        return JPH::SphereShapeSettings(to_m(size.size[0])).Create();
    case ShapeKind::Capsule:
        return JPH::CapsuleShapeSettings(to_m(size.size[1]), to_m(size.size[0])).Create();
    }
    return {};
}

// ---- What the world holds
// ---------------------------------------------------------------------------------------------

// An entity's body, with what it last wrote to or read from its Transform, which tells a place() from its own motion.
struct BodyHandle {
    JPH::uint32 id = JPH::BodyID::cInvalidBodyID;
    DVec3 position{0.0};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct DynamicBody {};
struct KinematicBody {};

// An entity's character: one reference to it, taken when it's made.
struct CharacterHandle {
    JPH::CharacterVirtual* character = nullptr;
    DVec3 position{0.0};
};

} // namespace

struct Physics::State {
    explicit State(const PhysicsDesc& desc)
        : temp(desc.temp_memory_bytes), gravity(desc.gravity_mps2), core_jobs(desc.jobs) {
        system.Init(desc.max_bodies, 0, desc.max_body_pairs, desc.max_contact_constraints, broad_phase_layers,
                    object_vs_broad_phase, object_vs_object);
        system.SetGravity(to_jolt(desc.gravity_mps2));
        if (desc.jobs != nullptr && desc.jobs->worker_count() > 0) {
            jobs = ::new (allocate_job_system(sizeof(CoreJobSystem))) CoreJobSystem(*desc.jobs);
        } else {
            jobs = ::new (allocate_job_system(sizeof(JPH::JobSystemSingleThreaded)))
                JPH::JobSystemSingleThreaded(JPH::cMaxPhysicsJobs);
            single_threaded = true;
        }
    }
    ~State() {
        if (single_threaded) {
            static_cast<JPH::JobSystemSingleThreaded*>(jobs)->~JobSystemSingleThreaded();
        } else {
            static_cast<CoreJobSystem*>(jobs)->~CoreJobSystem();
        }
        JPH::AlignedFree(jobs);
    }
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    static void* allocate_job_system(size_t size) { return JPH::AlignedAllocate(size, JPH_CACHE_LINE_SIZE); }

    const JPH::Shape* shape_for(const ShapeSize& size);
    void create_bodies(flecs::iter& it);
    void create_characters(flecs::iter& it);
    void follow_transforms(float tick_s);
    void update_characters(float tick_s);
    void write_transforms();
    void step(flecs::iter& it);

    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhase object_vs_broad_phase;
    ObjectVsObject object_vs_object;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImplWithMallocFallback temp;
    JPH::JobSystem* jobs = nullptr;
    bool single_threaded = false;
    Vec3 gravity;
    core::JobSystem* core_jobs = nullptr;
    uint32_t characters = 0;
    bool warned_out_of_bodies = false;
    JPH::EPhysicsUpdateError warned_errors = JPH::EPhysicsUpdateError::None;

    // Every shape made, by size, and the references that keep them.
    struct SharedShape {
        ShapeSize size;
        const JPH::Shape* shape = nullptr;
    };
    core::HashMap<uint64_t, SharedShape> shapes_by_size{core::heap_allocator(MemoryTag::Physics)};
    JPH::Array<JPH::RefConst<JPH::Shape>> shapes;
    // What's being added this tick, kept so it doesn't allocate each time.
    JPH::Array<JPH::BodyID> new_still;
    JPH::Array<JPH::BodyID> new_moving;

    // Named, so the world owns them. It goes first, and a query's destructor reads it, so they're never destroyed here.
    struct Queries {
        flecs::query<const Collider, const RigidBody, const WorldTransform> new_bodies;
        flecs::query<CharacterController, const WorldTransform> new_characters;
        flecs::query<const Transform, BodyHandle> placed_bodies;
        flecs::query<Transform, BodyHandle> dynamic_bodies;
        flecs::query<const Transform, BodyHandle> kinematic_bodies;
        flecs::query<CharacterController, Transform, CharacterHandle> characters;
    };
    alignas(Queries) std::byte queries_storage[sizeof(Queries)];
    Queries& queries() { return *reinterpret_cast<Queries*>(queries_storage); }
};

// One shape for all of a size. Null, and said so, if Jolt won't have it.
const JPH::Shape* Physics::State::shape_for(const ShapeSize& size) {
    const uint64_t key = hash_of(size);
    if (const SharedShape* const shared = shapes_by_size.find(key); shared != nullptr && shared->size == size) {
        return shared->shape;
    }
    const JPH::ShapeSettings::ShapeResult result = make_shape(size);
    if (result.HasError()) {
        DE_LOG_ERROR("physics", "a collider's shape: %s", result.GetError().c_str());
        return nullptr;
    }
    const JPH::RefConst<JPH::Shape>& kept = shapes.emplace_back(result.Get());
    // Two sizes with one hash: the second is simply not shared.
    if (shapes_by_size.find(key) == nullptr) {
        shapes_by_size.set(key, SharedShape{.size = size, .shape = kept.GetPtr()});
    }
    return kept.GetPtr();
}

void Physics::State::create_bodies(flecs::iter& it) {
    JPH::BodyInterface& bodies = system.GetBodyInterfaceNoLock();
    queries().new_bodies.each(
        [&](flecs::entity entity, const Collider& collider, const RigidBody& body, const WorldTransform& place) {
            const JPH::Shape* const shape = shape_for(size_of(collider, place.scale));
            if (shape == nullptr) {
                entity.mut(it).set(BodyHandle{}); // said so once: it won't be asked again
                return;
            }
            const bool moves = body.kind != BodyKind::Static;
            const JPH::EMotionType motion = body.kind == BodyKind::Static      ? JPH::EMotionType::Static
                                            : body.kind == BodyKind::Kinematic ? JPH::EMotionType::Kinematic
                                                                               : JPH::EMotionType::Dynamic;
            JPH::ObjectLayer layer = object_layer::kMoving;
            if (!moves) {
                constexpr float kBigM = 1000.0f;
                const JPH::Vec3 extent = shape->GetLocalBounds().GetSize();
                layer = extent.ReduceMax() >= kBigM ? object_layer::kStaticBig : object_layer::kStatic;
            } else if (body.debris && body.kind == BodyKind::Dynamic) {
                layer = object_layer::kDebris;
            }
            JPH::BodyCreationSettings settings(shape, to_jolt(place.position), to_jolt(place.rotation), motion, layer);
            settings.mFriction = body.friction;
            settings.mRestitution = body.restitution;
            settings.mLinearDamping = body.linear_damping;
            settings.mAngularDamping = body.angular_damping;
            settings.mUserData = entity.id();
            if (body.kind == BodyKind::Dynamic && body.mass_kg > 0.0f) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = body.mass_kg;
            }
            const JPH::Body* const created = bodies.CreateBody(settings);
            if (created == nullptr) {
                if (!warned_out_of_bodies) {
                    DE_LOG_ERROR("physics", "out of bodies (%u): the rest have none", system.GetMaxBodies());
                    warned_out_of_bodies = true;
                }
                entity.mut(it).set(BodyHandle{});
                return;
            }
            const bool asleep = !moves || (body.kind == BodyKind::Dynamic && body.start_asleep);
            (asleep ? new_still : new_moving).push_back(created->GetID());
            entity.mut(it).set(BodyHandle{.id = created->GetID().GetIndexAndSequenceNumber(),
                                          .position = place.position,
                                          .rotation = place.rotation});
            if (body.kind == BodyKind::Dynamic) {
                entity.mut(it).add<DynamicBody>();
            } else if (body.kind == BodyKind::Kinematic) {
                entity.mut(it).add<KinematicBody>();
            }
        });
    // Together, as adding them one by one leaves the broad phase in a poor state, and after a lot, it's rebuilt.
    const size_t added = new_still.size() + new_moving.size();
    const auto add = [&bodies](JPH::Array<JPH::BodyID>& ids, JPH::EActivation activation) {
        if (!ids.empty()) {
            const auto count = static_cast<int>(ids.size());
            JPH::BodyInterface::AddState state = bodies.AddBodiesPrepare(ids.data(), count);
            bodies.AddBodiesFinalize(ids.data(), count, state, activation);
            ids.clear();
        }
    };
    add(new_still, JPH::EActivation::DontActivate);
    add(new_moving, JPH::EActivation::Activate);
    constexpr size_t kOptimizeAfter = 1024;
    if (added >= kOptimizeAfter) {
        system.OptimizeBroadPhase();
    }
}

void Physics::State::create_characters(flecs::iter& it) {
    queries().new_characters.each([&](flecs::entity entity, CharacterController& controller,
                                      const WorldTransform& place) {
        const float radius = std::max(controller.radius_m, 1e-3f);
        const ShapeSize size{.kind = kCharacterShape,
                             .size = {to_mm(radius), to_mm(std::max(controller.height_m * 0.5f - radius, 1e-3f)), 0}};
        JPH::CharacterVirtualSettings settings;
        settings.mShape = shape_for(size);
        settings.mMaxSlopeAngle = controller.max_slope_rad;
        settings.mMass = controller.mass_kg;
        settings.mUp = to_jolt(kWorldUp);
        auto* const character =
            new JPH::CharacterVirtual(&settings, to_jolt(place.position), JPH::Quat::sIdentity(), entity.id(), &system);
        character->AddRef();
        entity.mut(it).set(CharacterHandle{.character = character, .position = place.position});
        ++characters;
    });
}

// A kinematic body heads for its Transform this step. A dynamic one that's been place()d goes straight there: only
// tables something wrote to are looked at, and they're not marked as written to by this.
void Physics::State::follow_transforms(float tick_s) {
    JPH::BodyInterface& bodies = system.GetBodyInterfaceNoLock();
    queries().kinematic_bodies.run([&](flecs::iter& it) {
        while (it.next()) {
            const auto transforms = it.field<const Transform>(0);
            const auto handles = it.field<BodyHandle>(1);
            for (const auto row : it) {
                BodyHandle& handle = handles[row];
                const Transform& transform = transforms[row];
                const JPH::BodyID id(handle.id);
                const bool changed = transform.position != handle.position || transform.rotation != handle.rotation;
                if (changed || bodies.IsActive(id)) { // still moving, it has to be told to stop
                    bodies.MoveKinematic(id, to_jolt(transform.position), to_jolt(transform.rotation), tick_s);
                    handle.position = transform.position;
                    handle.rotation = transform.rotation;
                }
            }
        }
    });
    queries().placed_bodies.run([&](flecs::iter& it) {
        while (it.next()) {
            if (!it.changed()) {
                it.skip();
                continue;
            }
            const auto transforms = it.field<const Transform>(0);
            const auto handles = it.field<BodyHandle>(1);
            for (const auto row : it) {
                BodyHandle& handle = handles[row];
                const Transform& transform = transforms[row];
                if (transform.position != handle.position || transform.rotation != handle.rotation) {
                    const JPH::BodyID id(handle.id);
                    bodies.SetPositionAndRotation(id, to_jolt(transform.position), to_jolt(transform.rotation),
                                                  JPH::EActivation::Activate);
                    bodies.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                    handle.position = transform.position;
                    handle.rotation = transform.rotation;
                }
            }
        }
    });
}

void Physics::State::update_characters(float tick_s) {
    const JPH::Vec3 gravity_step = to_jolt(gravity) * tick_s;
    const JPH::DefaultBroadPhaseLayerFilter broad_phase = system.GetDefaultBroadPhaseLayerFilter(object_layer::kMoving);
    const JPH::DefaultObjectLayerFilter layers = system.GetDefaultLayerFilter(object_layer::kMoving);
    const JPH::BodyFilter body_filter;
    const JPH::ShapeFilter shape_filter;
    queries().characters.run([&](flecs::iter& it) {
        while (it.next()) {
            const auto controllers = it.field<CharacterController>(0);
            const auto transforms = it.field<Transform>(1);
            const auto handles = it.field<CharacterHandle>(2);
            for (const auto row : it) {
                CharacterController& controller = controllers[row];
                Transform& transform = transforms[row];
                CharacterHandle& handle = handles[row];
                JPH::CharacterVirtual& character = *handle.character;
                const bool placed = transform.position != handle.position;
                if (placed) {
                    character.SetPosition(to_jolt(transform.position));
                    character.SetLinearVelocity(JPH::Vec3::sZero());
                }
                character.SetMaxSlopeAngle(controller.max_slope_rad);
                character.UpdateGroundVelocity();
                const JPH::Vec3 up = character.GetUp();
                // On the ground it goes with the ground; in the air, it keeps its fall. Gravity is always on, so it
                // stays pressed down, and across the ground is what was asked for.
                JPH::Vec3 velocity = character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround
                                         ? character.GetGroundVelocity()
                                         : character.GetLinearVelocity().Dot(up) * up;
                velocity += gravity_step + to_jolt(controller.desired_velocity_mps);
                character.SetLinearVelocity(velocity);

                JPH::CharacterVirtual::ExtendedUpdateSettings update;
                update.mWalkStairsStepUp = up * controller.step_height_m;
                update.mStickToFloorStepDown = -up * controller.step_height_m;
                character.ExtendedUpdate(tick_s, to_jolt(gravity), update, broad_phase, layers, body_filter,
                                         shape_filter, temp);

                handle.position = to_dvec3(character.GetPosition());
                transform.position = handle.position;
                controller.velocity_mps = to_vec3(character.GetLinearVelocity());
                controller.grounded = character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
                controller.ground_normal = to_vec3(character.GetGroundNormal());
            }
        }
    });
}

// What a step moved, across the job system: each row is one body's. A table with nothing awake in it isn't marked as
// written to, and when all's asleep, no table is looked at.
void Physics::State::write_transforms() {
    if (system.GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0) {
        return;
    }
    const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock();
    queries().dynamic_bodies.run([&](flecs::iter& it) {
        while (it.next()) {
            const auto transforms = it.field<Transform>(0);
            const auto handles = it.field<BodyHandle>(1);
            std::atomic<bool> any_awake{false};
            world::for_each_row_parallel(core_jobs, it.count(), 2048, [&](size_t row) {
                BodyHandle& handle = handles[row];
                const JPH::Body* const body = locks.TryGetBody(JPH::BodyID(handle.id));
                if (body == nullptr || !body->IsActive()) {
                    return;
                }
                handle.position = to_dvec3(body->GetPosition());
                handle.rotation = to_quat(body->GetRotation());
                transforms[row].position = handle.position;
                transforms[row].rotation = handle.rotation;
                any_awake.store(true, std::memory_order_relaxed);
            });
            if (!any_awake.load(std::memory_order_relaxed)) {
                it.skip();
            }
        }
    });
}

void Physics::State::step(flecs::iter& it) {
    DE_PROFILE_ZONE("physics step");
    const float tick_s = it.delta_time();
    create_bodies(it);
    create_characters(it);
    // Handles made above wait for the tick's end, so what's new joins in from the next tick. Its body is in this step.
    follow_transforms(tick_s);
    update_characters(tick_s);
    const JPH::EPhysicsUpdateError errors = system.Update(tick_s, 1, &temp, jobs);
    // Once per kind of error: it would go on every tick.
    const auto new_errors = static_cast<uint32_t>(errors) & ~static_cast<uint32_t>(warned_errors);
    if (new_errors != 0) {
        DE_LOG_WARN(
            "physics", "a step ran out of room (%s%s%s): raise the limits in PhysicsDesc",
            (new_errors & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ManifoldCacheFull)) != 0 ? "manifolds " : "",
            (new_errors & static_cast<uint32_t>(JPH::EPhysicsUpdateError::BodyPairCacheFull)) != 0 ? "body pairs " : "",
            (new_errors & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ContactConstraintsFull)) != 0 ? "contacts"
                                                                                                        : "");
        warned_errors = static_cast<JPH::EPhysicsUpdateError>(static_cast<uint32_t>(warned_errors) | new_errors);
    }
    write_transforms();
    DE_PROFILE_PLOT("physics bodies", static_cast<int64_t>(system.GetNumBodies()));
    DE_PROFILE_PLOT("physics active bodies",
                    static_cast<int64_t>(system.GetNumActiveBodies(JPH::EBodyType::RigidBody)));
}

Physics::Physics(const PhysicsDesc& desc) {
    acquire_jolt();
    core::Allocator& heap = core::heap_allocator(MemoryTag::Physics);
    state_ = ::new (heap.allocate(sizeof(State), alignof(State))) State(desc);
}

Physics::~Physics() {
    state_->~State();
    core::heap_allocator(MemoryTag::Physics).deallocate(state_, sizeof(State), alignof(State));
    release_jolt();
}

void Physics::add_to_world(world::World& world) {
    const flecs::world& ecs = world.ecs();
    ecs.component<ShapeKind>("de::ShapeKind")
        .constant("Box", ShapeKind::Box)
        .constant("Sphere", ShapeKind::Sphere)
        .constant("Capsule", ShapeKind::Capsule);
    ecs.component<BodyKind>("de::BodyKind")
        .constant("Static", BodyKind::Static)
        .constant("Kinematic", BodyKind::Kinematic)
        .constant("Dynamic", BodyKind::Dynamic);
    ecs.component<Collider>("de::Collider")
        .member<ShapeKind>("shape")
        .member<Vec3>("half_extents_m")
        .member<float>("radius_m")
        .member<float>("half_height_m");
    ecs.component<RigidBody>("de::RigidBody")
        .member<BodyKind>("kind")
        .member<float>("mass_kg")
        .member<float>("friction")
        .member<float>("restitution")
        .member<float>("linear_damping")
        .member<float>("angular_damping")
        .member<bool>("debris")
        .member<bool>("start_asleep");
    ecs.component<CharacterController>("de::CharacterController")
        .member<float>("radius_m")
        .member<float>("height_m")
        .member<float>("max_slope_rad")
        .member<float>("step_height_m")
        .member<float>("mass_kg")
        .member<Vec3>("desired_velocity_mps")
        .member<Vec3>("velocity_mps")
        .member<Vec3>("ground_normal")
        .member<bool>("grounded");
    ecs.component<BodyHandle>("de::BodyHandle");
    ecs.component<CharacterHandle>("de::CharacterHandle");
    ecs.component<DynamicBody>("de::DynamicBody");
    ecs.component<KinematicBody>("de::KinematicBody");

    State* const state = state_;
    ::new (static_cast<void*>(state->queries_storage)) State::Queries{
        .new_bodies = ecs.query_builder<const Collider, const RigidBody, const WorldTransform>("de::physics::NewBodies")
                          .cached()
                          .without<BodyHandle>()
                          .build(),
        .new_characters = ecs.query_builder<CharacterController, const WorldTransform>("de::physics::NewCharacters")
                              .cached()
                              .without<CharacterHandle>()
                              .build(),
        // Written to itself by write_transforms: a change to its handle alone doesn't make a table "changed".
        .placed_bodies = ecs.query_builder<const Transform, BodyHandle>("de::physics::PlacedBodies")
                             .cached()
                             .with<DynamicBody>()
                             .term_at(1)
                             .out()
                             .detect_changes()
                             .build(),
        .dynamic_bodies = ecs.query_builder<Transform, BodyHandle>("de::physics::DynamicBodies")
                              .cached()
                              .with<DynamicBody>()
                              .term_at(0)
                              .out()
                              .build(),
        .kinematic_bodies = ecs.query_builder<const Transform, BodyHandle>("de::physics::KinematicBodies")
                                .cached()
                                .with<KinematicBody>()
                                .build(),
        .characters = ecs.query_builder<CharacterController, Transform, CharacterHandle>("de::physics::Characters")
                          .cached()
                          .build()};

    ecs.system("de::PhysicsStep").kind(world.phase(world::Phase::Physics)).run([state](flecs::iter& it) {
        state->step(it);
    });

    // A body goes with its handle, and its handle with what asked for it.
    ecs.observer<BodyHandle>("de::DestroyBody").event(flecs::OnRemove).each([state](flecs::entity, BodyHandle& handle) {
        const JPH::BodyID id(handle.id);
        if (!id.IsInvalid()) {
            JPH::BodyInterface& bodies = state->system.GetBodyInterfaceNoLock();
            bodies.RemoveBody(id);
            bodies.DestroyBody(id);
        }
    });
    ecs.observer<Collider>("de::DropBodyOfCollider").event(flecs::OnRemove).each([](flecs::entity entity, Collider&) {
        entity.remove<BodyHandle>();
    });
    ecs.observer<RigidBody>("de::DropBodyOfRigidBody")
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, RigidBody&) { entity.remove<BodyHandle>(); });
    ecs.observer<CharacterHandle>("de::DestroyCharacter")
        .event(flecs::OnRemove)
        .each([state](flecs::entity, CharacterHandle& handle) {
            if (handle.character != nullptr) {
                handle.character->Release();
                --state->characters;
            }
        });
    ecs.observer<CharacterController>("de::DropCharacter")
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, CharacterController&) { entity.remove<CharacterHandle>(); });
}

bool Physics::ray_cast(const DVec3& origin, const Vec3& direction, float distance_m, QueryHit& hit,
                       const QueryFilter& filter) const {
    const JPH::RRayCast ray(to_jolt(origin), to_jolt(direction) * distance_m);
    JPH::RayCastResult result;
    const IgnoreEntity bodies(filter.ignore);
    const QueryBroadPhase broad_phase(filter.debris);
    const QueryLayers layers(filter.debris);
    if (!state_->system.GetNarrowPhaseQueryNoLock().CastRay(ray, result, broad_phase, layers, bodies)) {
        return false;
    }
    hit.position = to_dvec3(ray.GetPointOnRay(result.mFraction));
    hit.distance_m = result.mFraction * distance_m;
    const JPH::BodyLockRead lock(state_->system.GetBodyLockInterfaceNoLock(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.entity = body.GetUserData();
        hit.normal = to_vec3(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
    }
    return true;
}

bool Physics::sphere_cast(const DVec3& origin, float radius_m, const Vec3& direction, float distance_m, QueryHit& hit,
                          const QueryFilter& filter) const {
    const JPH::SphereShape sphere(radius_m);
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(&sphere, JPH::Vec3::sReplicate(1.0f),
                                                                      JPH::RMat44::sTranslation(to_jolt(origin)),
                                                                      to_jolt(direction) * distance_m);
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> closest;
    const IgnoreEntity bodies(filter.ignore);
    const QueryBroadPhase broad_phase(filter.debris);
    const QueryLayers layers(filter.debris);
    state_->system.GetNarrowPhaseQueryNoLock().CastShape(cast, settings, to_jolt(origin), closest, broad_phase, layers,
                                                         bodies);
    if (!closest.HadHit()) {
        return false;
    }
    const JPH::ShapeCastResult& result = closest.mHit;
    hit.position = origin + DVec3(to_vec3(result.mContactPointOn2));
    hit.distance_m = result.mFraction * distance_m;
    hit.normal = to_vec3(-result.mPenetrationAxis.Normalized());
    const JPH::BodyLockRead lock(state_->system.GetBodyLockInterfaceNoLock(), result.mBodyID2);
    if (lock.Succeeded()) {
        hit.entity = lock.GetBody().GetUserData();
    }
    return true;
}

PhysicsStats Physics::stats() const {
    return {.bodies = state_->system.GetNumBodies(),
            .active_bodies = state_->system.GetNumActiveBodies(JPH::EBodyType::RigidBody),
            .characters = state_->characters,
            .shapes = static_cast<uint32_t>(state_->shapes.size())};
}

} // namespace de::physics
