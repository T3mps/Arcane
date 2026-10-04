#pragma once

// Input module facade: Unity-style action maps -> actions (Button|Value)
// -> bindings (simple paths, "+<" chords, 2DVector/1DAxis composites),
// evaluated once per frame from an InputSnapshot. Feature-parity port of
// the client oracle's core subset (Client/src/services/Input.lua);
// snapshot-driven architecture per the 2026-06-12 input-actions spec.
// Deferred: rebinding/persistence, glyphs, event-order lookups, replay.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <Json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    enum class InputDevice : uint8_t { Kbm, Gamepad };

    enum class InputActionPhase : uint8_t { Waiting, Started, Performed, Canceled };

    struct InputActionValue
    {
        InputActionType type = InputActionType::Button;
        bool down = false;
        float scalar = 0.0f;
        glm::vec2 vector = { 0.0f, 0.0f };
        InputActionPhase phase = InputActionPhase::Waiting;
    };

    struct InputActionTransition
    {
        Guid action;
        InputActionPhase phase = InputActionPhase::Waiting;
        uint64_t sampleIndex = 0;
    };

    struct InputMapInfo
    {
        Guid id;
        std::string name;
        bool blocking = false;
        int priority = 0;
    };

    struct InputActionInfo
    {
        Guid id;
        Guid mapId;
        std::string name;
        InputActionType type = InputActionType::Button;
    };

    struct InputBindingPartInfo
    {
        Guid id;
        std::string name;
        std::string authoredPath;
        std::string effectivePath;
        std::vector<std::string> groups;
    };

    struct InputBindingInfo
    {
        Guid id;
        Guid actionId;
        std::string authoredPath;
        std::string effectivePath;
        std::string composite;
        std::vector<std::string> groups;
        std::vector<InputBindingPartInfo> parts;
    };

    // A control path's readable name with its device split out, for a UI that
    // draws the device as an icon: "<Gamepad>/buttonSouth" -> {"Gamepad",
    // "South Button"}, "<Keyboard>/scancode/a" -> {"Keyboard", "A"}. A path
    // the compiler cannot parse comes back as {"", path}. A chord whose parts
    // span devices has an empty device; each part names its own device in
    // `control` ("Keyboard Left Ctrl + Mouse Left Button"). Captured scancode
    // paths display with SDL's canonical name, resolved by the compiler's own
    // lookup ("<Keyboard>/scancode/left shift" -> "Left Shift").
    struct InputControlDisplay
    {
        std::string device;
        std::string control;
    };

    // One entry of KnownControls(): a simple path the compiler accepts.
    struct InputControlChoice
    {
        std::string path;
        InputControlDisplay display;
    };

    // The hold/tap interaction timings an undecorated "hold" / "tap" token
    // gets ("hold(duration=0.3)" overrides). ONE definition: the evaluator's
    // parser and the editor's readable interaction text both read these.
    inline constexpr float kDefaultHoldSeconds = 0.4f;
    inline constexpr float kDefaultTapSeconds = 0.2f;

    class ARC_API InputActions
    {
    public:
        static std::unique_ptr<InputActions> Create();
        virtual ~InputActions() = default;

        // Parses an input_actions.json document (actionMaps[] schema, the
        // Tools InputEditor shape). FULL REPLACE: previous maps and the
        // context stack are reset. Returns false on malformed or
        // schema-violating input (one ARC_WARN). Unknown device/control
        // path tokens compile to constant-zero bindings with one
        // load-time warn naming map/action/path.
        virtual bool LoadJson(const nlohmann::json& doc) = 0;

        // Compile a validated, versioned project asset. Stable IDs address
        // native actions and bindings; LoadJson remains the host-config path.
        virtual bool LoadAsset(const InputActionAsset& asset) = 0;
        virtual bool SetControlScheme(std::string_view schemeName) = 0;
        [[nodiscard]] virtual std::optional<Guid> FindAction(
            std::string_view map, std::string_view action) const = 0;
        [[nodiscard]] virtual std::optional<Guid> FindAction(std::string_view action) const = 0;
        [[nodiscard]] virtual InputActionValue Value(const Guid& action) const = 0;
        [[nodiscard]] virtual std::optional<bool> ButtonDown(const Guid& action) const = 0;
        [[nodiscard]] virtual std::optional<float> ScalarValue(const Guid& action) const = 0;
        [[nodiscard]] virtual std::optional<glm::vec2> VectorValue(const Guid& action) const = 0;
        [[nodiscard]] virtual std::optional<InputActionPhase> Phase(const Guid& action) const = 0;
        [[nodiscard]] virtual bool Pressed(const Guid& action) const = 0;
        [[nodiscard]] virtual bool Released(const Guid& action) const = 0;
        [[nodiscard]] virtual bool Started(const Guid& action) const = 0;
        [[nodiscard]] virtual bool Performed(const Guid& action) const = 0;
        [[nodiscard]] virtual bool Canceled(const Guid& action) const = 0;
        virtual bool SetBindingPath(const Guid& binding, std::string_view path) = 0;
        [[nodiscard]] virtual std::vector<InputMapInfo> Maps() const = 0;
        [[nodiscard]] virtual std::vector<InputActionInfo> Actions(const Guid& map) const = 0;
        [[nodiscard]] virtual std::vector<InputBindingInfo> Bindings(const Guid& action) const = 0;
        [[nodiscard]] virtual std::string BindingDisplayString(const Guid& binding) const = 0;

        // ---- control-path vocabulary (input-editor redesign spec s2.4) ----
        // Generated from the path compiler's OWN tables (LoveToSdlName, the
        // gamepad token tables, the mouse branch), never a hand-kept list, so
        // an editor picker built from it can never offer a path that compiles
        // to a constant-zero binding (hygiene pass 2026-09-28: five did).
        [[nodiscard]] static std::vector<InputControlChoice> KnownControls();
        // True when every "+<"-separated part compiles -- the exact test
        // LoadAsset applies, minus its warning.
        [[nodiscard]] static bool IsKnownControlPath(std::string_view path);
        // The compiled identity of a control path, spelling-independent: two
        // paths that drive the same physical control on the running layout
        // yield the same key ("<Mouse>/button/1" and "<Mouse>/leftButton",
        // "<Keyboard>/scancode/space" and "<Keyboard>/space", "a+b" and
        // "b+a"). Empty when any part fails to compile. The editor's conflict
        // detection compares THESE, never path strings: the rebind capture
        // writes the scancode form while assets author the keycode form.
        [[nodiscard]] static std::string CanonicalControlKey(std::string_view path);
        [[nodiscard]] static InputControlDisplay DisplayForPath(std::string_view path);
        // The binding's RAW value from the last Update's snapshot: 1/0 for a
        // button chord, the signed axis value, the max over a composite's
        // parts. 0 for an unknown id or before any Update. Feeds the editor's
        // live-preview glow; the runtime asks actions, not bindings.
        [[nodiscard]] virtual float BindingValue(const Guid& binding) const = 0;

        // Reads + parses the file. Relative paths resolve against the exe
        // (the engine-wide anchor). False on missing/unreadable/malformed.
        virtual bool LoadFile(const std::filesystem::path& path) = 0;

        // Evaluates every map's actions from the snapshot. Once per frame,
        // before queries. dt feeds hold/tap interaction timing.
        virtual void Update(double dt, const InputSnapshot& snap) = 0;
        virtual void BeginFixedStep() = 0;
        [[nodiscard]] virtual bool PressedThisFixedStep(const Guid& action) const = 0;
        [[nodiscard]] virtual bool ReleasedThisFixedStep(const Guid& action) const = 0;
        [[nodiscard]] virtual std::span<const InputActionTransition> TransitionsThisFixedStep() const = 0;

        // Context stack: queries resolve top-down; a 'blocking' map stops
        // fall-through. Unknown map names warn + no-op.
        virtual void PushContext(std::string_view map) = 0;
        virtual void PopContext() = 0;
        virtual void SetBaseContext(std::string_view map) = 0;  // resets stack
        virtual void SwapBaseContext(std::string_view map) = 0; // bottom only
        virtual std::string ActiveContext() const = 0;

        // Unresolvable actions return false / 0 / zero vector.
        virtual bool Down(std::string_view action) const = 0;      // held
        virtual bool Pressed(std::string_view action) const = 0;   // rising
        virtual bool Released(std::string_view action) const = 0;  // falling
        virtual bool Started(std::string_view action) const = 0;   // phase
        virtual bool Performed(std::string_view action) const = 0; // phase
        virtual bool Canceled(std::string_view action) const = 0;  // phase
        virtual float Strength(std::string_view action) const = 0;
        virtual glm::vec2 Axis(std::string_view action) const = 0;
        // Pressed within the last `frames` frames; consumes on success.
        virtual bool Buffered(std::string_view action, int frames = 6) = 0;

        virtual InputDevice ActiveDevice() const = 0;
    };
}
