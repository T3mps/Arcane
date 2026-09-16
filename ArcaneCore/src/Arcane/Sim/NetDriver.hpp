#pragma once

// INetDriver: the replication seam a Runtime holds, declared here ahead of the
// replication arc (Core-DLL split, spec docs/specs/2026-09-15-core-dll-split-
// design.md s5). ONE question is asked of it today -- "are you live?" -- because
// that is the one question the hot-reload path must ask: swapping the game module
// under a running net driver would tear down the world both ends of a connection
// agreed on, so PluginHost REFUSES the reload while any attached Runtime reports
// an active driver, and names which one.
//
// No implementation ships yet; ArcaneTests supplies a double (MultiRuntime
// ReloadTest.cpp's FakeDriver). Deliberately a bare interface with no lifetime
// claim: Runtime stores a raw pointer and never owns it.

namespace Arcane
{
    struct INetDriver
    {
        virtual ~INetDriver() = default;
        virtual bool IsActive() const noexcept = 0;
    };
}
