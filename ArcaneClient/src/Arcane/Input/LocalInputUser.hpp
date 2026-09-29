#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputBindingProfile.hpp>
#include <Arcane/Input/InputRebindOperation.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Arcane
{
    // One gameplay input session for one running client. The host keeps its raw
    // snapshot and editor shortcut evaluator independently of this object.
    class ARCANE_API LocalInputUser
    {
    public:
        LocalInputUser();

        [[nodiscard]] bool Configure(const InputActionAsset& asset, const Guid& projectId);
        void Clear();
        void Update(double dt, const InputSnapshot& snapshot);
        void BeginFixedStep();
        [[nodiscard]] Guid ProjectId() const noexcept { return projectId_; }
        [[nodiscard]] bool Configured() const noexcept { return asset_.has_value(); }

        [[nodiscard]] std::optional<Guid> FindAction(std::string_view map,
                                                     std::string_view action) const;
        [[nodiscard]] InputActionValue Value(const Guid& action) const;
        [[nodiscard]] std::optional<bool> ButtonDown(const Guid& action) const;
        [[nodiscard]] std::optional<float> ScalarValue(const Guid& action) const;
        [[nodiscard]] std::optional<glm::vec2> VectorValue(const Guid& action) const;
        [[nodiscard]] std::optional<InputActionPhase> Phase(const Guid& action) const;
        [[nodiscard]] bool Down(const Guid& action) const;
        [[nodiscard]] bool Pressed(const Guid& action) const;
        [[nodiscard]] bool Released(const Guid& action) const;
        [[nodiscard]] bool Started(const Guid& action) const;
        [[nodiscard]] bool Performed(const Guid& action) const;
        [[nodiscard]] bool Canceled(const Guid& action) const;
        [[nodiscard]] bool PressedThisFixedStep(const Guid& action) const;
        [[nodiscard]] bool ReleasedThisFixedStep(const Guid& action) const;
        [[nodiscard]] std::span<const InputActionTransition> TransitionsThisFixedStep() const;

        [[nodiscard]] bool SetBaseMap(const Guid& map);
        [[nodiscard]] bool PushMap(const Guid& map);
        void PopMap();
        [[nodiscard]] bool SetControlScheme(std::string_view name);
        // The map on top of the stack: the base map when nothing is pushed,
        // nullopt before Configure. A same-project re-Configure replays it.
        [[nodiscard]] std::optional<Guid> ActiveMap() const noexcept
        { return mapStack_.empty() ? std::nullopt : std::optional<Guid>{ mapStack_.back() }; }
        // The active control scheme ("" = none): re-applied by a same-project
        // re-Configure, cleared when the scheme no longer exists or on a cold start.
        [[nodiscard]] std::string_view ControlScheme() const noexcept { return scheme_; }
        [[nodiscard]] std::vector<InputMapInfo> Maps() const;
        [[nodiscard]] std::vector<InputActionInfo> Actions(const Guid& map) const;
        [[nodiscard]] std::vector<InputBindingInfo> Bindings(const Guid& action) const;
        [[nodiscard]] std::string BindingDisplayString(const Guid& binding) const;

        [[nodiscard]] bool BeginRebind(const Guid& binding,
                                       std::optional<InputDevice> eligibleDevice,
                                       float timeoutSeconds);
        void CancelRebind();
        [[nodiscard]] const InputRebindResult& RebindResult() const noexcept;

        [[nodiscard]] ProfileLoadResult LoadProfile(std::string_view name);
        [[nodiscard]] bool SaveProfile();
        [[nodiscard]] ProfileLoadResult ImportProfile(const std::filesystem::path& path);
        [[nodiscard]] bool ExportProfile(const std::filesystem::path& path) const;
        [[nodiscard]] bool SetOverride(const Guid& binding, std::string_view path);
        [[nodiscard]] bool RemoveOverride(const Guid& binding);
        void ResetMapOverrides(const Guid& map);
        void ResetOverrides();
        [[nodiscard]] bool ProfileDirty() const noexcept { return profile_.Dirty(); }

    private:
        [[nodiscard]] std::optional<std::string> MapName(const Guid& map) const;
        [[nodiscard]] std::optional<std::string> AuthoredPath(const Guid& binding) const;
        void ForgetContextMirrors();
        void ApplyProfile();
        void WarnQuery(const Guid& action, std::string_view requested) const;
        [[nodiscard]] static std::string SafeProfileName(std::string_view name);

        std::unique_ptr<InputActions> actions_;
        std::optional<InputActionAsset> asset_;
        InputBindingProfile profile_;
        InputRebindOperation rebind_;
        InputSnapshot lastSnapshot_;
        std::vector<Guid> mapStack_;   // SetBaseMap = {map}; PushMap appends on success; PopMap pops. Replayed by a same-project re-Configure.
        std::string scheme_;           // the last SetControlScheme that succeeded ("" = none); re-applied by a same-project re-Configure
        Guid projectId_;
        std::filesystem::path profileRoot_;
        std::string profileName_ = "Default";
        mutable std::unordered_set<std::string> reportedQueries_;
    };
}
