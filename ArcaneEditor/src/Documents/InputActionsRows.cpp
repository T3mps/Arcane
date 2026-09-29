#include "Documents/InputActionsRows.hpp"

#include "Documents/InputActionsJson.hpp"

#include <Arcane/Input/InputActions.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>   // std::strtof (InteractionText)

namespace Arcane::Editor
{
    namespace
    {
        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
        bool Contains(const std::string& haystack, const std::string& needleLower)
        {
            return needleLower.empty() || Lower(haystack).find(needleLower) != std::string::npos;
        }
        std::vector<std::string> Groups(const nlohmann::json& row)
        {
            std::vector<std::string> out;
            if (row.is_object() && row.contains("groups") && row["groups"].is_array())
                for (const auto& g : row["groups"]) if (g.is_string()) out.push_back(g.get<std::string>());
            return out;
        }
        std::string Joined(const std::vector<std::string>& groups)
        {
            std::string s;
            for (const auto& g : groups) { if (!s.empty()) s += ", "; s += g; }
            return s;
        }
        bool InScheme(const std::vector<std::string>& groups, const std::string& scheme)
        {
            return scheme.empty() || groups.empty() || std::find(groups.begin(), groups.end(), scheme) != groups.end();
        }
        bool Conflicted(const std::vector<InputActionsEditorModel::BindingConflict>& conflicts, const Guid& id)
        {
            return std::any_of(conflicts.begin(), conflicts.end(), [&](const auto& c) { return c.binding == id; });
        }
    }

