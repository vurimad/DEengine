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
//
// Accepts anything callable as R(Args...), like std::function: lambdas, functors and free
// functions whose signature converts. With R = void, a returned value is discarded.
template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
    using GenericFunction = void (*)();

    union Target {
        void* object;
        GenericFunction function;
    };

public:
    // Free functions. Implicit by design, so callables pass straight into FunctionRef parameters.
    template <typename Fn>
        requires(std::is_function_v<Fn> && std::is_invocable_r_v<R, Fn*, Args...>)
    FunctionRef(Fn* function) noexcept : trampoline_(&call_function<Fn>) { // NOLINT
        DE_ASSERT(function != nullptr);
        target_.function = reinterpret_cast<GenericFunction>(function);
    }

    // Lambdas and functors, referenced in place (never copied).
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
    template <typename F>
    static R call_object(Target target, Args... args) {
        F& callable = *static_cast<F*>(target.object);
        if constexpr (std::is_void_v<R>) {
            callable(std::forward<Args>(args)...);
        } else {
            return callable(std::forward<Args>(args)...);
        }
    }

    // Function pointers round-trip through GenericFunction: casting back to the original type is well-defined.
    template <typename Fn>
    static R call_function(Target target, Args... args) {
        Fn* function = reinterpret_cast<Fn*>(target.function);
        if constexpr (std::is_void_v<R>) {
            function(std::forward<Args>(args)...);
        } else {
            return function(std::forward<Args>(args)...);
        }
    }

    Target target_{};
    R (*trampoline_)(Target, Args...);
};

} // namespace de::core
