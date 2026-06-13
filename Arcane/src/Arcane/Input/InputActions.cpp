#include <Arcane/Input/InputActions.hpp>

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // ── exe-relative path resolution (Assets.cpp pattern) ───────────────
        std::filesystem::path ResolveInputPath(const std::filesystem::path& path)
        {
            if (path.is_absolute())
                return path;
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                return std::filesystem::path(modulePath).parent_path() / path;
#endif
            return path;
        }

        // ── local binary file reader (Assets.cpp pattern) ───────────────────
        std::vector<uint8_t> ReadInputFileBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return {};
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes((size_t)size);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
                return {};
            return bytes;
        }

        // ── ControlSource / ControlId ────────────────────────────────────────
        enum class ControlSource : uint8_t
        {
            None,
            Scancode,
            Keycode,
            MouseButton,
            GamepadButton,
            GamepadAxis,
            GamepadStick,   // resolved as a 2D vector (Task 3)
        };

        struct ControlId
        {
            ControlSource source = ControlSource::None;
            uint32_t code = 0;
        };

        // ── LOVE->SDL key name translation table ─────────────────────────────
        // LOVE names appear in the asset JSON; SDL name-lookup functions want
        // SDL names. Everything not in this table passes through unchanged
        // (single letters, digits, f1..f24 all match SDL names directly).
        const char* LoveToSdlName(const std::string& loveName)
        {
            struct Entry { const char* love; const char* sdl; };
            static constexpr Entry kTable[] = {
                { "lshift",    "Left Shift"  },
                { "rshift",    "Right Shift" },
                { "lctrl",     "Left Ctrl"   },
                { "rctrl",     "Right Ctrl"  },
                { "lalt",      "Left Alt"    },
                { "ralt",      "Right Alt"   },
                { "lgui",      "Left GUI"    },
                { "rgui",      "Right GUI"   },
                { "return",    "Return"      },
                { "escape",    "Escape"      },
                { "space",     "Space"       },
                { "tab",       "Tab"         },
                { "backspace", "Backspace"   },
                { "up",        "Up"          },
                { "down",      "Down"        },
                { "left",      "Left"        },
                { "right",     "Right"       },
            };
            for (const auto& e : kTable)
                if (loveName == e.love)
                    return e.sdl;
            return nullptr;  // pass through
        }

        // ── Gamepad token tables ─────────────────────────────────────────────
        // Token -> GamepadButton bit index (must match InputDevices bit order).
        // Task 1: these tables exist for compilation (unknown path handling),
        // but GamepadButton/Axis resolution itself returns 0 (snap.gamepadButtons /
        // snap.gamepadAxes access is implemented in Task 2). Storing the token
        // so at least the ControlId is properly typed.
        constexpr int kNoGamepadToken = -1;

        int GamepadButtonToken(const std::string& token)
        {
            struct Entry { const char* name; int bit; };
            static constexpr Entry kTable[] = {
                { "buttonSouth",      0  },
                { "buttonEast",       1  },
                { "buttonWest",       2  },
                { "buttonNorth",      3  },
                { "dpadUp",           4  },
                { "dpadDown",         5  },
                { "dpadLeft",         6  },
                { "dpadRight",        7  },
                { "leftShoulder",     8  },
                { "rightShoulder",    9  },
                { "start",            10 },
                { "back",             11 },
                { "guide",            12 },
                { "leftStickPress",   13 },
                { "rightStickPress",  14 },
            };
            for (const auto& e : kTable)
                if (token == e.name)
                    return e.bit;
            return kNoGamepadToken;
        }

        int GamepadAxisToken(const std::string& token)
        {
            struct Entry { const char* name; int idx; };
            static constexpr Entry kTable[] = {
                { "leftStick/x",   0 },
                { "leftStick/y",   1 },
                { "rightStick/x",  2 },
                { "rightStick/y",  3 },
                { "leftTrigger",   4 },
                { "rightTrigger",  5 },
            };
            for (const auto& e : kTable)
                if (token == e.name)
                    return e.idx;
            return kNoGamepadToken;
        }

        // Returns 0 for leftStick, 1 for rightStick, -1 if not a stick.
        int GamepadStickToken(const std::string& token)
        {
            if (token == "leftStick")  return 0;
            if (token == "rightStick") return 1;
            return kNoGamepadToken;
        }

        // ── Path compiler ─────────────────────────────────────────────────────
        // Compiles a single simple path (no '+') to a ControlId.
        // Returns {None,0} + one ARC_WARN for unknown device tokens.
        // <Gamepad>/ paths compile to a typed ControlId returning 0 until Task 2.
        ControlId CompileSinglePath(const std::string& path,
                                    const std::string& mapName,
                                    const std::string& actionName)
        {
            // Parse <Device>/ctrl
            if (path.empty())
                return {};
            if (path[0] != '<')
            {
                ARC_WARN("input: unknown control path '{}' in {}/{}", path, mapName, actionName);
                return {};
            }
            auto closeAngle = path.find('>');
            if (closeAngle == std::string::npos || closeAngle + 1 >= path.size() || path[closeAngle + 1] != '/')
            {
                ARC_WARN("input: unknown control path '{}' in {}/{}", path, mapName, actionName);
                return {};
            }
            std::string device = path.substr(1, closeAngle - 1);
            std::string ctrl   = path.substr(closeAngle + 2);  // after '<Device>/'

            if (device == "Keyboard")
            {
                // Check for scancode sub-path
                constexpr std::string_view kScanPrefix = "scancode/";
                if (ctrl.size() > kScanPrefix.size() &&
                    ctrl.substr(0, kScanPrefix.size()) == kScanPrefix)
                {
                    std::string scanName = ctrl.substr(kScanPrefix.size());
                    const char* sdlName = LoveToSdlName(scanName);
                    SDL_Scancode sc = SDL_GetScancodeFromName(sdlName ? sdlName : scanName.c_str());
                    if (sc == SDL_SCANCODE_UNKNOWN)
                    {
                        ARC_WARN("input: unknown scancode name '{}' in path '{}' in {}/{}",
                                 scanName, path, mapName, actionName);
                        return {};
                    }
                    return { ControlSource::Scancode, (uint32_t)sc };
                }
                else
                {
                    // Keycode path: translate LOVE name -> SDL name, then look up
                    const char* sdlName = LoveToSdlName(ctrl);
                    SDL_Keycode kc = SDL_GetKeyFromName(sdlName ? sdlName : ctrl.c_str());
                    if (kc == SDLK_UNKNOWN)
                    {
                        ARC_WARN("input: unknown key name '{}' in path '{}' in {}/{}",
                                 ctrl, path, mapName, actionName);
                        return {};
                    }
                    return { ControlSource::Keycode, (uint32_t)kc };
                }
            }
            else if (device == "Mouse")
            {
                // leftButton=bit0, rightButton=bit1, middleButton=bit2, button/N=bit(N-1)
                if (ctrl == "leftButton")   return { ControlSource::MouseButton, 0 };
                if (ctrl == "rightButton")  return { ControlSource::MouseButton, 1 };
                if (ctrl == "middleButton") return { ControlSource::MouseButton, 2 };
                // button/N form
                if (ctrl.size() > 7 && ctrl.substr(0, 7) == "button/")
                {
                    try
                    {
                        int n = std::stoi(ctrl.substr(7));
                        if (n >= 1 && n <= 5)
                            return { ControlSource::MouseButton, (uint32_t)(n - 1) };
                    }
                    catch (...) {}
                }
                ARC_WARN("input: unknown mouse control '{}' in path '{}' in {}/{}",
                         ctrl, path, mapName, actionName);
                return {};
            }
            else if (device == "Gamepad")
            {
                // Stick vector (leftStick / rightStick)
                int stick = GamepadStickToken(ctrl);
                if (stick >= 0)
                    return { ControlSource::GamepadStick, (uint32_t)stick };

                // Axis (leftTrigger, rightTrigger, leftStick/x, etc.)
                int axis = GamepadAxisToken(ctrl);
                if (axis >= 0)
                    return { ControlSource::GamepadAxis, (uint32_t)axis };

                // Button
                int btn = GamepadButtonToken(ctrl);
                if (btn >= 0)
                    return { ControlSource::GamepadButton, (uint32_t)btn };

                // Unknown gamepad token: warn + None
                ARC_WARN("input: unknown gamepad control '{}' in path '{}' in {}/{}",
                         ctrl, path, mapName, actionName);
                return {};
            }
            else
            {
                // Unknown device (e.g. <Wheel>)
                ARC_WARN("input: unknown control path '{}' in {}/{}", path, mapName, actionName);
                return {};
            }
        }

        // Compiles a possibly-chorded path ('+'-split) to a list of ControlIds.
        std::vector<ControlId> CompilePath(const std::string& path,
                                           const std::string& mapName,
                                           const std::string& actionName)
        {
            std::vector<ControlId> chord;
            // Split on '+'; handle the case that '<' is not a '+' separator
            // We split by '+' that is NOT inside '<...>'
            std::string part;
            bool inAngle = false;
            for (char c : path)
            {
                if (c == '<') inAngle = true;
                else if (c == '>') inAngle = false;
                else if (c == '+' && !inAngle)
                {
                    chord.push_back(CompileSinglePath(part, mapName, actionName));
                    part.clear();
                    continue;
                }
                part += c;
            }
            if (!part.empty())
                chord.push_back(CompileSinglePath(part, mapName, actionName));
            return chord;
        }

        // ── ResolveControl ────────────────────────────────────────────────────
        // Returns raw value [0,1] for buttons, signed float for axes.
        // Capture suppression lives HERE and only here (spec rule).
        float ResolveControl(const ControlId& id, const InputSnapshot& snap)
        {
            switch (id.source)
            {
            case ControlSource::Scancode:
                if (snap.wantCaptureKeyboard) return 0.0f;
                return snap.ScancodeDown(id.code) ? 1.0f : 0.0f;

            case ControlSource::Keycode:
                if (snap.wantCaptureKeyboard) return 0.0f;
                return snap.KeycodeDown(id.code) ? 1.0f : 0.0f;

            case ControlSource::MouseButton:
                if (snap.wantCaptureMouse) return 0.0f;
                return ((snap.mouseButtons >> id.code) & 1) ? 1.0f : 0.0f;

            case ControlSource::GamepadButton:
                if (!snap.gamepadConnected) return 0.0f;
                return ((snap.gamepadButtons >> id.code) & 1) ? 1.0f : 0.0f;

            case ControlSource::GamepadAxis:
                if (!snap.gamepadConnected) return 0.0f;
                if (id.code < 6) return snap.gamepadAxes[id.code];
                return 0.0f;

            case ControlSource::GamepadStick:
                // Sticks resolve to a vec2 via the composite/vector path
                // (the scalar ResolveControl path is not used for sticks).
                return 0.0f;

            case ControlSource::None:
            default:
                return 0.0f;
            }
        }

        // Resolve a chord: 1 only if every part magnitude >= threshold.
        // Returns the max magnitude for the whole chord if all down, else 0.
        // (oracle resolvePath chord logic)
        constexpr float kBtnThreshold = 0.5f;

        float ResolveChord(const std::vector<ControlId>& chord, const InputSnapshot& snap)
        {
            if (chord.empty()) return 0.0f;
            float minMag = 1.0f;
            for (const auto& id : chord)
            {
                float v = ResolveControl(id, snap);
                float mag = std::abs(v);
                if (mag < kBtnThreshold) return 0.0f;
                minMag = std::min(minMag, mag);
            }
            return minMag;
        }

        // ── Processor ops (Task 2 fully; stubs placed now for LoadJson) ──────
        struct ProcessorOp
        {
            enum class Kind { Invert, Scale, Deadzone, NormalizeVector2 } kind;
            float min    = 0.125f;
            float max    = 0.925f;
            float factor = 1.0f;
        };

        ProcessorOp ParseProcessorToken(const std::string& token)
        {
            ProcessorOp op;
            // Try to match name(args) form
            auto paren = token.find('(');
            std::string name = (paren == std::string::npos) ? token : token.substr(0, paren);
            std::string argStr = (paren == std::string::npos) ? "" : token.substr(paren + 1);
            if (!argStr.empty() && argStr.back() == ')') argStr.pop_back();

            if (name == "invert")
            {
                op.kind = ProcessorOp::Kind::Invert;
            }
            else if (name == "scale")
            {
                op.kind = ProcessorOp::Kind::Scale;
                // parse factor=N
                auto pos = argStr.find("factor=");
                if (pos != std::string::npos)
                {
                    try { op.factor = std::stof(argStr.substr(pos + 7)); }
                    catch (...) {}
                }
            }
            else if (name == "deadzone")
            {
                op.kind = ProcessorOp::Kind::Deadzone;
                // parse min=N,max=N
                auto minPos = argStr.find("min=");
                if (minPos != std::string::npos)
                {
                    try { op.min = std::stof(argStr.substr(minPos + 4)); }
                    catch (...) {}
                }
                auto maxPos = argStr.find("max=");
                if (maxPos != std::string::npos)
                {
                    try { op.max = std::stof(argStr.substr(maxPos + 4)); }
                    catch (...) {}
                }
            }
            else if (name == "normalizeVector2")
            {
                op.kind = ProcessorOp::Kind::NormalizeVector2;
            }
            else
            {
                // Unknown processor: warn at load time, evaluate as passthrough
                // (Scale by 1 is identity for both scalar and vector forms).
                ARC_WARN("input: unknown processor '{}' (ignored)", name);
                op.kind = ProcessorOp::Kind::Scale;
                op.factor = 1.0f;
            }
            return op;
        }

        // ── CompiledBinding ───────────────────────────────────────────────────
        struct CompiledBinding
        {
            std::vector<ControlId> chord;   // size 1 = simple path; size>1 = chord
            std::string path;               // original string (deferred features)
            std::vector<ProcessorOp> processors;  // Task 2
            bool isComposite = false;       // Task 3 fields join then
            // Task 3 composite fields (stubs; parsed at Task 3):
            std::string compositeType;      // "2DVector" or "1DAxis"
            // parts[part_name] = array of CompiledBinding
        };

        // ── Interaction ───────────────────────────────────────────────────────
        struct Interaction
        {
            enum class Kind { Press, Hold, Tap } kind = Kind::Press;
            float duration = 0.4f;
        };

        Interaction ParseInteraction(const std::string& token)
        {
            Interaction it;
            auto paren = token.find('(');
            std::string name = (paren == std::string::npos) ? token : token.substr(0, paren);
            std::string argStr = (paren == std::string::npos) ? "" : token.substr(paren + 1);
            if (!argStr.empty() && argStr.back() == ')') argStr.pop_back();

            if (name == "hold")
            {
                it.kind = Interaction::Kind::Hold;
                it.duration = 0.4f;
                auto pos = argStr.find("duration=");
                if (pos != std::string::npos)
                {
                    try { it.duration = std::stof(argStr.substr(pos + 9)); }
                    catch (...) {}
                }
            }
            else if (name == "tap")
            {
                it.kind = Interaction::Kind::Tap;
                it.duration = 0.2f;
                auto pos = argStr.find("duration=");
                if (pos != std::string::npos)
                {
                    try { it.duration = std::stof(argStr.substr(pos + 9)); }
                    catch (...) {}
                }
            }
            else
            {
                it.kind = Interaction::Kind::Press;
            }
            return it;
        }

        // ── Action ────────────────────────────────────────────────────────────
        struct Action
        {
            std::string name;
            std::string type;           // "Button" | "Value"
            std::string controlType;    // "Vector2" or empty

            std::vector<CompiledBinding> bindings;
            Interaction interaction;

            // Per-frame eval state
            bool    prevDown = false;
            bool    curDown  = false;
            float   strength = 0.0f;
            glm::vec2 vec    = { 0.0f, 0.0f };

            // Phase state
            bool    started   = false;
            bool    performed = false;
            bool    canceled  = false;
            bool    _perfFired = false;
            bool    _tapValid  = false;
            double  heldTime   = 0.0;

            // Buffered press state
            uint64_t lastPressFrame = 0;
            bool     bufConsumed    = true;

            // Device contribution tracking
            bool kbmContrib = false;
            bool padContrib = false;
        };

        // ── Map ───────────────────────────────────────────────────────────────
        struct Map
        {
            std::string name;
            bool blocking = false;
            std::unordered_map<std::string, Action> actions;
        };

        // ── Compile bindings from JSON ────────────────────────────────────────
        CompiledBinding CompileBinding(const nlohmann::json& bj,
                                       const std::string& mapName,
                                       const std::string& actionName)
        {
            CompiledBinding cb;

            // Check for composite
            if (bj.contains("composite") && bj["composite"].is_string())
            {
                cb.isComposite = true;
                cb.compositeType = bj["composite"].get<std::string>();
                // Task 3 will populate parts; for Task 1 just mark as composite.
                // Processors on composites
                if (bj.contains("processors") && bj["processors"].is_array())
                {
                    for (const auto& pt : bj["processors"])
                    {
                        if (pt.is_string())
                            cb.processors.push_back(ParseProcessorToken(pt.get<std::string>()));
                    }
                }
                return cb;
            }

            // Simple or chord path
            if (bj.contains("path") && bj["path"].is_string())
            {
                cb.path = bj["path"].get<std::string>();
                cb.chord = CompilePath(cb.path, mapName, actionName);
            }

            // Processors
            if (bj.contains("processors") && bj["processors"].is_array())
            {
                for (const auto& pt : bj["processors"])
                {
                    if (pt.is_string())
                        cb.processors.push_back(ParseProcessorToken(pt.get<std::string>()));
                }
            }

            return cb;
        }

        // ── InputActionsImpl ──────────────────────────────────────────────────
        class InputActionsImpl final : public InputActions
        {
        public:
            bool LoadJson(const nlohmann::json& doc) override
            {
                if (!doc.is_object() || !doc.contains("actionMaps") || !doc["actionMaps"].is_array())
                {
                    ARC_WARN("input: LoadJson failed: missing or invalid 'actionMaps' array");
                    return false;
                }

                m_maps.clear();
                m_contextStack.clear();
                m_frame = 0;

                for (const auto& mj : doc["actionMaps"])
                {
                    if (!mj.is_object()) continue;
                    std::string mapName = mj.contains("name") && mj["name"].is_string()
                                         ? mj["name"].get<std::string>() : "";
                    if (mapName.empty()) continue;

                    Map m;
                    m.name = mapName;
                    m.blocking = mj.contains("blocking") && mj["blocking"].is_boolean()
                                 ? mj["blocking"].get<bool>() : false;

                    if (mj.contains("actions") && mj["actions"].is_array())
                    {
                        for (const auto& aj : mj["actions"])
                        {
                            if (!aj.is_object()) continue;
                            std::string aName = aj.contains("name") && aj["name"].is_string()
                                                ? aj["name"].get<std::string>() : "";
                            if (aName.empty()) continue;

                            Action a;
                            a.name = aName;
                            a.type = aj.contains("type") && aj["type"].is_string()
                                     ? aj["type"].get<std::string>() : "Button";
                            a.controlType = aj.contains("controlType") && aj["controlType"].is_string()
                                            ? aj["controlType"].get<std::string>() : "";

                            // Interaction (first entry only, oracle pattern)
                            if (aj.contains("interactions") && aj["interactions"].is_array()
                                && !aj["interactions"].empty() && aj["interactions"][0].is_string())
                            {
                                a.interaction = ParseInteraction(aj["interactions"][0].get<std::string>());
                            }

                            // Bindings
                            if (aj.contains("bindings") && aj["bindings"].is_array())
                            {
                                for (const auto& bj : aj["bindings"])
                                {
                                    a.bindings.push_back(CompileBinding(bj, mapName, aName));
                                }
                            }

                            m.actions[aName] = std::move(a);
                        }
                    }

                    m_maps[mapName] = std::move(m);
                }

                return true;
            }

            bool LoadFile(const std::filesystem::path& path) override
            {
                auto resolved = ResolveInputPath(path);
                auto bytes = ReadInputFileBytes(resolved);
                if (bytes.empty())
                {
                    ARC_WARN("input: LoadFile failed: '{}' not found or empty",
                             resolved.string());
                    return false;
                }

                auto doc = nlohmann::json::parse(
                    bytes.begin(), bytes.end(),
                    /*cb=*/nullptr,
                    /*allow_exceptions=*/false);
                if (doc.is_discarded())
                {
                    ARC_WARN("input: LoadFile failed: JSON parse error in '{}'",
                             resolved.string());
                    return false;
                }

                return LoadJson(doc);
            }

            void Update(double dt, const InputSnapshot& snap) override
            {
                ++m_frame;

                bool kbmActive = false;
                float padMaxMag = 0.0f;

                for (auto& [mapName, m] : m_maps)
                {
                    for (auto& [aName, a] : m.actions)
                    {
                        EvalAction(a, dt, snap);

                        // Track device contribution for active-device hysteresis
                        if (a.kbmContrib) kbmActive = true;
                        if (a.padContrib) padMaxMag = std::max(padMaxMag, a.strength);
                    }
                }

                // Active device: kbm wins immediately; gamepad needs magnitude > 0.5
                if (kbmActive)
                    m_activeDevice = InputDevice::Kbm;
                else if (padMaxMag > kBtnThreshold)
                    m_activeDevice = InputDevice::Gamepad;
            }

            void PushContext(std::string_view map) override
            {
                std::string name(map);
                if (m_maps.find(name) == m_maps.end())
                {
                    ARC_WARN("input: PushContext: unknown map '{}'", name);
                    return;
                }
                m_contextStack.push_back(name);
            }

            void PopContext() override
            {
                if (!m_contextStack.empty())
                    m_contextStack.pop_back();
            }

            void SetBaseContext(std::string_view map) override
            {
                m_contextStack.clear();
                std::string name(map);
                if (m_maps.find(name) != m_maps.end())
                    m_contextStack.push_back(name);
            }

            void SwapBaseContext(std::string_view map) override
            {
                std::string name(map);
                if (m_maps.find(name) == m_maps.end())
                {
                    ARC_WARN("input: SwapBaseContext: unknown map '{}'", name);
                    return;
                }
                if (m_contextStack.empty())
                    m_contextStack.push_back(name);
                else
                    m_contextStack[0] = name;
            }

            std::string ActiveContext() const override
            {
                return m_contextStack.empty() ? "" : m_contextStack.back();
            }

            bool Down(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->curDown;
            }

            bool Pressed(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->curDown && !a->prevDown;
            }

            bool Released(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->prevDown && !a->curDown;
            }

            bool Started(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->started;
            }

            bool Performed(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->performed;
            }

            bool Canceled(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->canceled;
            }

            float Strength(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a ? a->strength : 0.0f;
            }

            glm::vec2 Axis(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a ? a->vec : glm::vec2(0.0f);
            }

            bool Buffered(std::string_view action, int frames) override
            {
                Action* a = ResolveMutable(action);
                if (!a || a->bufConsumed) return false;
                if ((int64_t)(m_frame - a->lastPressFrame) <= frames)
                {
                    a->bufConsumed = true;
                    return true;
                }
                return false;
            }

            InputDevice ActiveDevice() const override
            {
                return m_activeDevice;
            }

        private:
            std::unordered_map<std::string, Map> m_maps;
            std::vector<std::string> m_contextStack;
            uint64_t m_frame = 0;
            InputDevice m_activeDevice = InputDevice::Kbm;

            // Resolve action name through context stack top-down.
            // A blocking map stops fall-through (oracle resolve()).
            const Action* Resolve(std::string_view name) const
            {
                for (int i = (int)m_contextStack.size() - 1; i >= 0; --i)
                {
                    auto mit = m_maps.find(m_contextStack[i]);
                    if (mit == m_maps.end()) continue;
                    const Map& m = mit->second;
                    auto ait = m.actions.find(std::string(name));
                    if (ait != m.actions.end()) return &ait->second;
                    if (m.blocking) return nullptr;
                }
                return nullptr;
            }

            Action* ResolveMutable(std::string_view name)
            {
                for (int i = (int)m_contextStack.size() - 1; i >= 0; --i)
                {
                    auto mit = m_maps.find(m_contextStack[i]);
                    if (mit == m_maps.end()) continue;
                    Map& m = mit->second;
                    auto ait = m.actions.find(std::string(name));
                    if (ait != m.actions.end()) return &ait->second;
                    if (m.blocking) return nullptr;
                }
                return nullptr;
            }

            // Evaluate one action (oracle evalAction, button path).
            // Task 1: implements scalar bindings (Keyboard/Mouse) + button edges.
            // Composite bindings are skipped (isComposite=true) until Task 3.
            void EvalAction(Action& a, double dt, const InputSnapshot& snap)
            {
                bool isVec = (a.controlType == "Vector2");
                float bestScalar = 0.0f;
                float bestScalarMag = 0.0f;
                bool kbmContrib = false;
                bool padContrib = false;

                for (const auto& b : a.bindings)
                {
                    if (b.isComposite) continue;  // Task 3

                    float raw = ResolveChord(b.chord, snap);
                    // Apply processors (scalar, Task 2 fully; basic ops work now)
                    float val = ApplyScalarProcessors(b.processors, raw);

                    float mag = std::abs(val);
                    if (mag > bestScalarMag)
                    {
                        bestScalarMag = mag;
                        bestScalar = val;
                    }

                    // Device contribution tracking: check what sourced this binding
                    if (mag >= kBtnThreshold)
                    {
                        for (const auto& id : b.chord)
                        {
                            if (id.source == ControlSource::Scancode ||
                                id.source == ControlSource::Keycode  ||
                                id.source == ControlSource::MouseButton)
                            {
                                kbmContrib = true;
                            }
                            else if (id.source == ControlSource::GamepadButton ||
                                     id.source == ControlSource::GamepadAxis   ||
                                     id.source == ControlSource::GamepadStick)
                            {
                                padContrib = true;
                            }
                        }
                    }
                }

                a.prevDown = a.curDown;

                if (isVec)
                {
                    // Task 3: composites set this. For Task 1, scalar fallback.
                    a.vec      = glm::vec2(0.0f);
                    a.strength = bestScalarMag;
                    a.curDown  = bestScalarMag >= kBtnThreshold;
                }
                else
                {
                    a.vec      = glm::vec2(0.0f);
                    a.strength = bestScalar;
                    a.curDown  = bestScalarMag >= kBtnThreshold;
                }

                a.kbmContrib = kbmContrib;
                a.padContrib = padContrib;

                // Interaction phase logic (oracle evalAction)
                bool rising  = a.curDown && !a.prevDown;
                bool falling = a.prevDown && !a.curDown;

                if (rising)
                {
                    a.heldTime      = 0.0;
                    a._perfFired    = false;
                    a._tapValid     = true;
                    a.lastPressFrame = m_frame;
                    a.bufConsumed   = false;
                }
                else if (a.curDown)
                {
                    a.heldTime += dt;
                }

                a.started   = rising;
                a.performed = false;
                a.canceled  = false;

                const Interaction& it = a.interaction;
                if (it.kind == Interaction::Kind::Hold)
                {
                    if (a.curDown && !a._perfFired && a.heldTime >= (double)it.duration)
                    {
                        a.performed  = true;
                        a._perfFired = true;
                    }
                    else if (falling && !a._perfFired)
                    {
                        a.canceled = true;
                    }
                }
                else if (it.kind == Interaction::Kind::Tap)
                {
                    if (a.curDown && a.heldTime > (double)it.duration)
                        a._tapValid = false;
                    if (falling)
                    {
                        if (a._tapValid && a.heldTime <= (double)it.duration)
                            a.performed = true;
                        else
                            a.canceled = true;
                    }
                }
                else  // Press (default)
                {
                    a.performed = rising;
                    a.canceled  = falling;
                }
            }

            // Apply scalar processors in order (Task 2 extends this; basic ops work now).
            static float ApplyScalarProcessors(const std::vector<ProcessorOp>& procs, float v)
            {
                for (const auto& op : procs)
                {
                    switch (op.kind)
                    {
                    case ProcessorOp::Kind::Invert:
                        v = -v;
                        break;
                    case ProcessorOp::Kind::Scale:
                        v *= op.factor;
                        break;
                    case ProcessorOp::Kind::Deadzone:
                    {
                        float m = std::abs(v);
                        float sgn = (v < 0.0f) ? -1.0f : 1.0f;
                        if (m < op.min) v = 0.0f;
                        else if (m > op.max) v = sgn;
                        else v = sgn * (m - op.min) / (op.max - op.min);
                        break;
                    }
                    case ProcessorOp::Kind::NormalizeVector2:
                        // Scalar NormalizeVector2 is a no-op on scalars (Task 3 handles vectors)
                        break;
                    }
                }
                return v;
            }
        };

    }  // anonymous namespace

    std::unique_ptr<InputActions> InputActions::Create()
    {
        return std::make_unique<InputActionsImpl>();
    }

}  // namespace Arcane
