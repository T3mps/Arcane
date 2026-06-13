#pragma once

// Input module facade: Unity-style action maps -> actions (Button|Value)
// -> bindings (simple paths, '+' chords, 2DVector/1DAxis composites),
// evaluated once per frame from an InputSnapshot. Feature-parity port of
// the client oracle's core subset (Client/src/services/Input.lua);
// snapshot-driven architecture per the 2026-06-12 input-actions spec.
// Deferred: rebinding/persistence, glyphs, event-order lookups, replay.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <Json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Arcane
{
    enum class InputDevice : uint8_t { Kbm, Gamepad };

    class ARCANE_API InputActions
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

        // Reads + parses the file. Relative paths resolve against the exe
        // (ShaderLibrary pattern). False on missing/unreadable/malformed.
        virtual bool LoadFile(const std::filesystem::path& path) = 0;

        // Evaluates every map's actions from the snapshot. Once per frame,
        // before queries. dt feeds hold/tap interaction timing.
        virtual void Update(double dt, const InputSnapshot& snap) = 0;

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
