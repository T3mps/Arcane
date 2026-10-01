#pragma once

// Arcane/Edit: editor command foundation. ICommand is the undo/redo unit --
// the forward edit already happened (live), so a command only reverses/replays.
// ARCANE_API generic capability; Arcane Editor consumes it (no editor state here).

#include <Arcane/Base/Api.hpp>

#include <cstddef>

namespace Arcane
{
    class ARCANE_API ICommand
    {
    public:
        virtual ~ICommand() = default;
        virtual void Undo() = 0;
        virtual void Redo() = 0;
        virtual const char* Label() const = 0;   // for UI / debug

        // Spec 2026-09-30 s3.3. Every default errs toward the SAFE direction.
        // False: a document/asset/file step -- it never dirties the scene.
        virtual bool        AffectsScene() const { return true; }
        // True: nothing is left to act on (a closed document's step). The
        // stack discards it rather than spending a Ctrl+Z on it.
        virtual bool        IsExpired()    const { return false; }
        // Bytes this step holds, in memory OR spilled (UndoPayload): the unit
        // of the stack's byte budget.
        virtual std::size_t PayloadBytes() const { return 0; }
    };
}
