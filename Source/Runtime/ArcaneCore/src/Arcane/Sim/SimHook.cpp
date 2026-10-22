#include <Arcane/Sim/SimHook.hpp>

#include <algorithm>
#include <exception>   // std::terminate
#include <mutex>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct HookRegistry
        {
            std::mutex mutex;
            std::vector<SimHook> hooks;
        };
        HookRegistry& Hooks() { static HookRegistry registry; return registry; }

        bool OrderLess(const SimHook& a, const SimHook& b) noexcept { return a.order < b.order; }
    }

    void RegisterSimHook(SimHook hook) noexcept
    {
        try
        {
            auto& registry = Hooks();
            std::lock_guard lock(registry.mutex);
            // upper_bound: an equal order lands AFTER the existing ones.
            const auto at = std::upper_bound(registry.hooks.begin(), registry.hooks.end(), hook, OrderLess);
            registry.hooks.insert(at, hook);
        }
        catch (...) { std::terminate(); }
    }

    std::span<const SimHook> SimHooks() noexcept
    {
        auto& registry = Hooks();
        std::lock_guard lock(registry.mutex);
        return { registry.hooks.data(), registry.hooks.size() };
    }

    void ReplaceSimHooksForTest(std::span<const SimHook> hooks) noexcept
    {
        try
        {
            // Copy first: `hooks` may view the registry's own storage.
            std::vector<SimHook> next(hooks.begin(), hooks.end());
            std::stable_sort(next.begin(), next.end(), OrderLess);
            auto& registry = Hooks();
            std::lock_guard lock(registry.mutex);
            registry.hooks = std::move(next);
        }
        catch (...) { std::terminate(); }
    }
}
