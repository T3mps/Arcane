#include <Arcane/Core/ModuleAttach.hpp>
#include <exception>   // std::terminate (libc++ in Dist reaches it through nothing else)
#include <mutex>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct AttachRegistry
        {
            std::mutex mutex;
            std::vector<ModuleAttachFn> callbacks;
            Astra::TypeContext* current = nullptr;
        };
        AttachRegistry& Registry() { static AttachRegistry registry; return registry; }
    }

    void RegisterModuleAttach(const char*, ModuleAttachFn fn) noexcept
    {
        try
        {
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.callbacks.push_back(fn);
            if (registry.current) fn(registry.current);
        }
        catch (...) { std::terminate(); }
    }

    void AttachAllModules(Astra::TypeContext* ctx) noexcept
    {
        try
        {
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.current = ctx;
            for (auto fn : registry.callbacks) fn(ctx);
        }
        catch (...) { std::terminate(); }
    }
}
