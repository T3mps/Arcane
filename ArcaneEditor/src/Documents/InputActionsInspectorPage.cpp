#include "Documents/InputActionsInspectorPage.hpp"

#include "Widgets/IconsLucide.h"
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Input/InputActions.hpp>   // KnownControls, DisplayForPath, kDefaultHoldSeconds / kDefaultTapSeconds

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>   // std::strtof (interaction / processor parameters)
#include <map>       // perGroup (DrawAction's Bindings section)

namespace Arcane::Editor
{
    namespace
    {
        Guid IdOf(const nlohmann::json& row)
        {
            if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) return {};
            return Guid::FromString(row["id"].get<std::string>()).value_or(Guid{});
        }
        std::string Str(const nlohmann::json& row, const char* key)
        { return row.is_object() && row.contains(key) && row[key].is_string() ? row[key].get<std::string>() : std::string{}; }
        // Tolerant scalar reads: json::value() THROWS when the key holds another
        // type, and the draft is whatever the user last typed (a document opens
        // malformed sources for repair) -- the Inspector must never crash on it.
        bool Bool(const nlohmann::json& row, const char* key, bool fallback)
        { return row.is_object() && row.contains(key) && row[key].is_boolean() ? row[key].get<bool>() : fallback; }
        int Int(const nlohmann::json& row, const char* key, int fallback)
        { return row.is_object() && row.contains(key) && row[key].is_number_integer() ? row[key].get<int>() : fallback; }
        const nlohmann::json* Find(const nlohmann::json& node, const Guid& id)
        {
            if (!id.IsValid()) return nullptr;
            if (node.is_object())
            {
                if (IdOf(node) == id) return &node;
                for (const auto& [k, child] : node.items()) if (const auto* m = Find(child, id)) return m;
            }
            else if (node.is_array())
                for (const auto& child : node) if (const auto* m = Find(child, id)) return m;
            return nullptr;
        }
        std::vector<std::string> Groups(const nlohmann::json& row)
        {
            std::vector<std::string> out;
            if (row.is_object() && row.contains("groups") && row["groups"].is_array())
                for (const auto& g : row["groups"]) if (g.is_string()) out.push_back(g.get<std::string>());
            return out;
        }
        std::string Joined(const nlohmann::json& arr)
        {
            std::string s;
            if (arr.is_array()) for (const auto& e : arr) if (e.is_string()) { if (!s.empty()) s += ", "; s += e.get<std::string>(); }
            return s;
        }
        nlohmann::json Split(const std::string& csv)
        {
            nlohmann::json out = nlohmann::json::array();
            std::size_t start = 0;
            while (start <= csv.size())
            {
                const std::size_t comma = csv.find(',', start);
                std::string tok = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const auto b = tok.find_first_not_of(" \t"), e = tok.find_last_not_of(" \t");
                if (b != std::string::npos) out.push_back(tok.substr(b, e - b + 1));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            return out;
        }
        // The TARGET's id scope around a Rows region. RAII and declared BEFORE
        // the Rows, so the table ends (Rows' destructor -> EndTable) before the
        // id pops: a PopID ahead of EndTable pops the TABLE's id and ImGui
        // asserts "Mismatching PushID/PopID!" (imgui_tables.cpp EndTable).
        struct TargetIdScope
        {
            explicit TargetIdScope(const Guid& id) { ImGui::PushID(id.ToString().c_str()); }
            ~TargetIdScope() { ImGui::PopID(); }
            TargetIdScope(const TargetIdScope&) = delete;
            TargetIdScope& operator=(const TargetIdScope&) = delete;
        };
        constexpr const char* kTypes[] = { "Button", "Axis1D", "Axis2D" };
        constexpr const char* kInteractions[] = { "--", "Press", "Hold", "Tap" };
    }

    InputActionsInspectorPage::InputActionsInspectorPage(InputActionsEditorModel& model, std::string assetName,
                                                         std::string assetPath, Services services)
        : model_(model), assetName_(std::move(assetName)), assetPath_(std::move(assetPath)), services_(std::move(services)) {}

