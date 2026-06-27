#pragma once

// FunctionRef<Sig>: a non-owning, zero-allocation callable VIEW (void* + thunk).
// For SYNCHRONOUS, non-escaping callbacks only -- the referent MUST outlive the
// FunctionRef. Do NOT store one (it would dangle); a stored callback uses an
// owning std::function (or a future Delegate). C++26 std::function_ref-aligned:
// this can be replaced by a using-alias when MSVC ships it.

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace Arcane
{
    template <class Sig> class FunctionRef;

    template <class R, class... Args>
    class FunctionRef<R(Args...)>
    {
        void* m_obj = nullptr;
        R (*m_thunk)(void*, Args...) = nullptr;

    public:
        FunctionRef() = default;

        // Implicit by design: call sites pass a lambda directly.
        template <class F>
            requires (!std::is_same_v<std::remove_cvref_t<F>, FunctionRef>
                      && std::is_invocable_r_v<R, F&, Args...>)
        FunctionRef(F&& f) noexcept
            : m_obj(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
              m_thunk(+[](void* o, Args... a) -> R {
                  return (*static_cast<std::remove_reference_t<F>*>(o))(static_cast<Args&&>(a)...);
              })
        {
        }

        R operator()(Args... a) const { return m_thunk(m_obj, static_cast<Args&&>(a)...); }

        explicit operator bool() const noexcept { return m_thunk != nullptr; }
    };
}
