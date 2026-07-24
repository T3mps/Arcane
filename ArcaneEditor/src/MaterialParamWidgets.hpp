#pragma once

// Param-declaration -> editor widget mapping (shader-editor Slice 5). Pure
// (no ImGui): the ShaderEditorDocument's params panel switches on WidgetFor;
// the [editor] unit test pins the mapping so a new MatParamType cannot land
// without deciding its widget.

#include <Arcane/Material/MaterialTypes.hpp>

namespace Arcane::Editor
{
    enum class ParamWidget
    {
        SliderFloat,     // Float -- SliderFloat over ParamMeta [min..max]
        DragFloat2,      // Float2
        DragFloat4,      // Float4
        ColorEdit,       // Color -- ColorEdit4
        TexturePicker,   // Texture -- guid field now, browser picker in Slice 6
    };

    [[nodiscard]] constexpr ParamWidget WidgetFor(MatParamType type) noexcept
    {
        switch (type)
        {
            case MatParamType::Float:   return ParamWidget::SliderFloat;
            case MatParamType::Float2:  return ParamWidget::DragFloat2;
            case MatParamType::Float4:  return ParamWidget::DragFloat4;
            case MatParamType::Color:   return ParamWidget::ColorEdit;
            case MatParamType::Texture: return ParamWidget::TexturePicker;
        }
        return ParamWidget::DragFloat4;
    }
}
