#include <Arcane/Input/InputBindingProfile.hpp>

#include <Arcane/Input/InputActions.hpp>

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        bool ValidOverride(const Guid& id, std::string_view path, const InputActionAsset& asset)
        {
            const bool vectorPath = path == "<Gamepad>/leftStick" ||
                                    path == "<Gamepad>/rightStick";
            bool found = false;
            bool compatible = false;
            for (const auto& map : asset.actionMaps)
                for (const auto& action : map.actions)
                    for (const auto& binding : action.bindings)
                    {
                        if (binding.id == id)
                        {
                            found = true;
                            compatible = binding.composite.empty() &&
                                (action.type == InputActionType::Axis2D) == vectorPath;
                        }
                        for (const auto& part : binding.parts)
                            if (part.id == id)
                            {
                                found = true;
                                compatible = !vectorPath;
                            }
                    }
            if (!found || !compatible) return false;
            auto evaluator = InputActions::Create();
            return evaluator->LoadAsset(asset) && evaluator->SetBindingPath(id, path);
        }

        bool WriteAtomically(const std::filesystem::path& path,
                             const std::unordered_map<Guid, std::string>& overrides)
        {
            std::error_code error;
            if (path.empty()) return false;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path(), error);
                if (error) return false;
            }
            nlohmann::json doc = { { "version", 1 }, { "overrides", nlohmann::json::object() } };
            for (const auto& [id, replacement] : overrides)
                doc["overrides"][id.ToString()] = replacement;
            auto temporary = path;
            temporary += "." + Guid::Generate().ToString() + ".tmp";
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream) return false;
                stream << doc.dump(2) << '\n';
                stream.flush();
                if (!stream)
                {
                    stream.close();
                    std::filesystem::remove(temporary, error);
                    return false;
                }
            }
#ifdef _WIN32
            const bool replaced = MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            std::filesystem::rename(temporary, path, error);
            const bool replaced = !error;
#endif
            if (!replaced) std::filesystem::remove(temporary, error);
            return replaced;
        }
    }

    ProfileLoadResult InputBindingProfile::Read(const std::filesystem::path& path,
                                                const InputActionAsset& asset, bool markDirty)
    {
        std::error_code error;
        if (!std::filesystem::exists(path, error))
            return { error ? ProfileLoadStatus::Invalid : ProfileLoadStatus::Missing, {} };
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return { ProfileLoadStatus::Invalid, { "profile cannot be opened" } };
        const auto doc = nlohmann::json::parse(stream, nullptr, false);
        if (doc.is_discarded() || !doc.is_object() ||
            !doc.contains("version") || doc["version"] != 1 ||
            !doc.contains("overrides") || !doc["overrides"].is_object())
            return { ProfileLoadStatus::Invalid, { "invalid profile schema" } };

        std::unordered_map<Guid, std::string> next;
        ProfileLoadResult result { ProfileLoadStatus::Loaded, {} };
        for (auto it = doc["overrides"].begin(); it != doc["overrides"].end(); ++it)
        {
            const auto id = Guid::FromString(it.key());
            if (!id || id->IsNil() || !it.value().is_string() ||
                !ValidOverride(*id, it.value().get_ref<const std::string&>(), asset))
            {
                result.diagnostics.push_back("ignored stale or invalid binding override " + it.key());
                continue;
            }
            next[*id] = it.value().get<std::string>();
        }
        overrides_ = std::move(next);
        dirty_ = markDirty;
        return result;
    }

    ProfileLoadResult InputBindingProfile::Load(const std::filesystem::path& path,
                                                 const InputActionAsset& asset)
    {
        return Read(path, asset, false);
    }

    ProfileLoadResult InputBindingProfile::Import(const std::filesystem::path& path,
                                                   const InputActionAsset& asset)
    {
        return Read(path, asset, true);
    }

    bool InputBindingProfile::Save(const std::filesystem::path& path)
    {
        if (!WriteAtomically(path, overrides_)) return false;
        dirty_ = false;
        return true;
    }

    bool InputBindingProfile::Export(const std::filesystem::path& path) const
    {
        return WriteAtomically(path, overrides_);
    }

    bool InputBindingProfile::SetOverride(const Guid& id, std::string_view path,
                                          const InputActionAsset& asset)
    {
        if (!ValidOverride(id, path, asset)) return false;
        overrides_.insert_or_assign(id, std::string(path));
        dirty_ = true;
        return true;
    }

    bool InputBindingProfile::RemoveOverride(const Guid& id)
    {
        if (overrides_.erase(id) == 0) return false;
        dirty_ = true;
        return true;
    }

    void InputBindingProfile::Reset()
    {
        if (!overrides_.empty()) dirty_ = true;
        overrides_.clear();
    }
}
