#include "Panels/CreateAssetDialog.hpp"

#include "Panels/AssetPanelModel.hpp"
#include "Project/ClassTemplates.hpp"   // CppClass: ValidateClassName + the Template combo's labels
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Project/Project.hpp>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        // Spec s11.2's values table: "create menu / dialog widths | ~220px,
        // rows 28px / ~380px". The dialog is pinned to 380 EXACTLY (a
        // size-constraint with equal min/max on x), height auto-fits its
        // content -- the field count varies by kind and by whether the parent
        // picker is expanded.
        constexpr float kDialogWidth = 380.0f;
        // Picker rows reuse the table row pitch (s11.2: "table rows | 24px"),
        // the same value RowWithThumb defaults to -- the picker IS an asset
        // list, so it gets the asset-list row, not a bespoke one.
        constexpr float kPickerRowHeight = 24.0f;
        // Six rows before the picker scrolls: enough to browse a small
        // project's materials without the modal growing past a comfortable
        // height on a big one.
        constexpr int   kPickerVisibleRows = 6;
        // The mock's footer buttons.
        constexpr float kFooterButtonWidth = 92.0f;

        // One entry in the Location combo. `display` is what the combo shows
        // ("Content/materials"); `relative` is what CreateAssetResult carries
        // ("materials", or "" for Content/ itself), which the dispatcher joins
        // onto the project's content root.
        struct FolderChoice
        {
            std::string display;
            std::string relative;
        };

        // The model's folder strings are mount-path directories with a
        // trailing slash ("materials/"), and root-level assets fold into the
        // synthetic bucket "Content/" (MakeBaseEntry's own comment). Both
        // shapes normalise to the same pair here. `root` is the kind's
        // CreateKindRoot ("Content" / "Source"): the display is "<root>/rel".
        FolderChoice MakeFolderChoice(const std::string& relDir, const char* root)
        {
            std::string rel = relDir;
            while (!rel.empty() && rel.back() == '/')
                rel.pop_back();
            if (rel.empty())
                return { root, "" };   // the root itself names no subdirectory
            return { std::string(root) + "/" + rel, rel };
        }

        // A model folder KEY -> the directory relative to its mount root, or
        // nullopt when the key belongs to another mount. Two key shapes
        // (AssetPanelEntry::folder's own doc): the game mount is UNQUALIFIED
        // ("Content/" is its root, "materials/" nested); every other mount is
        // QUALIFIED ("source://" is its root, "source://combat/" nested).
        std::optional<std::string> RelativeDirOfFolderKey(const std::string& key, const char* root)
        {
            const bool wantSource = std::string_view(root) == "Source";
            if (const std::size_t sep = key.find("://"); sep != std::string::npos)
            {
                if (!wantSource || key.substr(0, sep) != "source")
                    return std::nullopt;
                return key.substr(sep + 3);   // "" for the root, "combat/" nested
            }
            if (wantSource)
                return std::nullopt;
            return key == "Content/" ? std::string() : key;
        }

        // Distinct create-able folders: every directory the project's OWN
        // files already use under the kind's root, plus the kind's default,
        // plus the root itself.
        //
        // ONE mount only -- "game://" for every asset kind, "source://" for
        // CppClass. The model groups folders across every mount (an engine://
        // and a game:// "materials/" share one Browse group), but a created
        // file can only land -- and only register + resolve by GUID -- under
        // the project's own root for that kind (Project.cpp mounts "game" at
        // root/Content and "source" at root/Source). Offering an engine or
        // plugin folder here would offer a target RegisterCreatedAsset refuses.
        std::vector<FolderChoice> BuildFolderChoices(const AssetPanelModel& model,
                                                     CreateAssetKind kind,
                                                     const std::string& cppDefaultFolder)
        {
            const char* root = CreateKindRoot(kind);
            std::set<std::string> folders;                     // relative dirs, "" = root
            folders.insert("");                                // always offer the root
            // CppClass's default folder comes from the manifest's sourceDir
            // (CppClassDefaultFolder, seeded onto the request by BeginCreateAsset);
            // every other kind keeps its fixed CreateKindDefaultFolder.
            folders.insert(kind == CreateAssetKind::CppClass
                               ? cppDefaultFolder
                               : std::string(CreateKindDefaultFolder(kind)));
            for (const auto& [guid, e] : model.Entries())
            {
                (void)guid;
                if (const auto rel = RelativeDirOfFolderKey(e.folder, root))
                    folders.insert(*rel);
            }

            std::vector<FolderChoice> out;
            out.reserve(folders.size());
            // Root first (it is the parent of everything else), then the rest
            // in the set's own alphabetical order -- a stable, predictable
            // list rather than unordered_map iteration order.
            out.push_back(MakeFolderChoice("", root));
            for (const std::string& f : folders)
                if (!f.empty())
                    out.push_back(MakeFolderChoice(f, root));
            return out;
        }

        int IndexOfRelativeFolder(const std::vector<FolderChoice>& choices, std::string_view relative)
        {
            for (int i = 0; i < static_cast<int>(choices.size()); ++i)
                if (choices[static_cast<std::size_t>(i)].relative == relative)
                    return i;
            return 0;
        }

        // The kind's default folder as a CreateAssetResult-shaped relative
        // path ("materials/" -> "materials"). Same CppClass substitution as
        // BuildFolderChoices above -- the seeded index must land on the same
        // folder the combo actually offers as the default.
        std::string DefaultRelativeFolder(CreateAssetKind kind, const std::string& cppDefaultFolder)
        {
            const std::string dir = kind == CreateAssetKind::CppClass
                                        ? cppDefaultFolder
                                        : std::string(CreateKindDefaultFolder(kind));
            return MakeFolderChoice(dir, CreateKindRoot(kind)).relative;
        }

        // Every material in the project, name-sorted -- the parent picker's
        // candidate list. Kind-filtered at the SOURCE (spec s7's
        // "kind-filtered pickers", which closes the recorded kind-blind-picker
        // follow-up): a non-material can never appear, so there is nothing to
        // mis-click.
        //
        // NOT mount-filtered, unlike the folder list above: a parent is a Guid
        // REFERENCE, not a file location, so an engine:// or plugin:// material
        // is a perfectly valid parent for an instance created under game://.
        std::vector<const AssetPanelEntry*> BuildMaterialCandidates(const AssetPanelModel& model)
        {
            std::vector<const AssetPanelEntry*> out;
            for (const auto& [guid, e] : model.Entries())
            {
                (void)guid;
                if (e.kind == AssetKind::Material)
                    out.push_back(&e);
            }
            std::sort(out.begin(), out.end(),
                      [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                      { return a->name < b->name; });
            return out;
        }

        // Name + Location, the two fields EVERY kind carries (spec s7's
        // "shared dialog anatomy"). Returns the name check so the caller can
        // gate Create on it.
        CreateNameCheck DrawNameAndLocation(CreateDialogState& st,
                                            const std::vector<FolderChoice>& folders,
                                            const Arcane::Project& project,
                                            bool justOpened)
        {
            ImGui::TextDisabled("Name");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (justOpened)
                ImGui::SetKeyboardFocusHere();
            ImGui::InputText("##createname", st.name, sizeof(st.name));

            const FolderChoice& folder = folders[static_cast<std::size_t>(
                std::clamp(st.folderIndex, 0, static_cast<int>(folders.size()) - 1))];
            const std::filesystem::path root = project.Root() / CreateKindRoot(st.request.kind);
            const std::filesystem::path targetDir =
                folder.relative.empty() ? root : (root / folder.relative);

            CreateNameCheck check =
                ValidateCreateName(st.name, targetDir, CreateKindExtension(st.request.kind));
            // CppClass: the name is also a C++ TYPE name. The file rules above
            // run first (cheapest, and a name that cannot be a file is moot);
            // the identifier rules only speak once those pass, so the two
            // messages never stack.
            if (check.ok && st.request.kind == CreateAssetKind::CppClass)
                if (const auto why = ClassTemplates::ValidateClassName(st.name))
                    check = { false, *why };
            // The validation line is drawn UNCONDITIONALLY (empty when the
            // name is fine) so the modal's height -- and therefore every
            // control below it -- does not jump the instant a name goes from
            // valid to invalid mid-typing.
            ImGui::TextDisabled("%s", check.ok ? "" : check.message.c_str());

            ImGui::TextDisabled("Location");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##createlocation", folder.display.c_str()))
            {
                for (int i = 0; i < static_cast<int>(folders.size()); ++i)
                {
                    const bool selected = (i == st.folderIndex);
                    if (ImGui::Selectable(folders[static_cast<std::size_t>(i)].display.c_str(), selected))
                        st.folderIndex = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            return check;
        }

        // Material: the surface combo (spec s7: "a Kind field (sprite / mesh /
        // post)" -- post materials get a real creation route for the first
        // time, and so does sprite, which until now could only be reached by
        // re-kinding an already-created fullscreen document).
        void DrawSurfaceField(CreateDialogState& st)
        {
            ImGui::TextDisabled("Kind");
            ImGui::SetNextItemWidth(-FLT_MIN);
            const int index = std::clamp(st.surface, 0, kMaterialSurfaceCount - 1);
            if (ImGui::BeginCombo("##createsurface", kMaterialSurfaceLabels[index]))
            {
                for (int i = 0; i < kMaterialSurfaceCount; ++i)
                {
                    const bool selected = (i == index);
                    if (ImGui::Selectable(kMaterialSurfaceLabels[i], selected))
                        st.surface = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        // CppClass: the Template combo -- Component / System / Plain class
        // (Project/ClassTemplates.hpp). Same shape as the Material Kind combo.
        void DrawClassTemplateField(CreateDialogState& st)
        {
            ImGui::TextDisabled("Template");
            ImGui::SetNextItemWidth(-FLT_MIN);
            const int count = static_cast<int>(ClassTemplates::Kind::Count);
            const int index = std::clamp(st.classTemplate, 0, count - 1);
            const auto label = [](int i) { return ClassTemplates::KindLabel(static_cast<ClassTemplates::Kind>(i)); };
            if (ImGui::BeginCombo("##createclasstemplate", label(index)))
            {
                for (int i = 0; i < count; ++i)
                {
                    const bool selected = (i == index);
                    if (ImGui::Selectable(label(i), selected))
                        st.classTemplate = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            // One line of what each template gives, so the choice is not a
            // guess. Component is the one that goes live by itself.
            switch (static_cast<ClassTemplates::Kind>(index))
            {
                case ClassTemplates::Kind::Component:
                    ImGui::TextDisabled("Reflected data on an entity. Live after Rebuild Game Module.");
                    break;
                case ClassTemplates::Kind::System:
                    ImGui::TextDisabled("A scheduler functor. Add its AddSystem line to Init (see the file).");
                    break;
                default:
                    ImGui::TextDisabled("A .hpp/.cpp pair in the project namespace.");
                    break;
            }
        }

        // MaterialInstance: the parent picker. A combo-shaped toggle over an
        // INLINE list (rather than a BeginCombo popup) because the list is not
        // just labels -- each row carries a kind icon and a subkind pill, and
        // the whole block carries a caption saying what was filtered out. That
        // caption is the point of the widget: an empty-looking picker must
        // explain itself rather than read as "there is nothing".
        void DrawParentPicker(CreateDialogState& st, const AssetPanelModel& model)
        {
            const std::vector<const AssetPanelEntry*> candidates = BuildMaterialCandidates(model);

            ImGui::TextDisabled("Parent material");

            const AssetPanelEntry* chosen = st.parent.IsValid() ? model.Find(st.parent) : nullptr;
            // A picked parent that vanished from the model (deleted mid-dialog)
            // must not read as still-picked -- drop it so Create stays gated.
            if (st.parent.IsValid() && !chosen)
                st.parent = Arcane::Guid{};

            char label[192];
            std::snprintf(label, sizeof(label), "%s##createparent",
                          chosen ? chosen->name.c_str() : "select...");
            // Styled to READ as the Location combo above it (the mock draws
            // both the same): a Button carries ImGuiCol_Button and centers its
            // label, so both are overridden here -- a frame-well background and
            // a left-aligned label, which is exactly what BeginCombo would have
            // drawn. It is a Button and not a combo only because the list below
            // is not a plain label list (see this function's own comment).
            ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            const bool parentClicked = ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            if (parentClicked)
                st.pickerOpen = !st.pickerOpen;
            {
                // The combo's chevron, painted over the button's own right
                // edge -- a Button is used (not BeginCombo) for the reason in
                // this function's comment, so its affordance is drawn here.
                const ImVec2 itemMin = ImGui::GetItemRectMin();
                const ImVec2 itemMax = ImGui::GetItemRectMax();
                const ImVec2 glyph = ImGui::CalcTextSize(ICON_LC_CHEVRON_DOWN);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(itemMax.x - glyph.x - ImGui::GetStyle().FramePadding.x,
                           itemMin.y + ((itemMax.y - itemMin.y) - glyph.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_LC_CHEVRON_DOWN);
            }

            if (!st.pickerOpen)
                return;

            // Caption: what the filter kept, and how much of the project that
            // is. `Entries()` is the UNFILTERED total on purpose -- "3 of 15
            // assets" is only meaningful against everything that exists.
            const float captionRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            ImGui::TextDisabled("materials only");
            char counts[48];
            std::snprintf(counts, sizeof(counts), "%d of %d assets",
                          static_cast<int>(candidates.size()),
                          static_cast<int>(model.Entries().size()));
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                          captionRight - ImGui::CalcTextSize(counts).x));
            ImGui::TextDisabled("%s", counts);

            const int rows = std::min(static_cast<int>(candidates.size()), kPickerVisibleRows);
            // + the bordered child's OWN vertical padding, both sides:
            // ImGuiChildFlags_Borders enables WindowPadding (imgui.h's own note
            // on that flag), so a height of exactly rows*24 clips the last row
            // by that padding instead of showing it.
            const float height = std::max(kPickerRowHeight, kPickerRowHeight * static_cast<float>(rows))
                               + ImGui::GetStyle().WindowPadding.y * 2.0f;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::kWell);
            if (ImGui::BeginChild("##createparentlist", ImVec2(-FLT_MIN, height), ImGuiChildFlags_Borders))
            {
                if (candidates.empty())
                {
                    ImGui::TextDisabled("no materials in this project yet");
                }
                for (const AssetPanelEntry* e : candidates)
                {
                    ImGui::PushID(e->guid.ToString().c_str());
                    const AssetRowResult res =
                        RowWithThumb("##pick", 0, ICON_LC_PALETTE, e->name.c_str(),
                                     st.parent == e->guid, 0.0f, kPickerRowHeight);
                    if (res.clicked)
                    {
                        st.parent = e->guid;
                        st.pickerOpen = false;
                    }
                    if (e->surface)
                    {
                        ImGui::SetCursorScreenPos(res.trailingPos);
                        AssetPill(MaterialSurfacePillText(*e->surface));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // Every texture in the project, name-sorted -- the Sprite dialog's
        // texture picker candidate list. Kind-filtered at the SOURCE, same
        // reasoning as BuildMaterialCandidates above.
        std::vector<const AssetPanelEntry*> BuildTextureCandidates(const AssetPanelModel& model)
        {
            std::vector<const AssetPanelEntry*> out;
            for (const auto& [guid, e] : model.Entries())
            {
                (void)guid;
                if (e.kind == AssetKind::Texture)
                    out.push_back(&e);
            }
            std::sort(out.begin(), out.end(),
                      [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                      { return a->name < b->name; });
            return out;
        }

        // Sprite: the source-texture picker. Same combo-shaped toggle-over-
        // inline-list widget as DrawParentPicker above (a Button substitutes
        // for BeginCombo for the reason given in that function's comment),
        // filtered to AssetKind::Texture -- a sprite's ONE asset-valued field
        // (spec s7: "Sprite -> texture picker"). No subkind pill: a texture
        // carries no MaterialSurface to show.
        void DrawTexturePicker(CreateDialogState& st, const AssetPanelModel& model)
        {
            const std::vector<const AssetPanelEntry*> candidates = BuildTextureCandidates(model);

            ImGui::TextDisabled("Texture");

            const AssetPanelEntry* chosen = st.texture.IsValid() ? model.Find(st.texture) : nullptr;
            // A picked texture that vanished from the model (deleted mid-
            // dialog) must not read as still-picked -- drop it, same guard
            // DrawParentPicker applies to its own chosen parent.
            if (st.texture.IsValid() && !chosen)
                st.texture = Arcane::Guid{};

            char label[192];
            std::snprintf(label, sizeof(label), "%s##createtexture",
                          chosen ? chosen->name.c_str() : "select...");
            ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            const bool textureClicked = ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            if (textureClicked)
                st.pickerOpen = !st.pickerOpen;
            {
                const ImVec2 itemMin = ImGui::GetItemRectMin();
                const ImVec2 itemMax = ImGui::GetItemRectMax();
                const ImVec2 glyph = ImGui::CalcTextSize(ICON_LC_CHEVRON_DOWN);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(itemMax.x - glyph.x - ImGui::GetStyle().FramePadding.x,
                           itemMin.y + ((itemMax.y - itemMin.y) - glyph.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_LC_CHEVRON_DOWN);
            }

            if (!st.pickerOpen)
                return;

            const float captionRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            ImGui::TextDisabled("textures only");
            char counts[48];
            std::snprintf(counts, sizeof(counts), "%d of %d assets",
                          static_cast<int>(candidates.size()),
                          static_cast<int>(model.Entries().size()));
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                          captionRight - ImGui::CalcTextSize(counts).x));
            ImGui::TextDisabled("%s", counts);

            const int rows = std::min(static_cast<int>(candidates.size()), kPickerVisibleRows);
            const float height = std::max(kPickerRowHeight, kPickerRowHeight * static_cast<float>(rows))
                               + ImGui::GetStyle().WindowPadding.y * 2.0f;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::kWell);
            if (ImGui::BeginChild("##createtexturelist", ImVec2(-FLT_MIN, height), ImGuiChildFlags_Borders))
            {
                if (candidates.empty())
                {
                    ImGui::TextDisabled("no textures in this project yet");
                }
                for (const AssetPanelEntry* e : candidates)
                {
                    ImGui::PushID(e->guid.ToString().c_str());
                    const AssetRowResult res =
                        RowWithThumb("##pick", 0, ICON_LC_IMAGE, e->name.c_str(),
                                     st.texture == e->guid, 0.0f, kPickerRowHeight);
                    if (res.clicked)
                    {
                        st.texture = e->guid;
                        st.pickerOpen = false;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }

    std::optional<CreateAssetResult> DrawCreateAssetDialog(CreateDialogState& st,
                                                           const AssetPanelModel& model,
                                                           const Arcane::Project& project)
    {
        if (!st.open)
            return std::nullopt;

        const char* title = CreateKindTitle(st.request.kind);
        const std::vector<FolderChoice> folders = BuildFolderChoices(model, st.request.kind, st.request.cppDefaultFolder);

        // Re-arm the ImGui popup whenever it isn't currently open -- which
        // includes a competing dockspace-level modal having closed it out
        // from under this request (see CreateDialogState::seeded). That is
        // NOT the same question as "is this a new request": folderIndex is
        // seeded exactly once per request, against the list that only EXISTS
        // here (BeginCreateAsset cannot know it), keyed on `st.seeded` rather
        // than on IsPopupOpen so an interrupted-then-resumed dialog keeps
        // whatever folder the user had already picked.
        const bool justOpened = !ImGui::IsPopupOpen(title);
        if (justOpened)
            ImGui::OpenPopup(title);
        if (!st.seeded)
        {
            st.folderIndex = IndexOfRelativeFolder(folders, DefaultRelativeFolder(st.request.kind, st.request.cppDefaultFolder));
            st.seeded = true;
        }

        // Width pinned to spec s11.2's ~380px EXACTLY (equal min/max on x);
        // height auto-fits, because the field count varies by kind and by
        // whether the parent picker is expanded.
        ImGui::SetNextWindowSizeConstraints(ImVec2(kDialogWidth, 0.0f),
                                            ImVec2(kDialogWidth, FLT_MAX));

        std::optional<CreateAssetResult> result;
        bool keepOpen = true;   // the title bar's x
        if (ImGui::BeginPopupModal(title, &keepOpen,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings))
        {
            const CreateNameCheck check = DrawNameAndLocation(st, folders, project, justOpened);

            bool ready = check.ok;
            switch (st.request.kind)
            {
                case CreateAssetKind::Material:
                    DrawSurfaceField(st);
                    break;
                case CreateAssetKind::MaterialInstance:
                    DrawParentPicker(st, model);
                    ready = ready && st.parent.IsValid();
                    break;
                case CreateAssetKind::Mesh:
                    // No kind-specific INPUT: MeshAssetData's own defaults (a
                    // unit Cube) are already a complete, valid asset -- Name +
                    // Location above is everything EditorApp::MintMeshAsset
                    // needs (spec s7: "Mesh gets the dialog (name + location,
                    // default cube data as today)"). F4 plan 1 Task 11 (spec
                    // s8): a `Create > Mesh > <primitive>` entry arrives with
                    // the generator PRESET on the request; it is shown as a
                    // read-only line (the choice was the menu entry itself --
                    // the MeshDocument's Source combo is where it changes
                    // after the fact) and rides the result to MintMeshAsset.
                    if (st.request.prefillMeshSource >= 0)
                    {
                        const char* name = PrimitiveMeshName(
                            static_cast<Arcane::MeshSource>(st.request.prefillMeshSource));
                        ImGui::TextDisabled("Source: %s", name ? name : "Cube");
                    }
                    break;
                case CreateAssetKind::Sprite:
                {
                    DrawTexturePicker(st, model);
                    ready = ready && st.texture.IsValid();

                    // Mint-or-reuse notice (spec s7): the MODEL's own fold
                    // data, not a fresh registry/disk scan -- Find(texture)->
                    // derivedChildren IS "sprites folded 1:1 under this
                    // texture" (AssetPanelModel's own aggregation, rebuilt
                    // every RebuildIfDirty), so this reads as of the same
                    // frame the Browse table's own fold rows do. Purely
                    // informational: Create stays enabled either way --
                    // EditorApp::MintOrReuseSpriteForTexture's own policy
                    // reuses a single match rather than minting a duplicate,
                    // so clicking Create here is never wrong, only redundant
                    // with the shortcut this notice offers.
                    //
                    // Gated on `== 1`, NOT "non-empty" (fix round 1): the
                    // core's own reuse gate is `matches == 1` (EditorAppProject.cpp,
                    // MintOrReuseSpriteForTexture) -- "never guess among
                    // duplicates" (EditorApp.hpp's own comment on that
                    // function). A texture with 2+ derived sprites is exactly
                    // the "several" case the core mints a fresh sibling for,
                    // so a singular "a 1:1 sprite already exists" notice and an
                    // `Open existing` that silently picks `.front()` would
                    // both misrepresent that state -- one guessing among
                    // duplicates in exactly the spot the core refuses to.
                    // Simplest fix consistent with the core: show NOTHING
                    // (plain Name/Location/Texture, Create enabled once the
                    // name validates) when there is more than one -- Create
                    // still does the right thing (mints a fresh sibling), the
                    // notice just isn't offered as a shortcut for an
                    // ambiguous case.
                    const AssetPanelEntry* tex =
                        st.texture.IsValid() ? model.Find(st.texture) : nullptr;
                    if (tex && tex->derivedChildren.size() == 1)
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled("a 1:1 sprite already exists");
                        if (ImGui::Button("Open existing", ImVec2(-FLT_MIN, 0.0f)))
                        {
                            CreateAssetResult r;
                            r.kind         = st.request.kind;
                            r.openExisting = tex->derivedChildren.front();
                            result         = std::move(r);
                            keepOpen       = false;
                        }
                    }
                    break;
                }
                case CreateAssetKind::Scene:
                    ImGui::Checkbox("Set as boot scene", &st.setAsBoot);
                    break;
                case CreateAssetKind::CppClass:
                    DrawClassTemplateField(st);
                    break;
            }

            // Footer: Cancel then Create, right-aligned (the mock's order).
            const float footerWidth = kFooterButtonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                          ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
                                          - footerWidth));
            if (ImGui::Button("Cancel", ImVec2(kFooterButtonWidth, 0.0f)))
                keepOpen = false;
            ImGui::SameLine();
            ImGui::BeginDisabled(!ready);
            if (ImGui::Button("Create", ImVec2(kFooterButtonWidth, 0.0f)))
            {
                CreateAssetResult r;
                r.kind      = st.request.kind;
                r.name      = st.name;
                r.folder    = folders[static_cast<std::size_t>(
                                  std::clamp(st.folderIndex, 0,
                                             static_cast<int>(folders.size()) - 1))].relative;
                r.surface   = static_cast<int>(MaterialSurfaceForComboIndex(st.surface));
                r.classTemplate = st.classTemplate;
                r.meshSource = st.request.prefillMeshSource;
                r.parent    = st.parent;
                r.texture   = st.texture;
                r.setAsBoot = st.setAsBoot;
                result      = std::move(r);
                keepOpen    = false;
            }
            ImGui::EndDisabled();

            if (!keepOpen)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // The popup is gone -- Cancel/Create/the x above, or ImGui's own
        // Escape handling, which closes a modal without telling anyone. Either
        // way the state must stop claiming to be open, or the block above
        // would re-OpenPopup it next frame.
        if (!keepOpen || !ImGui::IsPopupOpen(title))
            st.open = false;

        return result;
    }
}