    std::vector<InspectorCrumb> InputActionsInspectorPage::Breadcrumb() const
    {
        // `select` re-selects in the model (an unpinned instance follows);
        // `key` is that level's selection key in the model's 4-segment format
        // -- three slashes ALWAYS, PageFor rejects fewer -- so a PINNED instance
        // re-targets its own pin (InspectorHost::RepinKey) and never touches
        // the source. The asset root's key is "": PageFor("") IS the asset page.
        std::vector<InspectorCrumb> crumbs;
        crumbs.push_back({ assetName_, [m = &model_] { m->SelectMap({}); }, std::optional<std::string>{ std::string{} } });
        const auto& draft = model_.Draft();
        if (const auto* map = Find(draft, sel_.map))
        {
            const std::string mapKey = sel_.map.ToString();
            crumbs.push_back({ Str(*map, "name"), [m = &model_, id = sel_.map] { m->SelectMap(id); },
                               std::optional<std::string>{ mapKey + "///" } });
            if (const auto* action = Find(*map, sel_.action))
            {
                const std::string actionKey = mapKey + "/" + sel_.action.ToString();
                crumbs.push_back({ Str(*action, "name"), [m = &model_, map = sel_.map, id = sel_.action]
                                   { m->SelectMap(map); m->SelectAction(id); },
                                   std::optional<std::string>{ actionKey + "//" } });
                if (const auto* binding = Find(*action, sel_.binding))
                {
                    const std::string bindingKey = actionKey + "/" + sel_.binding.ToString();
                    const std::string label = binding->contains("composite")
                        ? (Str(*binding, "composite") == "1DAxis" ? "1D Axis" : "2D Vector")
                        : InputActions::DisplayForPath(Str(*binding, "path")).control;
                    crumbs.push_back({ label, [m = &model_, map = sel_.map, a = sel_.action, id = sel_.binding]
                                       { m->SelectMap(map); m->SelectAction(a); m->SelectBinding(id); },
                                       std::optional<std::string>{ bindingKey + "/" } });
                    if (const auto* part = Find(*binding, sel_.part))
                        crumbs.push_back({ Str(*part, "name") + " · " + InputActions::DisplayForPath(Str(*part, "path")).control,
                                           [m = &model_, map = sel_.map, a = sel_.action, b = sel_.binding, id = sel_.part]
                                           { m->SelectMap(map); m->SelectAction(a); m->SelectBinding(b); m->SelectPart(id); },
                                           std::optional<std::string>{ bindingKey + "/" + sel_.part.ToString() } });
                }
            }
        }
        return crumbs;
    }

    void InputActionsInspectorPage::Draw(PropertyGrid& grid)
    {
        edit_ = nullptr;
        drawing_ = true;   // Defer() queues until the end of this call
        const auto& draft = model_.Draft();
        const auto* map = Find(draft, sel_.map);
        const auto* action = map ? Find(*map, sel_.action) : nullptr;
        const auto* binding = action ? Find(*action, sel_.binding) : nullptr;
        const auto* part = binding ? Find(*binding, sel_.part) : nullptr;
        if (part) DrawBinding(grid, *part, true);
        else if (binding) DrawBinding(grid, *binding, false);
        else if (action) DrawAction(grid, *action);
        else if (map) DrawMap(grid, *map);
        else DrawAsset(grid);
        drawing_ = false;
        if (edit_) edit_();   // AFTER the draw: the draft must not mutate under the rows above
    }

