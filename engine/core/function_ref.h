#pragma once

#include "core/assert.h"

#include <type_traits>
#include <utility>

namespace de::core {

template <typename Signature>
class FunctionRef;

// Non-owning reference to a callable (replaces std::function for callbacks). Two pointers,
// never allocates, one indirect call.
//
// It does not keep the callable alive: use it for parameters, never store it beyond the call.
//     void for_each_enemy(FunctionRef<void(Entity)> visit);
//     for_each_enemy([&](Entity e) { ++count; });
template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
public:
    using FunctionPointer = R (*)(Args...);

    // Implicit by design, so callables pass straight into FunctionRef parameters.
    FunctionRef(FunctionPointer function) noexcept : trampoline_(&call_function) { // NOLINT
        DE_ASSERT(function != nullptr);
        target_.function = function;
    }

    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
                 !std::is_function_v<std::remove_reference_t<F>> && !std::is_pointer_v<std::remove_cvref_t<F>> &&
                 std::is_invocable_r_v<R, F&, Args...>)
    FunctionRef(F&& callable) noexcept // NOLINT
        : trampoline_(&call_object<std::remove_reference_t<F>>) {
        target_.object = const_cast<void*>(static_cast<const void*>(&callable));
    }

    R operator()(Args... args) const { return trampoline_(target_, std::forward<Args>(args)...); }

private:
    union Target {
        void* object;
        FunctionPointer function;
    };

    template <typename F>
    static R call_object(Target target, Args... args) {
        return (*static_cast<F*>(target.object))(std::forward<Args>(args)...);
    }

    static R call_function(Target target, Args... args) { return target.function(std::forward<Args>(args)...); }

    Target target_{};
    R (*trampoline_)(Target, Args...);
};

} // namespace de::core
