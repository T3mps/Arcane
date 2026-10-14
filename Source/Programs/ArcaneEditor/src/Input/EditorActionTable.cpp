#include "Input/EditorActionTable.hpp"

namespace Arcane::Editor
{
    void RegisterEditorActions(EditorActions& actions)
    {
        for (const EditorActionDesc& d : kEditorActionTable)
            actions.Register(d);
    }
}
