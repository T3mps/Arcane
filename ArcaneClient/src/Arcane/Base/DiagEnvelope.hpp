#pragma once

// .arcdiag envelope (GPU crash diagnostics arc, Task 2): the on-disk format
// for a GPU-crash diagnostic report. Pure unit -- no GPU/OS dependency, no
// filesystem side effects beyond WriteFile/ReadFile themselves.
//
// Downstream: Task 4 (Diagnostics::WriteReport, see Diagnostics.cpp's
// WriteReportImpl / F-6b) fills one of these per crash/hang and writes it as
// a third sibling beside the existing .txt/.dmp, at the same base stem. Task
// 9 registers .arcdiag as a native embedded-GUID asset in AssetRegistry
// (like .arcmat/.arcsprite). Task 10 renders it.
//
// Versioned + round-trip tested: Serialize/Parse agree exactly on every
// field. Parse refuses anything that is not a validly-versioned,
// GUID-carrying envelope -- a missing/wrong-typed/unsupported
// "formatVersion" or a missing/unparsable/nil "guid" both come back nullopt.
// Forward compat means UNKNOWN extra keys are ignored on read, never that the
// required ones become optional.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Diag
{
    // The envelope format this module writes and the only one Parse accepts
    // today. Bump alongside a real Parse migration path when format 2 exists.
    inline constexpr std::uint32_t kFormatVersion = 1;

    struct Envelope
    {
        std::uint32_t formatVersion = kFormatVersion;
        Guid          guid;   // minted by the CALLER (never by this module)

        std::string kind;              // e.g. "gpu-crash"
        std::string timestampUtc;
        std::string appName;
        std::string phase;
        std::string buildInfo;

        // Symbolized all-thread text, same content shape as the .txt sibling
        // (see F-6b) -- carried inline so a renderer needs only the .arcdiag.
        std::string cpuThreadSummary;

        // One GPU queue's breadcrumb state at capture time: the last pass
        // known to have completed, plus whatever was still mid-flight.
        struct Queue
        {
            std::string name;
            std::string lastCompleted;
            std::vector<std::string> inFlight;
        };
        std::vector<Queue> queues;

        // Fault classification (page-fault vs hang vs ...), the faulting GPU
        // VA, and the resource name it resolved to, when known.
        struct Fault
        {
            std::string type;
            std::string address;
            std::string resource;
        };
        Fault fault;

        // Sibling report files minted alongside this one at the same base
        // stem (Diagnostics.cpp's WriteReportImpl; see F-6b). Empty when a
        // given sibling wasn't produced (e.g. no vendor GPU dump available).
        std::string siblingTxt;
        std::string siblingDmp;
        std::string siblingGpuDump;

        // Which diagnostic layers were actually armed for this capture (DRED
        // tier achieved, marker-buffer fallback used, ...) -- see F-2c/F-2d.
        std::vector<std::string> activeLayers;
    };

    // Envelope -> JSON text (2-space indent, UTF-8; invalid-UTF-8 field text
    // degrades to U+FFFD rather than throw). Always succeeds -- no IO here.
    ARCANE_API std::string Serialize(const Envelope& envelope);

    // JSON text -> Envelope. nullopt if the text doesn't parse as a JSON
    // object, if "formatVersion" is missing/wrong-typed/unsupported, or if
    // "guid" is missing/unparsable/nil. Every other field is best-effort:
    // an absent or wrong-typed optional field defaults empty, and unknown
    // extra keys are silently ignored (forward compat).
    ARCANE_API std::optional<Envelope> Parse(std::string_view json);

    // Serialize(envelope) written to `path` as UTF-8 with no BOM (trailing
    // newline, same convention as SaveMaterialAsset). False on IO failure.
    ARCANE_API bool WriteFile(const Envelope& envelope, const std::filesystem::path& path);

    // Read `path` in full and Parse it. nullopt on IO failure, on read, or
    // on an invalid envelope.
    ARCANE_API std::optional<Envelope> ReadFile(const std::filesystem::path& path);
}
