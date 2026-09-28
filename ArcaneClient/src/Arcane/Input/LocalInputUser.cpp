#include <Arcane/Input/LocalInputUser.hpp>

#include <Arcane/Base/Log.hpp>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>

namespace Arcane
{
    LocalInputUser::LocalInputUser() : actions_(InputActions::Create()) {}

    bool LocalInputUser::Configure(const InputActionAsset& asset, const Guid& projectId)
    {
        if (projectId.IsNil()) return false;
        auto next = InputActions::Create();
        if (!next->LoadAsset(asset)) return false;
        Clear();
        actions_ = std::move(next);
        asset_ = asset;
        projectId_ = projectId;
        if (asset.defaultMap) (void)SetBaseMap(*asset.defaultMap);
        char* pref = SDL_GetPrefPath("Arcane", "Arcane");
        if (pref)
        {
            profileRoot_ = std::filesystem::path(reinterpret_cast<const char8_t*>(pref)) / "InputProfiles" / projectId.ToString();
            SDL_free(pref);
        }
        else
        {
            ARC_WARN("input: SDL_GetPrefPath failed; binding profiles cannot be saved");
        }
        const auto loaded = LoadProfile("Default");
        if (loaded.status == ProfileLoadStatus::Invalid)
            ARC_WARN("input: invalid Default binding profile for project {}", projectId.ToString());
        return true;
    }

    void LocalInputUser::Clear()
    {
        actions_ = InputActions::Create();
        asset_.reset();
        profile_ = {};
        rebind_ = {};
        lastSnapshot_ = {};
        projectId_ = Guid::Nil();
        profileRoot_.clear();
        profileName_ = "Default";
        reportedQueries_.clear();
    }

    void LocalInputUser::Update(double dt, const InputSnapshot& snapshot)
    {
        lastSnapshot_ = snapshot;
        if (!asset_) return;
        actions_->Update(dt, snapshot);
        if (rebind_.Result().state == InputRebindState::Waiting)
        {
            rebind_.Observe(snapshot, static_cast<float>(dt));
            if (rebind_.Result().state == InputRebindState::Completed &&
                !SetOverride(rebind_.Result().bindingId, rebind_.Result().replacementPath))
                ARC_WARN("input: captured path rejected for binding {}",
                         rebind_.Result().bindingId.ToString());
        }
    }

    void LocalInputUser::BeginFixedStep() { actions_->BeginFixedStep(); }

    std::optional<Guid> LocalInputUser::FindAction(std::string_view map,
                                                   std::string_view action) const
    { return actions_->FindAction(map, action); }
    InputActionValue LocalInputUser::Value(const Guid& action) const
    { return actions_->Value(action); }
    std::optional<bool> LocalInputUser::ButtonDown(const Guid& action) const
    {
        auto result = actions_->ButtonDown(action);
        if (!result) WarnQuery(action, "Button");
        return result;
    }
    std::optional<float> LocalInputUser::ScalarValue(const Guid& action) const
    {
        auto result = actions_->ScalarValue(action);
        if (!result) WarnQuery(action, "Axis1D");
        return result;
    }
    std::optional<glm::vec2> LocalInputUser::VectorValue(const Guid& action) const
    {
        auto result = actions_->VectorValue(action);
        if (!result) WarnQuery(action, "Axis2D");
        return result;
    }
    std::optional<InputActionPhase> LocalInputUser::Phase(const Guid& action) const
    {
        auto result = actions_->Phase(action);
        if (!result) WarnQuery(action, "Phase");
        return result;
    }
    bool LocalInputUser::Down(const Guid& action) const { return Value(action).down; }
    bool LocalInputUser::Pressed(const Guid& action) const { return actions_->Pressed(action); }
    bool LocalInputUser::Released(const Guid& action) const { return actions_->Released(action); }
    bool LocalInputUser::Started(const Guid& action) const { return actions_->Started(action); }
    bool LocalInputUser::Performed(const Guid& action) const { return actions_->Performed(action); }
    bool LocalInputUser::Canceled(const Guid& action) const { return actions_->Canceled(action); }
    bool LocalInputUser::PressedThisFixedStep(const Guid& action) const
    { return actions_->PressedThisFixedStep(action); }
    bool LocalInputUser::ReleasedThisFixedStep(const Guid& action) const
    { return actions_->ReleasedThisFixedStep(action); }
    std::span<const InputActionTransition> LocalInputUser::TransitionsThisFixedStep() const
    { return actions_->TransitionsThisFixedStep(); }

    std::optional<std::string> LocalInputUser::MapName(const Guid& map) const
    {
        for (const auto& info : actions_->Maps())
            if (info.id == map) return info.name;
        return std::nullopt;
    }
    bool LocalInputUser::SetBaseMap(const Guid& map)
    {
        const auto name = MapName(map);
        if (!name) return false;
        actions_->SetBaseContext(*name);
        return true;
    }
    bool LocalInputUser::PushMap(const Guid& map)
    {
        const auto name = MapName(map);
        if (!name) return false;
        actions_->PushContext(*name);
        return true;
    }
    void LocalInputUser::PopMap() { actions_->PopContext(); }
    bool LocalInputUser::SetControlScheme(std::string_view name)
    { return actions_->SetControlScheme(name); }
    std::vector<InputMapInfo> LocalInputUser::Maps() const { return actions_->Maps(); }
    std::vector<InputActionInfo> LocalInputUser::Actions(const Guid& map) const
    { return actions_->Actions(map); }
    std::vector<InputBindingInfo> LocalInputUser::Bindings(const Guid& action) const
    { return actions_->Bindings(action); }
    std::string LocalInputUser::BindingDisplayString(const Guid& binding) const
    { return actions_->BindingDisplayString(binding); }

