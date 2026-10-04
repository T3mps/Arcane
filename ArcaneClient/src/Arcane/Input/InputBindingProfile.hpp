#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputActionAsset.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    enum class ProfileLoadStatus { Loaded, Missing, Invalid };

    struct ProfileLoadResult
    {
        ProfileLoadStatus status = ProfileLoadStatus::Missing;
        std::vector<std::string> diagnostics;
    };

    class ARC_API InputBindingProfile
    {
    public:
        [[nodiscard]] ProfileLoadResult Load(const std::filesystem::path& path,
                                              const InputActionAsset& asset);
        [[nodiscard]] ProfileLoadResult Import(const std::filesystem::path& path,
                                                const InputActionAsset& asset);
        [[nodiscard]] bool Save(const std::filesystem::path& path);
        [[nodiscard]] bool Export(const std::filesystem::path& path) const;
        [[nodiscard]] bool SetOverride(const Guid& bindingId, std::string_view path,
                                        const InputActionAsset& asset);
        [[nodiscard]] bool RemoveOverride(const Guid& bindingId);
        void Reset();
        [[nodiscard]] bool Dirty() const noexcept { return dirty_; }
        [[nodiscard]] const std::unordered_map<Guid, std::string>& Overrides() const noexcept
        {
            return overrides_;
        }

    private:
        [[nodiscard]] ProfileLoadResult Read(const std::filesystem::path& path,
                                              const InputActionAsset& asset, bool markDirty);
        std::unordered_map<Guid, std::string> overrides_;
        bool dirty_ = false;
    };
}
