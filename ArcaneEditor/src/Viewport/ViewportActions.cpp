#include "Viewport/ViewportActions.hpp"

namespace Arcane::Editor
{
    GlobalShortcutPresses ReadGlobalShortcuts(const EditorActions& a)
    {
        GlobalShortcutPresses p;
        p.undo          = a.Pressed("edit.undo");
        p.redo          = a.Pressed("edit.redo") || a.Pressed("edit.redoAlt");
        p.newScene      = a.Pressed("file.newScene");
        p.openScene     = a.Pressed("file.openScene");
        p.saveScene     = a.Pressed("file.saveScene");
        p.cut           = a.Pressed("edit.cut");
        p.copy          = a.Pressed("edit.copy");
        p.paste         = a.Pressed("edit.paste");
        p.duplicate     = a.Pressed("edit.duplicate");
        p.closeDocument = a.Pressed("document.close");
        p.perspective   = a.Pressed("editor.view.perspective");
        p.ortho2D       = a.Pressed("editor.view.ortho2D");
        p.frameSelected = a.Pressed("editor.view.frameSelected");
        p.frameAll      = a.Pressed("editor.view.frameAll");
        return p;
    }

    ViewportTool ReadViewportTool(const EditorActions& a, bool rmbHeld)
    {
        if (rmbHeld) return ViewportTool::None;
        ViewportTool tool = ViewportTool::None;
        if (a.Pressed("editor.viewport.toolSelect"))    tool = ViewportTool::Select;
        if (a.Pressed("editor.viewport.toolTranslate")) tool = ViewportTool::Translate;
        if (a.Pressed("editor.viewport.toolRotate"))    tool = ViewportTool::Rotate;
        if (a.Pressed("editor.viewport.toolScale"))     tool = ViewportTool::Scale;
        return tool;
    }

    glm::vec3 ReadFlyAxis(const EditorActions& a)
    {
        glm::vec3 axis(0.0f);
        if (a.Down("editor.camera.flyForward")) axis.z += 1.0f;
        if (a.Down("editor.camera.flyBack"))    axis.z -= 1.0f;
        if (a.Down("editor.camera.flyRight"))   axis.x += 1.0f;
        if (a.Down("editor.camera.flyLeft"))    axis.x -= 1.0f;
        if (a.Down("editor.camera.flyUp"))      axis.y += 1.0f;
        if (a.Down("editor.camera.flyDown"))    axis.y -= 1.0f;
        return axis;
    }
}
