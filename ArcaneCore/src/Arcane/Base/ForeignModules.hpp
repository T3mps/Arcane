#pragma once

// Injected-module detection (Base module): which modules an outside party
// put into THIS process, classified two ways at once -- by ORIGIN (whose
// tree the file sits in), which needs no list and catches every injector,
// and by a TABLE of the overlays known to corrupt or crash a D3D12/Vulkan
// host, which supplies the product name, the consequence and the remedy.
//
// Why this exists: on 2026-09-22 the editor died at close because
// GTIII-OSD64.dll (ASUS GPU Tweak III's on-screen display) released our
// ID3D12Device once too often every 31 presented frames, so the device was
// destroyed under NRI inside nriDestroyDevice. The reference armor in
// Render/DeviceCreationD3D12.cpp absorbs that and measures the deficit
// without knowing any name; this component is what tells the desk WHICH
// module did it and what to change, and what makes a red gate lane on a
// desk with an overlay attributable from the verify report alone. Table
// provenance: docs/research/2026-09-22-injected-overlay-modules.md.
//
// THE LIST DECORATES, IT DOES NOT GATE. Origin is decided from the module's
// path: under one of this host's own trees (the exe directory, plus every
// directory Module::Load has loaded from -- the game module, plugins) it is
// ours; under the Windows directory it is the OS's; anywhere else it got in
// from outside and is REPORTED whether or not the table knows it. The table
// only upgrades a known module from "injected, uncatalogued" to "GPU Tweak
// III, do this", and it outranks the path: a catalogued Tier 1 module is
// Tier 1 wherever its installer put it.
//
// It lives in Base, not Render, because the fact is a PROCESS fact -- the
// crash/hang report (Diagnostics.cpp) and the .arcdiag envelope need it, and
// Base cannot include Render. The render layer is only the TRIGGER: the
// native device owner calls Report() once after either backend's device is
// created, which is the moment the process's hooks are all in place.
//
// Windows-only where it touches the OS (EnumerateProcessModules, the roots);
// the pure half compiles and works everywhere, and the OS half returns empty
// elsewhere so the Linux port keeps linking.