    void InputActionsInspectorPage::DrawAsset(PropertyGrid& grid)
    {
        const auto& draft = model_.Draft();
        if (grid.Section("Asset"))
        {
            PropertyGrid::Rows rows(grid, "##asset");
            if (rows)
            {
                grid.ReadOnlyRow("Name", assetName_);
                grid.ReadOnlyRow("Path", assetPath_);
                std::vector<std::string> mapNames; std::vector<const char*> items; int current = -1; std::vector<Guid> ids;
                if (draft.is_object() && draft.contains("actionMaps") && draft["actionMaps"].is_array())
                    for (const auto& m : draft["actionMaps"]) { mapNames.push_back(Str(m, "name")); ids.push_back(IdOf(m)); }
                for (const auto& n : mapNames) items.push_back(n.c_str());
                const std::string def = Str(draft, "defaultMap");
                for (std::size_t i = 0; i < ids.size(); ++i) if (ids[i].ToString() == def) current = static_cast<int>(i);
                if (const int picked = grid.ComboRow("Default map", items.data(), static_cast<int>(items.size()), current); picked >= 0)
                    edit_ = [m = &model_, id = ids[static_cast<std::size_t>(picked)]] { (void)m->SetDefaultMap(id); };
                std::size_t actions = 0, bindings = 0;
                if (draft.is_object() && draft.contains("actionMaps") && draft["actionMaps"].is_array())
                    for (const auto& m : draft["actionMaps"]) if (m.contains("actions") && m["actions"].is_array())
                        for (const auto& a : m["actions"]) { ++actions; if (a.contains("bindings") && a["bindings"].is_array()) bindings += a["bindings"].size(); }
                grid.ReadOnlyRow("Contents", std::to_string(ids.size()) + " maps · " + std::to_string(actions) + " actions · " + std::to_string(bindings) + " bindings");
            }
        }
        if (grid.Section("Control schemes"))
        {
            PropertyGrid::Rows rows(grid, "##schemes");
            if (rows && draft.is_object() && draft.contains("controlSchemes") && draft["controlSchemes"].is_array())
            {
                // Two schemes may share a name: the row id is the scheme's guid,
                // never its label (ImGui's id-conflict detection would paint the
                // twins red and keyboard activation would toggle both).
                for (const auto& s : draft["controlSchemes"])
                {
                    ImGui::PushID(IdOf(s).ToString().c_str());
                    grid.ReadOnlyRow(Str(s, "name").c_str(), "group " + Str(s, "bindingGroup"));
                    ImGui::PopID();
                }
                if (draft["controlSchemes"].empty()) grid.ReadOnlyRow("(none)", "add one from the toolbar's scheme combo");
            }
        }
    }

