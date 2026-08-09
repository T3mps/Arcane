#pragma once

// Module: the engine's low-level RAII wrapper for one loaded system module
// (DLL/shared object). It knows paths and raw symbols only; plugin ABI and
// reload policy live above this layer.

#include <Arcane/Base/Api.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>

namespace Arcane
{
    class ARCANE_API Module
    {
    public:
        Module() = default;
        ~Module();

        Module(const Module&) = delete;
        Module& operator=(const Module&) = delete;

        Module(Module&& other) noexcept;
        Module& operator=(Module&& other) noexcept;

        static std::optional<Module> Load(std::filesystem::path path);

        void* Symbol(const char* name) const noexcept;
        void Unload() noexcept;

        [[nodiscard]] bool IsLoaded() const noexcept { return m_handle != nullptr; }
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

        // The loaded image's address span. Exists so callers can DISOWN anything
        // that stored a pointer INTO this module before it is unmapped -- most
        // importantly Astra's ComponentDescriptors, whose raw function pointers
        // are left aiming at freed code by Unload() (see PluginHost::TeardownImage
        // and ComponentRegistry::UnregisterModuleRange).
        //
        // size == 0 means "unknown": not loaded, or a platform with no
        // implementation. Callers must treat that as "cannot disown" rather than
        // as an empty range.
        struct ImageSpan
        {
            const void* base = nullptr;
            std::size_t size = 0;
        };
        [[nodiscard]] ImageSpan Image() const noexcept;

    private:
        using NativeHandle = void*;

        Module(std::filesystem::path path, NativeHandle handle) noexcept;

        std::filesystem::path m_path;
        NativeHandle m_handle = nullptr;
    };
}
