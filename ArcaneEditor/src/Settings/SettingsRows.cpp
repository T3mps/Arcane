#include "Settings/SettingsRows.hpp"

#include "Panels/AssetPanelModel.hpp"        // AssetKind, KindLabel, kAssetKindCount
#include "Panels/AssetReferenceField.hpp"    // AssetRefRow
#include "Settings/SettingsModel.hpp"        // SettingDisplayName
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"         // InputTextString
#include "Widgets/IconsLucide.h"

#include <imgui.h>
#include <imgui_internal.h>                  // GetActiveID

#include <charconv>
#include <climits>
#include <cctype>
#include <memory>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        bool FitsInt(const std::optional<CVarValue>& v)
        {
            if (!v) return false;
            switch (v->type)
            {
            case CVarType::Int32:  return true;
            case CVarType::UInt32: return v->AsUInt32() <= static_cast<std::uint32_t>(INT_MAX);
            case CVarType::Int64:  return v->AsInt64() >= INT_MIN && v->AsInt64() <= INT_MAX;
            case CVarType::UInt64: return v->AsUInt64() <= static_cast<std::uint64_t>(INT_MAX);
            default:               return false;
            }
        }

        int ToInt(const CVarValue& v)
        {
            switch (v.type)
            {
            case CVarType::Int32:  return v.AsInt32();
            case CVarType::UInt32: return static_cast<int>(v.AsUInt32());
            case CVarType::Int64:  return static_cast<int>(v.AsInt64());
            case CVarType::UInt64: return static_cast<int>(v.AsUInt64());
            default:               return 0;
            }
        }

        CVarValue FromInt(CVarType type, int v)
        {
            switch (type)
            {
            case CVarType::UInt32: return CVarValue::UInt32(static_cast<std::uint32_t>(v < 0 ? 0 : v));
            case CVarType::Int64:  return CVarValue::Int64(v);
            case CVarType::UInt64: return CVarValue::UInt64(static_cast<std::uint64_t>(v < 0 ? 0 : v));
            default:               return CVarValue::Int32(v);
            }
        }

        double ToDouble(const CVarValue& v)
        {
            switch (v.type)
            {
            case CVarType::Int32:   return v.AsInt32();
            case CVarType::UInt32:  return v.AsUInt32();
            case CVarType::Int64:   return static_cast<double>(v.AsInt64());
            case CVarType::UInt64:  return static_cast<double>(v.AsUInt64());
            case CVarType::Float32: return v.AsFloat32();
            case CVarType::Float64: return v.AsFloat64();
            default:                return 0.0;
            }
        }

        void ReadVec(const CVarValue& v, float out[4])
        {
            switch (v.type)
            {
            case CVarType::Vec2: { const CVarVec2 x = v.AsVec2(); out[0] = x.x; out[1] = x.y; break; }
            case CVarType::Vec3: { const CVarVec3 x = v.AsVec3(); out[0] = x.x; out[1] = x.y; out[2] = x.z; break; }
            case CVarType::Vec4: { const CVarVec4 x = v.AsVec4(); out[0] = x.x; out[1] = x.y; out[2] = x.z; out[3] = x.w; break; }
            default: break;
            }
        }

        CVarValue MakeVec(CVarType type, const float v[4])
        {
            switch (type)
            {
            case CVarType::Vec2: return CVarValue::Vec2(CVarVec2{ v[0], v[1] });
            case CVarType::Vec3: return CVarValue::Vec3(CVarVec3{ v[0], v[1], v[2] });
            default:             return CVarValue::Vec4(CVarVec4{ v[0], v[1], v[2], v[3] });
            }
        }

        std::optional<CVarValue> ParseIntegral(CVarType type, std::string_view text)
        {
            const char* b = text.data();
            const char* e = text.data() + text.size();
            switch (type)
            {
            case CVarType::Int32:  { std::int32_t v{};  if (std::from_chars(b, e, v).ec == std::errc{}) return CVarValue::Int32(v);  break; }
            case CVarType::UInt32: { std::uint32_t v{}; if (std::from_chars(b, e, v).ec == std::errc{}) return CVarValue::UInt32(v); break; }
            case CVarType::Int64:  { std::int64_t v{};  if (std::from_chars(b, e, v).ec == std::errc{}) return CVarValue::Int64(v);  break; }
            case CVarType::UInt64: { std::uint64_t v{}; if (std::from_chars(b, e, v).ec == std::errc{}) return CVarValue::UInt64(v); break; }
            default: break;
            }
            return std::nullopt;
        }

        void Push(SettingsRowContext& ctx, std::unique_ptr<SettingEditCommand> step)
        {
            if (step) ctx.undo.Push(std::move(step));
        }

        bool Commit(SettingsRowContext& ctx, const CVarDescInfo& d, CVarValue value)
        {
            std::unique_ptr<SettingEditCommand> step = EditSetting(ctx.registry, d, ctx.window, std::move(value), ctx.sink);
            if (!step) return false;
            ctx.undo.Push(std::move(step));
            return true;
        }

        std::string SourceOfRung(const CVarRegistry& registry, std::string_view name, SetBy rung)
        {
            const std::optional<CVarExplain> explain = registry.Explain(name);
            if (!explain) return {};
            for (auto it = explain->history.rbegin(); it != explain->history.rend(); ++it)
                if (it->by == rung) return it->module;
            return {};
        }

        // The gesture's ONE step, built at activation, run once at close: the
        // rung's before (pinned now) against what it holds at close.
        std::function<void()> BuildStep(SettingsRowContext& ctx, const CVarDescInfo& d, SetBy rung, const std::string& label)
        {
            CVarRegistry* reg = &ctx.registry;
            Arcane::CommandStack* stack = &ctx.undo;
            return [reg, stack, name = d.name, rung, before = ctx.registry.RungValue(d.name, rung),
                    beforeSource = SourceOfRung(ctx.registry, d.name, rung), handle = ctx.registry.Find(d.name),
                    sink = ctx.sink, text = "Edit " + label]
            {
                std::optional<CVarValue> after = reg->RungValue(name, rung);
                if (before == after) return;
                RungChange change;
                change.name = name;
                change.rung = rung;
                change.before = before;
                change.after = std::move(after);
                change.beforeSource = beforeSource;
                change.afterSource = std::string(RungSource(rung));
                change.handle = handle;
                std::vector<RungChange> changes{ std::move(change) };
                stack->Push(std::make_unique<SettingEditCommand>(*reg, std::move(changes), text, sink));
            };
        }

        // The EditGesture-after-row contract for a live row: 1. activation
        // opens the step; 2. live write-through whenever the row's value
        // differs from the rung (Escape: the rung exactly as at activation);
        // 3. EndAfterRow closes it at the row.
        bool Gesture(SettingsRowContext& ctx, const CVarDescInfo& d, const RowFacts& f, const std::string& label,
                     const CVarValue& value, bool committed)
        {
            EditGesture::BeginOnActivate(&ctx.undo, ctx.gesture,
                [&] { return "Edit " + label; },
                [&]() -> std::function<void()>
                {
                    ctx.memo = SettingsGestureMemo{ d.name, f.target, ctx.registry.RungValue(d.name, f.target), true };
                    return BuildStep(ctx, d, f.target, label);
                });
            const bool cancelled = ctx.grid.LastRowEvents().cancelled;
            if (cancelled && ctx.memo.live && ctx.memo.name == d.name)
            {
                ApplyRungState(ctx.registry, d.name, ctx.memo.rung, ctx.memo.before);
                if (ctx.sink) ctx.sink(RungChange{ d.name, ctx.memo.rung, std::nullopt, ctx.memo.before });
            }
            else if (!(value == f.effective))
            {
                ApplyRungState(ctx.registry, d.name, f.target, value);
                if (ctx.sink) ctx.sink(RungChange{ d.name, f.target, std::nullopt, value });
            }
            EditGesture::EndAfterRow(&ctx.undo, ctx.gesture, cancelled);
            if (cancelled || committed) ctx.memo.live = false;
            return committed;
        }

        // A one-shot text box in a decorated custom row: the draft re-seeds
        // whenever the box is not active; returns the text to commit on
        // deactivate-after-edit (Escape reverts in ImGui, so it compares equal).
        std::optional<std::string> TextField(SettingsRowContext& ctx, const CVarDescInfo& d, const std::string& label,
                                             const std::string& current)
        {
            ctx.grid.BeginCustomRow(label.c_str(), false);
            std::string& draft = ctx.textDrafts[d.name];
            if (ImGui::GetActiveID() != ImGui::GetID("##value")) draft = current;
            (void)InputTextString("##value", &draft);
            const bool done = ImGui::IsItemDeactivatedAfterEdit();
            ctx.grid.EndCustomRow(label.c_str());
            if (done && draft != current) return draft;
            return std::nullopt;
        }

        struct BadgeStyle { const char* icon; ImVec4 color; const char* tooltip; };
        BadgeStyle StyleFor(RowBadge b)
        {
            switch (b)
            {
            case RowBadge::NextWorld:     return { ICON_LC_ROTATE_CW, Theme::kTextDim, "Applies on the next world load (Play or scene reopen)" };
            case RowBadge::Restart:       return { ICON_LC_POWER, Theme::kWarning, "Restart required: read once at startup" };
            case RowBadge::Deterministic: return { ICON_LC_ATOM, Theme::kAmber, "Simulation: changing it changes replays and goldens" };
            }
            return { "", Theme::kTextDim, "" };
        }

        // The value cell's lead (RowDecor::lead, drawn BEFORE the value so the
        // value stays LastItemData): the Preferences scope switch, the badges,
        // and -- read-only rows -- the provenance marker with Clear override,
        // or the no-project note; on a path row, Browse.
        std::string DrawLead(SettingsRowContext& ctx, const CVarDescInfo& d, const RowFacts& f,
                             const std::string& label, bool needsProject)
        {
            bool any = false;
            const auto gap = [&] { if (any) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x); any = true; };
            if (ctx.window == SettingsWindowKind::Preferences)
            {
                gap();
                const bool thisProject = f.mode == PrefMode::ThisProject;
                // S3-4: SwitchPrefMode refuses an overridden row -- disable the switch.
                ImGui::BeginDisabled(!ctx.projectOpen || f.overridden);
                if (ImGui::SmallButton(thisProject ? ICON_LC_FOLDER "##scope" : ICON_LC_GLOBE "##scope"))
                    Push(ctx, SwitchPrefMode(ctx.registry, d, thisProject ? PrefMode::AllProjects : PrefMode::ThisProject, ctx.sink));
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("%s", !ctx.projectOpen ? "Open a project to choose where this value lives"
                                          : thisProject ? "This project only -- click to use the value shared by all projects"
                                                        : "All projects -- click to override it for this project only");
                ctx.grid.ProbeItem((label + "#scope").c_str());
            }
            for (const RowBadge b : BadgesFor(d))
            {
                gap();
                const BadgeStyle s = StyleFor(b);
                ImGui::TextColored(s.color, "%s", s.icon);
                ImGui::SetItemTooltip("%s", s.tooltip);
            }
            if (f.overridden)
            {
                gap();
                ImGui::TextColored(Theme::kAmber, "Overridden by %s", RungLabel(f.winner));
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear override##clear"))
                    Push(ctx, ClearOverride(ctx.registry, d, ctx.window, ctx.sink));
                ImGui::SetItemTooltip("Remove the %s value; the row shows what is left underneath", RungLabel(f.winner));
                ctx.grid.ProbeItem((label + "#clear").c_str());
            }
            else if (needsProject)
            {
                gap();
                ImGui::TextDisabled("Open a project to edit");
            }
            else if (RowWidgetFor(d) == RowWidget::Path && ctx.browsePath)
            {
                gap();
                if (ImGui::SmallButton(ICON_LC_FOLDER_OPEN "##browse")) ctx.browsePath(d.name, d.widget == "path:dir");
                ImGui::SetItemTooltip("Browse...");
                ctx.grid.ProbeItem((label + "#browse").c_str());
            }
            return {};
        }

        // RowDecor::label: the label is the last item here. The tooltip carries
        // the help and the cvar name (dim), so the console spelling is always
        // one hover away (spec s6.2); the context menu has Copy name, the row
        // verbs, and Explain -- cvar_explain's history, newest first (s6.4).
        void LabelHook(SettingsRowContext& ctx, const CVarDescInfo& d, const RowFacts& f,
                       const std::string& label, bool hovered, bool editable)
        {
            ctx.grid.ProbeItem((label + "#label").c_str());   // TEST SEAM: the label's centre
            if (hovered)
            {
                ctx.lastTooltip = d.name;
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(label.c_str());
                if (!d.help.empty())
                {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                    ImGui::TextUnformatted(d.help.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::TextDisabled("%s", d.name.c_str());
                ImGui::TextDisabled("Right-click: copy the name, explain the value");
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("##settingctx"))
            {
                ctx.lastContextMenu = d.name;
                if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(d.name.c_str());
                if (ImGui::MenuItem("Reset to default", nullptr, false, editable && f.modified))
                    Push(ctx, ResetSetting(ctx.registry, d, ctx.window, ctx.sink));
                if (ImGui::MenuItem("Clear override", nullptr, false, f.overridden))
                    Push(ctx, ClearOverride(ctx.registry, d, ctx.window, ctx.sink));
                if (ctx.window == SettingsWindowKind::Preferences &&
                    ImGui::MenuItem(f.mode == PrefMode::ThisProject ? "Use for all projects" : "Override for this project",
                                    nullptr, false, ctx.projectOpen && !f.overridden))
                    Push(ctx, SwitchPrefMode(ctx.registry, d, f.mode == PrefMode::ThisProject ? PrefMode::AllProjects
                                                                                          : PrefMode::ThisProject, ctx.sink));
                ImGui::Separator();
                if (ImGui::BeginMenu("Explain"))
                {
                    if (const std::optional<CVarExplain> e = ctx.registry.Explain(d.name))
                        for (auto it = e->history.rbegin(); it != e->history.rend(); ++it)
                        {
                            const std::string v = FormatSettingValue(it->value, d.enumNames);
                            if (it->module.empty()) ImGui::Text("%-24s %s", RungLabel(it->by), v.c_str());
                            else ImGui::Text("%-24s %s   (%s)", RungLabel(it->by), v.c_str(), it->module.c_str());
                        }
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }
        }

        bool DrawValueWidget(SettingsRowContext& ctx, const CVarDescInfo& d, const RowFacts& f, const std::string& label)
        {
            PropertyGrid& g = ctx.grid;
            const char* l = label.c_str();
            RowWidget w = RowWidgetFor(d);
            if (w == RowWidget::Asset && !ctx.assetRefs) w = RowWidget::Text;
            switch (w)
            {
            case RowWidget::Checkbox:
            {
                bool v = f.effective.AsBool();
                return g.CheckboxRow(l, v) && Commit(ctx, d, CVarValue::Bool(v));
            }
            case RowWidget::Int:
            {
                int v = ToInt(f.effective);
                std::optional<Astra::Range> range;
                if (d.min && d.max) range = Astra::Range(ToDouble(*d.min), ToDouble(*d.max));
                const bool c = g.IntRow(l, v, range);
                return Gesture(ctx, d, f, label, FromInt(d.type, v), c);
            }
            case RowWidget::Float:
            {
                float v = f.effective.AsFloat32();
                std::optional<Astra::Range> range;
                if (d.min && d.max) range = Astra::Range(ToDouble(*d.min), ToDouble(*d.max));
                const bool c = g.FloatRow(l, v, 0.01f, range, "%.3f");
                return Gesture(ctx, d, f, label, CVarValue::Float32(v), c);
            }
            case RowWidget::Slider:
            {
                float v = f.effective.AsFloat32();
                const bool c = g.SliderRow(l, v, d.min->AsFloat32(), d.max->AsFloat32());
                return Gesture(ctx, d, f, label, CVarValue::Float32(v), c);
            }
            case RowWidget::Double:
            {
                double v = f.effective.AsFloat64();
                const double lo = d.min ? d.min->AsFloat64() : 0.0;
                const double hi = d.max ? d.max->AsFloat64() : 0.0;
                g.BeginCustomRow(l, false);
                ImGui::DragScalar("##value", ImGuiDataType_Double, &v, 0.01f, d.min ? &lo : nullptr, d.max ? &hi : nullptr, "%.6g");
                const bool c = ImGui::IsItemDeactivatedAfterEdit();
                g.EndCustomRow(l);
                return Gesture(ctx, d, f, label, CVarValue::Float64(v), c);
            }
            case RowWidget::Vec:
            {
                const int n = d.type == CVarType::Vec2 ? 2 : d.type == CVarType::Vec3 ? 3 : 4;
                float v[4] = {};
                ReadVec(f.effective, v);
                const bool c = g.VecRow(l, v, n, 0.01f);
                return Gesture(ctx, d, f, label, MakeVec(d.type, v), c);
            }
            case RowWidget::Color:
            {
                const CVarColor c0 = f.effective.AsColor();
                float v[4] = { c0.r, c0.g, c0.b, c0.a };
                ImGuiID popup = 0;
                const bool c = g.ColorRow(l, v, &popup);
                const bool committed = Gesture(ctx, d, f, label, CVarValue::Color(CVarColor{ v[0], v[1], v[2], v[3] }), c);
                if (popup != 0)
                {
                    // The popup's edits come from foreign widgets: the popup pair brackets them (EditGesture.hpp:121-140).
                    EditGesture::BeginOnPopupOpen(&ctx.undo, ctx.gesture, popup,
                        [&] { return "Edit " + label; },
                        [&]() -> std::function<void()> { return BuildStep(ctx, d, f.target, label); });
                    EditGesture::EndOnPopupClose(&ctx.undo, ctx.gesture, popup);
                }
                return committed;
            }
            case RowWidget::Enum:
            {
                std::vector<const char*> items;
                items.reserve(d.enumNames.size());
                for (const std::string& s : d.enumNames) items.push_back(s.c_str());
                const int picked = g.ComboRow(l, items.data(), static_cast<int>(items.size()), f.effective.AsEnum());
                return picked >= 0 && Commit(ctx, d, CVarValue::Enum(picked));
            }
            case RowWidget::Asset:
            {
                AssetRefArgs args;
                args.guid = Arcane::Guid::FromString(f.effective.AsString()).value_or(Arcane::Guid{});
                args.kindFilter = AssetKindFilterFor(d.widget);
                const AssetRefEdit e = AssetRefRow(g, l, args, *ctx.assetRefs);
                if (e.op == AssetRefEdit::Op::Set)   return Commit(ctx, d, CVarValue::String(e.guid.ToString()));
                if (e.op == AssetRefEdit::Op::Clear) return Commit(ctx, d, CVarValue::String(std::string()));
                return false;
            }
            case RowWidget::IntText:
            {
                const std::optional<std::string> text = TextField(ctx, d, label, FormatSettingValue(f.effective, d.enumNames));
                if (!text) return false;
                const std::optional<CVarValue> parsed = ParseIntegral(d.type, *text);
                return parsed && Commit(ctx, d, *parsed);
            }
            case RowWidget::Text:
            case RowWidget::Path:
            case RowWidget::KeyChord:   // raw text; a custom page (Keyboard) owns these rows on its node
            case RowWidget::Font:       // S4 replaces with the family combo
            {
                const std::optional<std::string> text = TextField(ctx, d, label, f.effective.AsString());
                return text && Commit(ctx, d, CVarValue::String(*text));
            }
            }
            return false;
        }
    }

    RowWidget RowWidgetFor(const CVarDescInfo& d) noexcept
    {
        switch (d.type)
        {
        case CVarType::Bool:    return RowWidget::Checkbox;
        case CVarType::Int32:   return RowWidget::Int;
        case CVarType::UInt32:
        case CVarType::Int64:
        case CVarType::UInt64:  return FitsInt(d.min) && FitsInt(d.max) ? RowWidget::Int : RowWidget::IntText;
        case CVarType::Float32: return d.widget == "slider" && d.min && d.max ? RowWidget::Slider : RowWidget::Float;
        case CVarType::Float64: return RowWidget::Double;
        case CVarType::String:
            if (d.widget.starts_with("asset:"))                 return RowWidget::Asset;
            if (d.widget == "path:file" || d.widget == "path:dir") return RowWidget::Path;
            if (d.widget == "keychord")                         return RowWidget::KeyChord;
            if (d.widget == "font")                             return RowWidget::Font;
            return RowWidget::Text;
        case CVarType::Color:   return RowWidget::Color;
        case CVarType::Vec2:
        case CVarType::Vec3:
        case CVarType::Vec4:    return RowWidget::Vec;
        case CVarType::Enum:    return RowWidget::Enum;
        }
        return RowWidget::Text;
    }

    int AssetKindFilterFor(std::string_view widget)
    {
        if (!widget.starts_with("asset:")) return -1;
        const auto squash = [](std::string_view s)
        {
            std::string out;
            for (const char c : s)
                if (c != ' ' && c != '_' && c != '-') out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            return out;
        };
        const std::string want = squash(widget.substr(6));
        if (want.empty()) return -1;
        for (int k = 0; k < kAssetKindCount; ++k)
            if (squash(KindLabel(static_cast<AssetKind>(k))) == want) return k;
        return -1;
    }

    std::vector<RowBadge> BadgesFor(const CVarDescInfo& d)
    {
        std::vector<RowBadge> out;
        if (d.apply == ApplyMode::NextWorld) out.push_back(RowBadge::NextWorld);
        if (d.apply == ApplyMode::Restart) out.push_back(RowBadge::Restart);
        if (HasFlag(d.flags, CVarFlags::Deterministic)) out.push_back(RowBadge::Deterministic);
        return out;
    }

    SettingRowResult DrawSettingRow(SettingsRowContext& ctx, std::string_view name)
    {
        SettingRowResult out;
        const std::optional<CVarDescInfo> desc = ctx.registry.Describe(name);
        if (!desc) return out;
        const RowFacts facts = ComputeRowFacts(ctx.registry, *desc, ctx.window);
        const std::string label = SettingDisplayName(*desc);
        const bool needsProject = !ctx.projectOpen && (facts.target == SetBy::Project || facts.target == SetBy::User);
        out.drawn = true;
        out.overridden = facts.overridden;
        out.modified = facts.modified;
        out.readOnly = facts.overridden || needsProject;
        ImGui::PushID(desc->name.c_str());   // the row's id carries its TARGET (PropertyGrid.hpp:45-47)
        RowDecor decor;
        decor.label = [&](bool hovered) { LabelHook(ctx, *desc, facts, label, hovered, !out.readOnly); };
        decor.lead = [&]() -> std::string { return DrawLead(ctx, *desc, facts, label, needsProject); };
        if (out.readOnly)
        {
            ctx.grid.SetNextRowDecor(decor);   // ReadOnlyRow takes lead + label (S3-7)
            ctx.grid.ReadOnlyRow(label.c_str(), FormatSettingValue(facts.effective, desc->enumNames));
        }
        else
        {
            decor.reset = true;
            decor.resetActive = facts.modified;
            ctx.grid.SetNextRowDecor(decor);
            out.committed = DrawValueWidget(ctx, *desc, facts, label);
            if (ctx.grid.LastRowEvents().resetClicked)
                Push(ctx, ResetSetting(ctx.registry, *desc, ctx.window, ctx.sink));
        }
        ImGui::PopID();
        return out;
    }
}
