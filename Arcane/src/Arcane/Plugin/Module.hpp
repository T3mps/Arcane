#pragma once

// Module: ArcaneRuntime's low-level RAII wrapper for one loaded system module
// (DLL/shared object). It knows paths and raw symbols only; plugin ABI and
// reload policy live above this layer.

#include <Arcane/Base/Api.hpp>

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

    private:
        using NativeHandle = void*;

        Module(std::filesystem::path path, NativeHandle handle) noexcept;

        std::filesystem::path m_path;
        NativeHandle m_handle = nullptr;
    };
}