#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::ForeignModules
{
    // The tiers, spelled as plain integers because they cross the report
    // boundary (VerifyReport's `foreignModules[].tier`) as numbers:
    //   1 -- catalogued, proven to corrupt or crash the host: one ARC_WARN
    //        naming the product, the consequence and the remedy; an editor
    //        Problems row; named on the RenderErrorCount summary line.
    //   2 -- catalogued present-path hook, usually benign: one ARC_INFO, so
    //        a red lane on such a desk can be attributed. Never a warning.
    //   3 -- NOT catalogued, but from outside this host's and Windows' trees:
    //        one ARC_INFO with its path. The row that makes the detection
    //        complete rather than a blocklist -- a new overlay, or a shell
    //        extension a file dialog pulled in, still shows up in the report.
    inline constexpr int kTierDestabilising = 1;
    inline constexpr int kTierPresentHook   = 2;
    inline constexpr int kTierUncatalogued  = 3;

    // One row of the known table. string_view because the table is static
    // storage; Match below copies out of it for anything that outlives a call.
    struct Entry
    {
        std::string_view module;        // canonical spelling, e.g. "GTIII-OSD64.dll"
        std::string_view product;       // what a human calls it
        int              tier;          // kTierDestabilising | kTierPresentHook
        std::string_view consequence;   // Tier 1 only: what it does to the host
        std::string_view remedy;        // Tier 1 only: what the desk should change
    };

    // The whole table, for a schema check or a listing. Never mutated.
    [[nodiscard]] ARCANE_CORE_API std::span<const Entry> Table() noexcept;

    // A module the process was found to carry. `module` is the base name AS
    // THE LOADER REPORTS IT (original case), so a log line names what the
    // user sees in Process Explorer, not the table's spelling; `path` is the
    // full path the loader reports (empty only when it could not say). A
    // Tier 3 row has a path and no product: the path IS its attribution.
    struct Match
    {
        std::string module;
        std::string path;
        std::string product;
        int         tier = 0;
        std::string consequence;
        std::string remedy;
    };

    // Table lookup by base name, case-insensitive and EXACT: "NahimicOSD.dll"
    // matches, "MyNahimicOSD.dll" and "NahimicOSD.dll.bak" do not. nullopt
    // for anything the table does not know. Pure; `path` is left empty.
    [[nodiscard]] ARCANE_CORE_API std::optional<Match> Classify(std::string_view moduleBaseName);

    // Every TABLE hit in `moduleBaseNames`, in the input's order, each module
    // reported ONCE however many times the list names it. Pure, name-only:
    // the origin-aware overload below is what the scan uses.
    [[nodiscard]] ARCANE_CORE_API std::vector<Match> MatchAll(std::span<const std::string> moduleBaseNames);

    // ---- origin -----------------------------------------------------------

    // Whose tree a module's file sits in. Unknown only for an empty path: a
    // module the enumerator could not path is not accused on no evidence.
    enum class Origin : std::uint8_t { Owned, System, Foreign, Unknown };

    // A module as the loader reports it: base name + full path.
    struct LoadedModule
    {
        std::string name;
        std::string path;
    };

    // Places `path` against the given roots. Case-insensitive, slash-
    // insensitive, and a TREE test rather than a prefix test ("C:\Windows2"
    // is not under "C:\Windows"). Pure: the roots are parameters so the rule
    // is testable without this process's own directories.
    [[nodiscard]] ARCANE_CORE_API Origin OriginOf(std::string_view path,
                                                  std::span<const std::string> ownedRoots,
                                                  std::string_view systemRoot);

    // The scan's real matcher: every table hit at its tier (the table
    // outranks the path), plus every remaining Origin::Foreign module as
    // Tier 3; nothing for Owned, System or Unknown. Input order, one row per
    // module. Pure, for the same reason as OriginOf.
    [[nodiscard]] ARCANE_CORE_API std::vector<Match> MatchAll(std::span<const LoadedModule> modules,
                                                              std::span<const std::string> ownedRoots,
                                                              std::string_view systemRoot);

    // The Tier 1 module names of `matches`, joined ", " in order; empty when
    // none. What the hosts append to their RenderErrorCount summary line so
    // "our error" and "the overlay's error" are told apart at a glance.
    [[nodiscard]] ARCANE_CORE_API std::string Tier1Names(std::span<const Match> matches);

    // ---- this process ------------------------------------------------------

    // The trees that count as OURS: the exe's own directory always, plus the
    // directory of everything NoteOwned has been told about. Read-only copy.
    [[nodiscard]] ARCANE_CORE_API std::vector<std::string> OwnedRoots();

    // "We loaded this ourselves": Module::Load's hook, called with the path
    // it just loaded. Its DIRECTORY joins the owned roots (once, whatever
    // the spelling), so a game module under <project>/Binaries or a plugin
    // in its own folder is ours and never a Tier 3 row.
    ARCANE_CORE_API void NoteOwned(std::string_view modulePath);

    // The Windows directory (GetWindowsDirectory), the OS's tree. Empty off
    // Windows.
    [[nodiscard]] ARCANE_CORE_API std::string SystemRoot();

    // Every module loaded in this process right now, name + full path
    // (EnumProcessModulesEx + GetModuleFileNameExW on Windows; empty
    // elsewhere). Takes the loader lock -- never call it from an exception
    // filter.
    [[nodiscard]] ARCANE_CORE_API std::vector<LoadedModule> EnumerateProcessModules();

    // ONE scan: EnumerateProcessModules through the origin-aware MatchAll
    // against this process's roots, REMEMBERED for LastScan. Cheap
    // (microseconds) but not free; the hosts run it at device creation
    // (through Report) and again when they write their verify report, so a
    // module that injected between the two is still on record. Never per
    // frame.
    ARCANE_CORE_API std::vector<Match> Scan();

    // What the most recent Scan found, without enumerating again -- nullopt
    // if no scan has run in this process. This is what a crash/hang report
    // reads: inside an exception filter the loader lock is off limits, and
    // "not scanned" is an honest answer there where a fresh scan is a hazard.
    [[nodiscard]] ARCANE_CORE_API std::optional<std::vector<Match>> LastScan();

    // Scan, then say each module ONCE per process: ARC_WARN per Tier 1 hit
    // (product, consequence, remedy), ARC_INFO per Tier 2 (product) and per
    // Tier 3 (path). Returns only the modules this call reported for the
    // first time, so a second call in the same process returns nothing -- a
    // second device, a project switch or a test creating twenty devices
    // never repeats a line. Never a modal.
    ARCANE_CORE_API std::vector<Match> Report();
}
