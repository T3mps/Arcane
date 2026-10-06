#pragma once
// The config rungs that exist BEFORE Runtime::OpenProject (settings arc S2,
// Integration ruling I2; specified in S6-2): engine config, the --project's
// Config/, the EditorUser rung (editor only), the project's user rung, then
// --perf (as diagnostics.perfLog=1, settings arc S6-2) and --set. Same SetBy
// and source strings as OpenProject, so its re-application replaces these
// records (S1: one history record per rung and source).
// Publishes immediately: Restart settings read at Install, at gpu_core and
// in the Runtime ctor see these values.
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Base/Api.hpp>
namespace Arcane { struct HostConfig; }
namespace Arcane::HostBoot
{
    ARC_API void ApplyEarlyConfigRungs(const HostConfig& cfg, CVarContext ctx, bool editor);
}