    std::string InteractionText(const nlohmann::json& interactions)
    {
        if (!interactions.is_array()) return {};
        std::string out;
        for (const auto& entry : interactions)
        {
            if (!entry.is_string()) continue;
            const std::string token = entry.get<std::string>();
            const std::size_t paren = token.find('(');
            const std::string name = token.substr(0, paren);
            float seconds = name == "hold" ? kDefaultHoldSeconds : name == "tap" ? kDefaultTapSeconds : 0.0f;   // the evaluator's own defaults (InputActions.hpp)
            if (paren != std::string::npos)
                if (const std::size_t d = token.find("duration=", paren); d != std::string::npos)
                {
                    // An unparseable value keeps the default, as the evaluator's
                    // ParseInteraction does (its std::stof throws, the default stays).
                    const char* begin = token.c_str() + d + 9;
                    char* end = nullptr;
                    const float parsed = std::strtof(begin, &end);
                    if (end != begin) seconds = parsed;
                }
            std::string text;
            if (name == "press") text = "Press";
            else if (name == "hold" || name == "tap")
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%s %.2f s", name == "hold" ? "Hold" : "Tap", seconds);
                text = buf;
            }
            else continue;
            if (!out.empty()) out += " · ";
            out += text;
        }
        return out;
    }

    std::vector<InputRow> BuildInputRows(const nlohmann::json& draft, const Guid& map, const InputRowFilter& filter,
                                         const std::vector<InputActionsEditorModel::BindingConflict>& conflicts,
                                         const std::unordered_set<std::string>& collapsedActions)
    {
        std::vector<InputRow> rows;
        const nlohmann::json* m = FindMap(draft, map);
        if (!m || !m->contains("actions") || !(*m)["actions"].is_array()) return rows;
        const std::string search = Lower(filter.search);

        auto bindingRow = [&](InputRowKind kind, const nlohmann::json& b, const Guid& actionId, const Guid& compositeId,
                              const std::vector<std::string>& groups, int depth)
        {
            InputRow r;
            r.kind = kind; r.id = IdOf(b); r.actionId = actionId;
            r.bindingId = kind == InputRowKind::Part ? compositeId : r.id;
            r.depth = depth; r.path = Str(b, "path");
            const InputControlDisplay d = InputActions::DisplayForPath(r.path);
            r.name = d.control; r.device = d.device;
            r.groups = groups; r.badge = Joined(groups);
            if (kind == InputRowKind::Part) r.detail = Str(b, "name");
            r.conflict = Conflicted(conflicts, r.id);
            return r;
        };

        for (const auto& action : (*m)["actions"])
        {
            const Guid actionId = IdOf(action);
            if (!actionId.IsValid()) continue;
            const std::string actionName = Str(action, "name");
            const bool actionHit = Contains(actionName, search);

            // Collect this action's rows first, so the search can decide
            // whether the action shows at all (any binding hit).
            std::vector<InputRow> children;
            bool anyBindingHit = false;
            if (action.contains("bindings") && action["bindings"].is_array())
                for (const auto& b : action["bindings"])
                {
                    if (!b.is_object()) continue;
                    const std::vector<std::string> groups = Groups(b);
                    if (!InScheme(groups, filter.schemeGroup)) continue;
                    if (b.contains("composite"))
                    {
                        InputRow header;
                        header.kind = InputRowKind::CompositeHeader; header.id = IdOf(b); header.actionId = actionId;
                        header.bindingId = header.id; header.depth = 1;
                        header.name = Str(b, "composite") == "1DAxis" ? "1D Axis" : "2D Vector";
                        header.groups = groups; header.badge = Joined(groups);
                        std::vector<InputRow> parts;
                        bool partHit = false;
                        if (b.contains("parts") && b["parts"].is_array())
                            for (const auto& p : b["parts"])
                            {
                                const auto pg = Groups(p);
                                if (!InScheme(pg.empty() ? groups : pg, filter.schemeGroup)) continue;
                                InputRow pr = bindingRow(InputRowKind::Part, p, actionId, header.id, pg.empty() ? groups : pg, 2);
                                pr.detail = header.name + " · " + pr.detail;   // "1D Axis · negative" (spec B 2.3); UTF-8 middle dot, the same literal as the "· actions" header
                                partHit |= Contains(pr.name, search) || Contains(pr.path, search);
                                parts.push_back(std::move(pr));
                            }
                        if (!actionHit && !search.empty() && !partHit) continue;
                        anyBindingHit |= partHit;
                        children.push_back(std::move(header));
                        for (auto& pr : parts) if (actionHit || search.empty() || Contains(pr.name, search) || Contains(pr.path, search)) children.push_back(std::move(pr));
                    }
                    else
                    {
                        InputRow br = bindingRow(InputRowKind::Binding, b, actionId, {}, groups, 1);
                        const bool hit = Contains(br.name, search) || Contains(br.path, search);
                        if (!actionHit && !search.empty() && !hit) continue;
                        anyBindingHit |= hit;
                        children.push_back(std::move(br));
                    }
                }
            if (!search.empty() && !actionHit && !anyBindingHit) continue;

            InputRow ar;
            ar.kind = InputRowKind::Action; ar.id = actionId; ar.actionId = actionId; ar.depth = 0;
            ar.name = actionName; ar.badge = Str(action, "type");
            ar.detail = action.contains("interactions") ? InteractionText(action["interactions"]) : std::string{};
            rows.push_back(std::move(ar));
            // A non-empty search overrides collapse: every surviving action draws
            // expanded so the matching binding is visible. collapsedActions itself
            // is untouched and returns when the search clears.
            if (search.empty() && collapsedActions.count(actionId.ToString())) continue;
            for (auto& c : children) rows.push_back(std::move(c));
            InputRow ghost;
            ghost.kind = InputRowKind::AddBinding; ghost.id = actionId; ghost.actionId = actionId; ghost.depth = 1;
            rows.push_back(std::move(ghost));
        }
        return rows;
    }

    std::optional<Guid> StepSelection(const std::vector<InputRow>& rows, const Guid& current, int direction)
    {
        std::vector<const InputRow*> selectable;
        for (const auto& r : rows) if (r.kind != InputRowKind::AddBinding) selectable.push_back(&r);
        if (selectable.empty()) return std::nullopt;
        std::ptrdiff_t at = -1;
        for (std::size_t i = 0; i < selectable.size(); ++i) if (selectable[i]->id == current) { at = static_cast<std::ptrdiff_t>(i); break; }
        if (at < 0) return selectable.front()->id;
        at = std::clamp<std::ptrdiff_t>(at + (direction < 0 ? -1 : 1), 0, static_cast<std::ptrdiff_t>(selectable.size()) - 1);
        return selectable[static_cast<std::size_t>(at)]->id;
    }

    std::optional<SiblingPosition> SiblingIndex(const nlohmann::json& draft, const Guid& id)
    {
        std::optional<SiblingPosition> found;
        auto visit = [&](auto&& self, const nlohmann::json& node, const Guid& parent) -> void
        {
            if (found) return;
            if (node.is_array())
            {
                for (std::size_t i = 0; i < node.size(); ++i)
                    if (IdOf(node[i]) == id) { found = SiblingPosition{ parent, i, node.size() }; return; }
                for (const auto& child : node) self(self, child, parent);
            }
            else if (node.is_object())
            {
                const Guid own = IdOf(node);
                for (const auto& [key, child] : node.items()) self(self, child, own.IsValid() ? own : parent);
            }
        };
        visit(visit, draft, Guid{});
        return found;
    }
}