    bool LocalInputUser::BeginRebind(const Guid& binding,
                                     std::optional<InputDevice> eligibleDevice,
                                     float timeoutSeconds)
    {
        if (!asset_ || !AuthoredPath(binding)) return false;
        rebind_.Begin(binding, eligibleDevice, timeoutSeconds, lastSnapshot_);
        return rebind_.Result().state == InputRebindState::Waiting;
    }
    void LocalInputUser::CancelRebind() { rebind_.Cancel(); }
    const InputRebindResult& LocalInputUser::RebindResult() const noexcept
    { return rebind_.Result(); }

    std::string LocalInputUser::SafeProfileName(std::string_view name)
    {
        if (name.empty() || name.size() > 64) return {};
        for (unsigned char ch : name)
            if (!std::isalnum(ch) && ch != '_' && ch != '-') return {};
        return std::string(name);
    }

    ProfileLoadResult LocalInputUser::LoadProfile(std::string_view name)
    {
        if (!asset_) return { ProfileLoadStatus::Invalid, { "no gameplay input asset" } };
        const auto safe = SafeProfileName(name);
        if (safe.empty()) return { ProfileLoadStatus::Invalid, { "invalid profile name" } };
        InputBindingProfile next;
        ProfileLoadResult result;
        if (!profileRoot_.empty())
            result = next.Load(profileRoot_ / (safe + ".json"), *asset_);
        if (result.status == ProfileLoadStatus::Invalid) return result;
        if (!actions_->LoadAsset(*asset_))
            return { ProfileLoadStatus::Invalid, { "gameplay asset failed to recompile" } };
        if (asset_->defaultMap) (void)SetBaseMap(*asset_->defaultMap);
        profile_ = std::move(next);
        profileName_ = safe;
        ApplyProfile();
        return result;
    }

    bool LocalInputUser::SaveProfile()
    {
        return asset_ && !profileRoot_.empty() &&
            profile_.Save(profileRoot_ / (profileName_ + ".json"));
    }
    ProfileLoadResult LocalInputUser::ImportProfile(const std::filesystem::path& path)
    {
        if (!asset_) return { ProfileLoadStatus::Invalid, { "no gameplay input asset" } };
        InputBindingProfile next = profile_;
        auto result = next.Import(path, *asset_);
        if (result.status == ProfileLoadStatus::Loaded)
        {
            if (!actions_->LoadAsset(*asset_))
                return { ProfileLoadStatus::Invalid, { "gameplay asset failed to recompile" } };
            if (asset_->defaultMap) (void)SetBaseMap(*asset_->defaultMap);
            profile_ = std::move(next);
            ApplyProfile();
        }
        return result;
    }
    bool LocalInputUser::ExportProfile(const std::filesystem::path& path) const
    { return asset_ && profile_.Export(path); }
    bool LocalInputUser::SetOverride(const Guid& binding, std::string_view path)
    {
        if (!asset_ || !profile_.SetOverride(binding, path, *asset_)) return false;
        return actions_->SetBindingPath(binding, path);
    }
    std::optional<std::string> LocalInputUser::AuthoredPath(const Guid& id) const
    {
        if (!asset_) return std::nullopt;
        for (const auto& map : asset_->actionMaps)
            for (const auto& action : map.actions)
                for (const auto& binding : action.bindings)
                {
                    if (binding.id == id && binding.composite.empty()) return binding.path;
                    for (const auto& part : binding.parts)
                        if (part.id == id) return part.path;
                }
        return std::nullopt;
    }
    bool LocalInputUser::RemoveOverride(const Guid& binding)
    {
        const auto authored = AuthoredPath(binding);
        if (!authored || !profile_.Overrides().contains(binding)) return false;
        if (!actions_->SetBindingPath(binding, *authored)) return false;
        return profile_.RemoveOverride(binding);
    }
    void LocalInputUser::ResetMapOverrides(const Guid& map)
    {
        for (const auto& action : Actions(map))
            for (const auto& binding : Bindings(action.id))
            {
                if (!binding.composite.empty())
                    for (const auto& part : binding.parts)
                        (void)RemoveOverride(part.id);
                else
                    (void)RemoveOverride(binding.id);
            }
    }
    void LocalInputUser::ResetOverrides()
    {
        if (!asset_) return;
        if (actions_->LoadAsset(*asset_) && asset_->defaultMap)
            (void)SetBaseMap(*asset_->defaultMap);
        profile_.Reset();
    }
    void LocalInputUser::ApplyProfile()
    {
        for (const auto& [binding, replacement] : profile_.Overrides())
            if (!actions_->SetBindingPath(binding, replacement))
                ARC_WARN("input: could not apply override for binding {}", binding.ToString());
    }
    void LocalInputUser::WarnQuery(const Guid& action, std::string_view requested) const
    {
        const std::string key = action.ToString() + ":" + std::string(requested);
        if (reportedQueries_.insert(key).second)
            ARC_WARN("input: invalid {} query for action {}", requested, action.ToString());
    }
}