    // Row ids are scoped by the TARGET (TargetIdScope around every Rows scope,
    // INSIDE the Section so section open-state stays shared per page kind):
    // a TextRow draft is keyed by ImGui id and would otherwise be shared by
    // every map/action/binding that draws a "Name" row (PropertyGrid rule).
    // TextRow commits capture BY VALUE plus the liveness token -- never [&]:
    // the draft STORES the callable and may run it after this frame's locals
    // (or this page) are gone; `this` is dereferenced only after the token is
    // checked. Target validation is the model's: SetField -> FindId returns
    // false for a deleted id, and ApplyEdit's before == after guard makes a
    // duplicate delivery a no-op.
    void InputActionsInspectorPage::DrawMap(PropertyGrid& grid, const nlohmann::json& map)
    {
        const Guid id = IdOf(map);
        if (grid.Section("Action map"))
        {
            const TargetIdScope target(id);   // declared BEFORE rows: EndTable runs first, then PopID
            PropertyGrid::Rows rows(grid, "##map");
            if (rows)
            {
                grid.TextRow("Name", Str(map, "name"), [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                             { if (w.expired()) return; Defer([m = &model_, id, v] { (void)m->SetField(id, "name", v); }); });
                bool blocking = Bool(map, "blocking", false);
                if (grid.CheckboxRow("Blocking", blocking)) edit_ = [m = &model_, id, blocking] { (void)m->SetField(id, "blocking", blocking); };
                int priority = Int(map, "priority", 0);
                if (grid.IntRow("Priority", priority)) edit_ = [m = &model_, id, priority] { (void)m->SetField(id, "priority", priority); };
            }
        }
        if (grid.Section("Contents"))
        {
            const TargetIdScope target(id);   // declared BEFORE rows: EndTable runs first, then PopID
            PropertyGrid::Rows rows(grid, "##contents");
            if (rows)
            {
                std::size_t actions = 0, bindings = 0;
                if (map.contains("actions") && map["actions"].is_array())
                    for (const auto& a : map["actions"]) { ++actions; if (a.contains("bindings") && a["bindings"].is_array()) bindings += a["bindings"].size(); }
                grid.ReadOnlyRow("Actions", std::to_string(actions));
                grid.ReadOnlyRow("Bindings", std::to_string(bindings));
            }
        }
    }

    void InputActionsInspectorPage::DrawAction(PropertyGrid& grid, const nlohmann::json& action)
    {
        const Guid id = IdOf(action);
        if (grid.Section("Action"))
        {
            const TargetIdScope target(id);   // declared BEFORE rows: EndTable runs first, then PopID
            PropertyGrid::Rows rows(grid, "##action");
            if (rows)
            {
                grid.TextRow("Name", Str(action, "name"), [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                             { if (w.expired()) return; Defer([m = &model_, id, v] { (void)m->SetField(id, "name", v); }); });
                const std::string type = Str(action, "type");
                int current = 0; for (int i = 0; i < 3; ++i) if (type == kTypes[i]) current = i;
                if (const int picked = grid.ComboRow("Type", kTypes, 3, current); picked >= 0)
                    edit_ = [m = &model_, id, picked] { (void)m->SetField(id, "type", kTypes[picked]); };
                // Interaction: the first token decides the combo; Hold/Tap carry a
                // duration. An undecorated token takes the ENGINE's default
                // (kDefaultHoldSeconds / kDefaultTapSeconds, InputActions.hpp):
                // one definition shared with the evaluator's parser.
                const nlohmann::json interactions = action.value("interactions", nlohmann::json::array());
                std::string first = interactions.is_array() && !interactions.empty() && interactions[0].is_string() ? interactions[0].get<std::string>() : "";
                const std::string name = first.substr(0, first.find('('));
                int kind = name == "press" ? 1 : name == "hold" ? 2 : name == "tap" ? 3 : 0;
                float seconds = kind == 2 ? kDefaultHoldSeconds : kDefaultTapSeconds;
                if (const auto d = first.find("duration="); d != std::string::npos) seconds = std::strtof(first.c_str() + d + 9, nullptr);
                auto compose = [](int k, float s) -> nlohmann::json {
                    char buf[48];
                    if (k == 1) return nlohmann::json::array({ "press" });
                    if (k == 2) { std::snprintf(buf, sizeof buf, "hold(duration=%.2f)", s); return nlohmann::json::array({ buf }); }
                    if (k == 3) { std::snprintf(buf, sizeof buf, "tap(duration=%.2f)", s); return nlohmann::json::array({ buf }); }
                    return nlohmann::json::array(); };
                if (const int picked = grid.ComboRow("Interaction", kInteractions, 4, kind); picked >= 0)
                    edit_ = [m = &model_, id, v = compose(picked, picked == 2 ? kDefaultHoldSeconds : kDefaultTapSeconds)]
                            { (void)m->SetField(id, "interactions", v); };
                if (kind == 2 || kind == 3)
                    if (grid.FloatRow("Seconds", seconds, 0.01f))
                        edit_ = [m = &model_, id, v = compose(kind, seconds)] { (void)m->SetField(id, "interactions", v); };
                grid.TextRow("Processors", Joined(action.value("processors", nlohmann::json::array())),
                             [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                             { if (w.expired()) return; Defer([m = &model_, id, arr = Split(v)] { (void)m->SetField(id, "processors", arr); }); });
            }
        }
        if (grid.Section("Bindings"))
        {
            const TargetIdScope target(id);   // declared BEFORE rows: EndTable runs first, then PopID
            PropertyGrid::Rows rows(grid, "##bindings");
            if (rows)
            {
                std::map<std::string, int> perGroup; int ungrouped = 0;
                if (action.contains("bindings") && action["bindings"].is_array())
                    for (const auto& b : action["bindings"]) { const auto g = Groups(b); if (g.empty()) ++ungrouped; for (const auto& n : g) ++perGroup[n]; }
                for (const auto& [g, n] : perGroup) grid.ReadOnlyRow(g.c_str(), std::to_string(n));
                grid.ReadOnlyRow("Ungrouped", std::to_string(ungrouped));
            }
        }
        DrawLivePreview(grid, id);
    }

    void InputActionsInspectorPage::DrawBinding(PropertyGrid& grid, const nlohmann::json& row, bool isPart)
    {
        const Guid id = IdOf(row);
        const bool composite = row.contains("composite");
        if (grid.Section(isPart ? "Composite part" : composite ? "Composite" : "Binding"))
        {
            const TargetIdScope target(id);   // declared BEFORE rows: EndTable runs first, then PopID
            PropertyGrid::Rows rows(grid, "##binding");
            if (rows)
            {
                if (composite)
                    grid.ReadOnlyRow("Type", Str(row, "composite") == "1DAxis" ? "1D Axis" : "2D Vector");
                else
                {
                    const std::string path = Str(row, "path");
                    grid.ReadOnlyRow("Control", InputActions::DisplayForPath(path).control);
                    // The raw path, editable: a monospace face is not installed
                    // (EditorFonts.hpp has Inter/Roboto/brand only) -- owed.
                    grid.TextRow("Path", path, [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                                 { if (w.expired()) return; Defer([m = &model_, id, v] { (void)m->SetField(id, "path", v); }); });
                    static const char* const kButtons[] = { "Rebind...", "Pick..." };
                    const int clicked = grid.ButtonRow("", kButtons, 2);
                    if (clicked == 0 && services_.beginRebind) services_.beginRebind(id);
                    if (clicked == 1) ImGui::OpenPopup("##input_pick");
                    DrawPicker(id);
                }
                if (isPart) grid.ReadOnlyRow("Role", Str(row, "name"));
            }
        }
        if (grid.Section("Control schemes"))
        {
            const TargetIdScope target(id);   // declared BEFORE rows: EndTable runs first, then PopID
            PropertyGrid::Rows rows(grid, "##groups");
            if (rows)
            {
                const auto groups = Groups(row);
                const auto& draft = model_.Draft();
                if (draft.is_object() && draft.contains("controlSchemes") && draft["controlSchemes"].is_array())
                    for (const auto& s : draft["controlSchemes"])
                    {
                        // Keyed by the scheme's guid, not its name: two schemes
                        // may share a name (see DrawAsset).
                        ImGui::PushID(IdOf(s).ToString().c_str());
                        const std::string group = Str(s, "bindingGroup");
                        bool on = std::find(groups.begin(), groups.end(), group) != groups.end();
                        if (grid.CheckboxRow(Str(s, "name").c_str(), on))
                        {
                            nlohmann::json next = nlohmann::json::array();
                            for (const auto& g : groups) if (g != group) next.push_back(g);
                            if (on) next.push_back(group);
                            edit_ = [m = &model_, id, next] { (void)m->SetField(id, "groups", next); };
                        }
                        ImGui::PopID();
                    }
                if (groups.empty()) grid.ReadOnlyRow("(ungrouped)", "applies in every scheme");
            }
        }
        if (!composite && grid.Section("Processors"))
        {
            const TargetIdScope target(id);   // declared BEFORE rows: EndTable runs first, then PopID
            PropertyGrid::Rows rows(grid, "##processors");
            if (rows)
            {
                const nlohmann::json procs = row.value("processors", nlohmann::json::array());
                bool invert = false; float scale = 1.0f; bool hasScale = false;
                if (procs.is_array()) for (const auto& p : procs) if (p.is_string())
                {
                    const std::string t = p.get<std::string>();
                    if (t.rfind("invert", 0) == 0) invert = true;
                    if (t.rfind("scale", 0) == 0) { hasScale = true; if (const auto f = t.find("factor="); f != std::string::npos) scale = std::strtof(t.c_str() + f + 7, nullptr); }
                }
                auto rebuild = [&](bool inv, bool sc, float factor) {
                    nlohmann::json next = nlohmann::json::array();
                    if (procs.is_array()) for (const auto& p : procs) if (p.is_string()) { const auto t = p.get<std::string>(); if (t.rfind("invert", 0) != 0 && t.rfind("scale", 0) != 0) next.push_back(t); }
                    if (inv) next.push_back("invert");
                    if (sc) { char buf[40]; std::snprintf(buf, sizeof buf, "scale(factor=%.3f)", factor); next.push_back(buf); }
                    return next; };
                if (grid.CheckboxRow("Invert", invert)) edit_ = [m = &model_, id, v = rebuild(invert, hasScale, scale)] { (void)m->SetField(id, "processors", v); };
                if (grid.FloatRow("Scale", scale, 0.01f)) edit_ = [m = &model_, id, v = rebuild(invert, true, scale)] { (void)m->SetField(id, "processors", v); };
                grid.ReadOnlyRow("Raw", Joined(procs));
            }
        }
        DrawLivePreview(grid, sel_.action);
    }

    void InputActionsInspectorPage::DrawLivePreview(PropertyGrid& grid, const Guid& action)
    {
        const bool armed = services_.state && services_.state->previewArmed && services_.preview;
        if (!grid.Section("Live preview")) return;
        PropertyGrid::Rows rows(grid, "##live");
        if (!rows) return;
        if (!armed) { grid.ReadOnlyRow("Phase", "turn on Preview in the document's toolbar"); return; }
        const InputActionValue v = services_.preview->Value(action);
        const char* phase = v.phase == InputActionPhase::Started ? "Started" : v.phase == InputActionPhase::Performed ? "Performed"
                          : v.phase == InputActionPhase::Canceled ? "Canceled" : "Waiting";
        grid.ReadOnlyRow("Phase", phase);
        grid.ReadOnlyRow("Device", services_.preview->ActiveDevice() == InputDevice::Gamepad ? "Gamepad" : "Keyboard / Mouse");
        char overlay[48];
        if (v.type == InputActionType::Axis2D) std::snprintf(overlay, sizeof overlay, "(%.2f, %.2f)", v.vector.x, v.vector.y);
        else std::snprintf(overlay, sizeof overlay, "%.2f", v.scalar);
        const float magnitude = v.type == InputActionType::Axis2D ? std::sqrt(v.vector.x * v.vector.x + v.vector.y * v.vector.y) : std::fabs(v.scalar);
        grid.MeterRow("Value", magnitude, overlay);
    }

    void InputActionsInspectorPage::DrawPicker(const Guid& target)
    {
        if (!ImGui::BeginPopup("##input_pick")) return;
        // Cleared and focused on EVERY opening: typing goes to the search box
        // and never reaches the document's row key handlers.
        if (ImGui::IsWindowAppearing()) { pickerSearch_[0] = '\0'; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputTextWithHint("##picksearch", ICON_LC_SEARCH " Search controls", pickerSearch_, sizeof pickerSearch_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }); return s; };
        // Tokenised search: a choice is visible when EVERY whitespace-separated
        // token occurs in "<device> <control> <path>" ("stick x" finds the left
        // AND right stick X rows); no tokens = everything visible.
        std::vector<std::string> tokens;
        {
            std::string cur;
            for (const char* p = pickerSearch_; ; ++p)
            {
                const char ch = *p;
                if (ch == '\0' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    if (!cur.empty()) { tokens.push_back(lower(cur)); cur.clear(); }
                    if (ch == '\0') break;
                }
                else cur.push_back(ch);
            }
        }
        static const std::vector<InputControlChoice> kKnown = InputActions::KnownControls();   // the evaluator's tables, once, in ITS emitted order
        const InputControlChoice* firstVisible = nullptr;
        auto commit = [&](const InputControlChoice& c)
        {
            edit_ = [m = &model_, target, path = c.path] { (void)m->SetField(target, "path", path); };
            ImGui::CloseCurrentPopup();
        };
        ImGui::BeginChild("##picklist", ImVec2(300.0f, 260.0f));
        std::string lastDevice;   // a device header is emitted lazily, when the device changes from the previous VISIBLE row: no empty headers
        for (const auto& c : kKnown)
        {
            const std::string hay = lower(c.display.device + " " + c.display.control + " " + c.path);
            bool visible = true;
            for (const auto& t : tokens) if (hay.find(t) == std::string::npos) { visible = false; break; }
            if (!visible) continue;
            if (c.display.device != lastDevice) { ImGui::TextDisabled("%s", c.display.device.c_str()); lastDevice = c.display.device; }
            if (!firstVisible) firstVisible = &c;
            const char* icon = c.display.device == "Keyboard" ? ICON_LC_KEYBOARD : c.display.device == "Mouse" ? ICON_LC_MOUSE : ICON_LC_GAMEPAD_2;
            const std::string label = std::string(icon) + " " + c.display.control + "##" + c.path;
            if (ImGui::Selectable(label.c_str())) commit(c);
            ImGui::SameLine(200.0f);
            ImGui::TextDisabled("%s", c.path.c_str());
        }
        ImGui::EndChild();
        // Enter picks the FIRST visible choice (UE SKeySelector's search-then-
        // Enter); with nothing visible it does nothing and the popup stays open.
        if (enter && firstVisible) commit(*firstVisible);
        ImGui::EndPopup();
    }
}
