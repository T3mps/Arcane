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
        if (!next->LoadAsset(asset)) return false;   // failure leaves the live session untouched
        if (asset_.has_value() && projectId == projectId_)
        {
            // RE-ENTRY (input-editor spec B 2.6): the editor saved THIS project's
            // asset while the session runs. Swap the evaluator in place and
            // carry the session across it.
            actions_ = std::move(next);
            asset_ = asset;
            // Overrides: a dirty profile is the user's unsaved work -- keep it
            // and re-apply ("compatible overrides": one whose binding id
            // vanished fails SetBindingPath and drops with ApplyProfile's WARN).
            // A clean profile is re-read against the NEW asset (Load drops
            // stale ids itself). Not LoadProfile(): that recompiles the
            // evaluator and resets the base context, undoing the replay below.
            if (!profile_.Dirty())
            {
                InputBindingProfile fresh;
                ProfileLoadResult loaded;
                if (!profileRoot_.empty())
                    loaded = fresh.Load(profileRoot_ / (profileName_ + ".json"), *asset_);
                if (loaded.status == ProfileLoadStatus::Invalid)
                    ARC_WARN("input: invalid {} binding profile for project {}", profileName_, projectId.ToString());
                else
                    profile_ = std::move(fresh);
            }
            ApplyProfile();
            // The scheme, while it still exists.
            if (!scheme_.empty() && !actions_->SetControlScheme(scheme_)) scheme_.clear();
            // Prime AFTER the overrides and the scheme (SetBindingPath zeroes
            // the action it retargets; the scheme filters which bindings
            // count): prev/cur both hold the held state, so the first live
            // tick sees no edge; dt = 0 adds no hold time; no map is on the
            // context stack yet (LoadAsset cleared it), so no fixed-step
            // transition is queued (UE: a key held across a mapping rebuild
            // is ignored until release).
            actions_->Update(0.0, lastSnapshot_);
            // Replay the map stack by id: the first id that still resolves is
            // the base, later ones are pushed, vanished ids are skipped; none
            // left -> the asset's default map.
            const std::vector<Guid> stack = mapStack_;
            mapStack_.clear();
            for (const Guid& map : stack)
            {
                if (mapStack_.empty()) (void)SetBaseMap(map);
                else (void)PushMap(map);
            }
            if (mapStack_.empty() && asset.defaultMap) (void)SetBaseMap(*asset.defaultMap);
            rebind_ = {};   // a capture armed against the old evaluator is void
            return true;
        }
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
        mapStack_.clear();
        scheme_.clear();
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
        mapStack_.assign(1, map);
        return true;
    }
    bool LocalInputUser::PushMap(const Guid& map)
    {
        const auto name = MapName(map);
        if (!name) return false;
        actions_->PushContext(*name);
        mapStack_.push_back(map);
        return true;
    }
    void LocalInputUser::PopMap()
    {
        actions_->PopContext();                          // pops whatever is on top, the base included
        if (!mapStack_.empty()) mapStack_.pop_back();
    }
    bool LocalInputUser::SetControlScheme(std::string_view name)
    {
        if (!actions_->SetControlScheme(name)) return false;
        scheme_.assign(name);                            // "" clears the scheme in both
        return true;
    }
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
        ForgetContextMirrors();
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
            ForgetContextMirrors();
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
        if (actions_->LoadAsset(*asset_))
        {
            ForgetContextMirrors();
            if (asset_->defaultMap) (void)SetBaseMap(*asset_->defaultMap);
        }
        profile_.Reset();
    }
    void LocalInputUser::ForgetContextMirrors()
    {
        // A recompile (LoadAsset -> LoadJson) empties the evaluator's context
        // stack and scheme group; the mirrors follow so ActiveMap() and
        // ControlScheme() never report state the evaluator dropped.
        mapStack_.clear();
        scheme_.clear();
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
