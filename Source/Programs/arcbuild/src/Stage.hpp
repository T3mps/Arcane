#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace arcbuild
{
    // The single-slot copy a backend cannot make for itself.
    //
    // Every backend but Ninja links the module straight into the slot
    // (Binaries/<gameModule>): MSBuild and Make write one output path per
    // configuration without complaint. beta8's ninja action refuses three
    // configurations naming the same output ("multiple rules generate
    // Binaries/X.dll"), so build/arcane.lua links each configuration to its
    // own Intermediate/<Config>/Ninja/Binaries/<gameModule> (NinjaLinkOutput)
    // -- and the copy into the slot has to happen HERE, in the driver, not in
    // a Premake post-build step: beta8's ninja module wraps post-build
    // commands in `cmd /C "..."` and escapes every inner quote as `\"`,
    // which cmd.exe reads as a bare backslash plus a quote -- a quoted
    // relative path becomes a DRIVE-ROOT path (`\"Binaries\"` -> `\Binaries\`,
    // i.e. C:\Binaries), and the module's own always-appended stamp touch
    // fails the same way, so on Windows no ninja post-build can ever exit 0
    // honestly (multibackend hardening, review F4: characterized live --
    // the first run created C:\Binaries and failed, every later run
    // silently did nothing). A raw `ninja <stem>_<Config>` therefore
    // links and stops; `arcbuild build/rebuild` is what updates the slot.
    //
    // Copies `built` over `slot` (parent directory created, an existing
    // slot overwritten -- the single-slot rule, spec 2026-09-13 s4.3) and,
    // when `built`'s sibling <stem>.pdb exists, that beside the slot too so
    // a debugger finds symbols exactly as it does for an MSBuild-built slot.
    // Returns the staged paths, or one message naming what failed; never
    // throws. A missing `built` is a failure (the backend reported success
    // but produced nothing where the contract says it would).
    struct StagedModule
    {
        std::vector<std::filesystem::path> copied;
    };

    [[nodiscard]]
    std::expected<StagedModule, std::string> StageBuiltModule(
        const std::filesystem::path& built,
        const std::filesystem::path& slot);
}
