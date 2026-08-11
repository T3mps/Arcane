#pragma once
// Edge (architecture pass sec 6): per-key rising/falling edge tracking.
// Replaces 17 hand-rolled m_prev* bools + 18 `down && !prev` expressions +
// write-backs scattered across three functions. Update owns the write-back:
// forgetting the Update line makes a key DEAD (visible immediately), not
// auto-repeating (the old silent failure).
namespace Arcane::Editor
{
    struct Edge
    {
        bool down     = false;
        bool pressed  = false;   // this frame's rising edge
        bool released = false;   // this frame's falling edge

        void Update(bool nowDown)
        {
            pressed  = nowDown && !down;
            released = !nowDown && down;
            down     = nowDown;
        }
    };
}
