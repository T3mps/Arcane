#pragma once

// Arcane/Edit: editor command foundation. ICommand is the undo/redo unit --
// the forward edit already happened (live), so a command only reverses/replays.
// ARCANE_API generic capability; Arcane Editor consumes it (no editor state here).

#include <Arcane/Base/Api.hpp>

namespace Arcane
{
    class ARCANE_API ICommand
    {
    public:
        virtual ~ICommand() = default;
        virtual void Undo() = 0;
        virtual void Redo() = 0;
        virtual const char* Label() const = 0;   // for UI / debug
    };
}
