#include "Settings/EditorGridSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorGridSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.viewport.grid", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorGridSettings, majorEvery)
            ARC_REFLECT_ATTR(DisplayName, "Major line every") ARC_REFLECT_ATTR(Range, 1.0, 1000.0)
            ARC_REFLECT_ATTR(Tooltip, "Metres between the 3D grid's major lines. The 2D grid's majors stay every tenth line of its decade.")
        ARC_REFLECT_FIELD(EditorGridSettings, lineColor)
            ARC_REFLECT_ATTR(DisplayName, "Line colour")
            ARC_REFLECT_ATTR(Tooltip, "Colour of the grid lines in both grids; the minor and major opacities set the alpha.")
        ARC_REFLECT_FIELD(EditorGridSettings, minorAlpha)
            ARC_REFLECT_ATTR(DisplayName, "Minor line opacity") ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Opacity of a fully faded-in minor grid line.")
        ARC_REFLECT_FIELD(EditorGridSettings, majorAlpha)
            ARC_REFLECT_ATTR(DisplayName, "Major line opacity") ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Opacity of the 2D grid's major lines. The 3D grid's major colour is under grid3D.")
        ARC_REFLECT_FIELD(EditorGridSettings, lineThickness)
            ARC_REFLECT_ATTR(DisplayName, "Line thickness") ARC_REFLECT_ATTR(Range, 0.5, 4.0)
            ARC_REFLECT_ATTR(Tooltip, "Width (px) of the 2D grid's lines.")
        ARC_REFLECT_FIELD(EditorGridSettings, maxLinesPerAxis)
            ARC_REFLECT_ATTR(DisplayName, "Max lines per axis") ARC_REFLECT_ATTR(Range, 256.0, 65536.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Draw budget: a 2D grid level with more lines than this on either axis is skipped.")
        ARC_REFLECT_FIELD(EditorGridSettings, fadeInPx)
            ARC_REFLECT_ATTR(DisplayName, "Level fade-in spacing") ARC_REFLECT_ATTR(Range, 1.0, 256.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "grid lod crossfade decade")
            ARC_REFLECT_ATTR(Tooltip, "Screen pixels between a 2D grid level's lines at which the level starts to fade in.")
        ARC_REFLECT_FIELD(EditorGridSettings, fadeFullPx)
            ARC_REFLECT_ATTR(DisplayName, "Level full spacing") ARC_REFLECT_ATTR(Range, 1.0, 1024.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "grid lod crossfade decade")
            ARC_REFLECT_ATTR(Tooltip, "Screen pixels between a 2D grid level's lines at which the level is fully drawn "
                                      "(at least 1 px past the fade-in spacing).")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(EditorGrid3DSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.viewport.grid3D", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorGrid3DSettings, majorColor)
            ARC_REFLECT_ATTR(DisplayName, "Major line colour")
            ARC_REFLECT_ATTR(Tooltip, "Colour and opacity of the 3D grid's major lines.")
        ARC_REFLECT_FIELD(EditorGrid3DSettings, minorSpacing)
            ARC_REFLECT_ATTR(DisplayName, "Minor spacing") ARC_REFLECT_ATTR(Range, 0.01, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Metres between the 3D grid's minor lines.")
        ARC_REFLECT_FIELD(EditorGrid3DSettings, fadeDistance)
            ARC_REFLECT_ATTR(DisplayName, "Fade distance") ARC_REFLECT_ATTR(Range, 1.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Metres from the camera at which the 3D grid has faded out.")
        ARC_REFLECT_FIELD(EditorGrid3DSettings, minHalfExtent)
            ARC_REFLECT_ATTR(DisplayName, "Minimum half-extent") ARC_REFLECT_ATTR(Range, 10.0, 1000000.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Smallest half-size (m) of the 3D grid's ground quad.")
        ARC_REFLECT_FIELD(EditorGrid3DSettings, extentPerAltitude)
            ARC_REFLECT_ATTR(DisplayName, "Extent per altitude") ARC_REFLECT_ATTR(Range, 1.0, 1000.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "How fast the ground quad grows with the camera's height above the plane (metres of half-extent per metre).")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorGridSettings);
    ARC_SETTINGS(EditorGrid3DSettings);

    GridSceneDesc MakeGridScene(const EditorGridSettings& grid, const EditorGrid3DSettings& grid3D) noexcept
    {
        GridSceneDesc d;
        d.minorSpacing      = grid3D.minorSpacing;
        d.majorEvery        = grid.majorEvery;
        d.fadeDistance      = grid3D.fadeDistance;
        d.minHalfExtent     = grid3D.minHalfExtent;
        d.extentPerAltitude = grid3D.extentPerAltitude;
        d.minorColor = glm::vec4(grid.lineColor.r, grid.lineColor.g, grid.lineColor.b, grid.minorAlpha);
        d.majorColor = glm::vec4(grid3D.majorColor.r, grid3D.majorColor.g, grid3D.majorColor.b, grid3D.majorColor.a);
        return d;
    }
}
