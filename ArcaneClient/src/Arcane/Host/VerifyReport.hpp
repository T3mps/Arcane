#pragma once

// VerifyReport: turns an offscreen host's capture into machine-readable FACTS
// an agent can assert on -- probe specs (`brightness@640,360`, `census`, ...),
// their evaluation against one captured frame, and a JSON report.
//
// This is the boundary between the engine tier and the separate, optional
// Servitor package: Servitor parses this JSON WITHOUT linking the engine, so
// `schemaVersion` is a compatibility promise, not decoration -- the same role
// `.arcproj`'s `formatVersion` and the engine's own `abi` play elsewhere. The
// engine's job stops at emitting facts; there is no assertion DSL here, and
// none belongs here -- that lives on the Servitor side of the boundary.
//
// Wiring: HostConfig (Task 2) carries `probes` (raw, unparsed, in command-line
// order) and `reportPath`. Parsing probe CONTENTS is deliberately this
// component's job, not HostConfig's -- see HostConfig.hpp's `probes` comment.
// Feeding a real capture/census into this component from a running host is
// Task 8, not here: this header only defines the component and its contract.
//
// CAPTURE FORMAT ASSUMPTION (read before touching Brightness/Luma below):
// SetCapture's bytes come from ReadCapture, which reads back
// kGraphOffscreenFormat -- BGRA8_UNORM, NOT a `_SRGB` format
// (NriGraphContext.hpp:284-293's own comment: "Display-referred... the
// tonemap already gamma-2.2 encodes, so a plain UNORM target matches what a
// real backbuffer shows"). That means every byte SetCapture receives is
// ALREADY gamma-encoded (display-referred), not scene-linear. Luma (Y') is
// defined ON gamma-encoded values -- its weights apply directly to these
// bytes, no linearisation step. Do not add an sRGB-to-linear conversion here;
// that would be correct for LUMINANCE (Y), which this is deliberately not.

#include <Arcane/Base/Api.hpp>

#include <Json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    // The probe vocabulary. Brightness/Luma/Rgba/Pick are POSITIONAL
    // (`kind@x,y`); Census is ARGLESS (`census`, no `@...`) -- it asks a
    // question about the whole scene's material binding, not one pixel. See
    // ParseProbe for the exact grammar and ProbeSpec below for which fields
    // each kind actually uses.
    //
    // One line each, because an agent reads the JSON this produces, not this
    // header -- but "what does this number mean" has to be answerable here,
    // without archaeology, for the next engineer:
    //   Brightness -- unweighted (R+G+B) / 765, NriGraphPixelTest.cpp's own
    //                 Luma() reused verbatim (see VerifyReport.cpp). Renamed
    //                 FROM "luma" because it is not perceptual -- pure red
    //                 and pure green both read ~0.33 under this formula
    //                 despite being ~3x apart in perceived brightness.
    //   Luma       -- Rec.709 Y': 0.2126*R + 0.7152*G + 0.0722*B, weights
    //                 applied DIRECTLY to the gamma-encoded byte values (see
    //                 the capture-format note above), normalised to [0,1].
    //   Rgba       -- the four raw channel bytes at (x, y), 0-255 each.
    //   Pick       -- the entity at (x, y), reported as a DURABLE identity
    //                 (Identity.id's Guid + Identity.name), not the raw
    //                 hit-proxy uint32 the id pass wrote -- see SetPick's
    //                 comment for why the two must never be conflated.
    //   Census     -- the six SceneRenderResolver::MaterialCensus counts.
    enum class ProbeKind : std::uint8_t { Brightness, Luma, Rgba, Pick, Census };

    struct ProbeSpec
    {
        ProbeKind    kind{};
        std::int32_t x = 0, y = 0;   // only meaningful for Brightness/Luma/Rgba/Pick
        std::string  raw;            // as typed, echoed into the report
    };

    // Parses one `--probe` argument's text (HostConfig::probes elements, one
    // call each). nullopt + `error` on anything malformed: a positional kind
    // with no `@x,y`, an argless kind WITH `@...`, a non-numeric or negative
    // coordinate, or a kind this build does not know. This is a REFUSAL, not
    // a silent skip -- a probe an agent asked for and silently never got is a
    // worse failure than one that never ran, because it is invisible in the
    // report (`ArcaneRuntime --probe brigtness@1,2` typo'd would otherwise
    // just vanish rather than tell anyone).
    //
    // Coordinates are unsigned on purpose, matching --pick-probe's own parse
    // (HostConfig.cpp): a NEGATIVE probe pixel is not a coordinate, it is a
    // typo, and evaluating it would report a fact about the wrong question.
    [[nodiscard]] ARCANE_API std::optional<ProbeSpec> ParseProbe(std::string_view text, std::string& error);

    // The FIRST `pick@x,y` spec among `probes` (HostConfig::probes, raw and
    // unparsed, in command-line order), or nullopt if none is present. ONE
    // pixel is ever probed per run -- FrameDesc::pickPixel is armed once
    // (RuntimeFrame.cpp's RenderGraph), from this same spec -- so this
    // helper is the SINGLE source of truth for which pixel that is, shared
    // by the frame driver (arms it, every frame) and the report writer
    // (resolves + describes it, once, at shutdown) so the two can never
    // disagree about which spec won. A run with more than one `pick@` probe
    // only ever gets an answer for this one; Evaluate's Pick case refuses
    // the others rather than silently answering the wrong question.
    //
    // Malformed specs among `probes` are silently skipped here -- ParseProbe's
    // error is reported once, later, by the real parse-and-log pass
    // (RuntimeApp.cpp's ShutdownGraphPath); this helper only answers "is
    // there a pick request", and must not double that log just to answer it.
    [[nodiscard]] ARCANE_API std::optional<ProbeSpec> FirstPickProbe(const std::vector<std::string>& probes);

    // Accumulates one host run's observations and renders them as one JSON
    // document. Every setter is independent and optional except Evaluate,
    // which reads back whatever SetCapture/AddCensus have been given so far
    // -- call them BEFORE Evaluate, not after, or a probe that needed that
    // data reports "not set" rather than picking up a later call.
    class ARCANE_API VerifyReport
    {
    public:
        // The run's own identity: which backend rendered it, whether it ran
        // offscreen, how many frames it completed, and why it stopped
        // (mirrors the exit-reason vocabulary the offscreen loop already
        // logs -- "frames-complete", a gpu-stall watchdog verdict, etc).
        void SetRun(std::string backend, bool offscreen, std::uint64_t framesRendered,
                    std::string exitReason);

        // The captured frame Brightness/Luma/Rgba probes read against. `rgba` is TIGHT
        // RGBA8 (row stride == w*4), the same shape ReadCapture hands back --
        // see NriGraphPixelTest.cpp's `At()` helper, which this reuses the
        // indexing math from.
        void SetCapture(std::uint32_t w, std::uint32_t h, const std::vector<unsigned char>& rgba);

        // The scene's material-binding facts a Census probe reads -- the same
        // six counts SceneRenderResolver::MaterialCensus already computes
        // (SceneRenderResolver.hpp). VerifyReport does not compute these
        // itself: it has no scene to look at, only whatever a host (Task 8)
        // hands it.
        void AddCensus(int spriteReferenced, int spriteBound, bool postReferenced,
                       bool postBound, int meshReferenced, int meshBound);

        // The pick readback (Task 9): the entity-id probe's answer, resolved
        // to a durable identity HOST-SIDE, before this call -- VerifyReport
        // has no registry to invert a hit-proxy id itself (the same "no
        // scene to look at" reasoning AddCensus's own comment gives for
        // Census). `armedX`/`armedY` is the ONE pixel FrameDesc::pickPixel
        // was set to this run (FirstPickProbe's spec) -- Evaluate's Pick
        // case refuses any `pick@x,y` probe asking about a DIFFERENT pixel,
        // rather than answering a question this run never measured.
        //
        //   landed      -- false means the readback never arrived
        //                  (NriGraphContext::ProbeId() returned nullopt: the
        //                  run was too short, or the pixel was outside the
        //                  surface). Evaluate reuses RuntimeApp.cpp's own
        //                  "NO READBACK LANDED" wording for this case, so
        //                  the two paths read alike.
        //   hitProxyId  -- the RAW, frame-scoped id the id pass wrote (0 ==
        //                  background). Carried into the report for
        //                  debugging ONLY -- meaningless outside the one
        //                  draw submission that produced it, and NEVER the
        //                  field an agent should address an entity by (see
        //                  entityGuid below).
        //   resolved    -- true iff hitProxyId named a live entity that ALSO
        //                  carries an Identity component, i.e.
        //                  entityName/entityGuid are meaningful. Only
        //                  consulted when hitProxyId != 0 -- a background
        //                  hit (id 0) is a FACT regardless of this flag, not
        //                  routed through it.
        //   entityName/entityGuid -- Identity::name / Identity::id.ToString()
        //                  (canonical lowercase 8-4-4-4-12 hex): the durable
        //                  pair an agent bootstraps a readable query from and
        //                  then addresses by id. Ignored unless
        //                  resolved && hitProxyId != 0.
        void SetPick(std::int32_t armedX, std::int32_t armedY, bool landed,
                     std::uint32_t hitProxyId, bool resolved,
                     std::string entityName, std::string entityGuid);

        // Evaluates every spec against whatever SetCapture/AddCensus/SetPick were
        // given before this call, and appends one JSON entry per spec.
        // Callable more than once (specs accumulate) -- there is no reset,
        // matching the one-shot lifetime a host uses this for (parse ->
        // evaluate once -> write report -> exit).
        void Evaluate(const std::vector<ProbeSpec>& specs);

        // Renders the accumulated report. Follows EngineInfoJson's precedent
        // (ProjectBoot.hpp) exactly: compact (`dump(-1, ' ', false, ...)`)
        // with `error_handler_t::replace` so a malformed byte anywhere in a
        // string field (an exit reason, a probe's raw text) degrades to
        // U+FFFD instead of THROWING OUT OF THE CALLER -- an agent-facing
        // report that crashes the host it was reporting on is worse than no
        // report at all.
        [[nodiscard]] std::string ToJson() const;

        // Writes ToJson()'s bytes to `path`. false on any open/write failure
        // (path does not exist, no permission, disk full); the caller decides
        // whether that is fatal. Not the atomic temp+ReplaceFile dance
        // SaveSceneFile uses -- this report has no prior version on disk to
        // protect from a partial overwrite, it is a fresh diagnostic artifact
        // every run.
        [[nodiscard]] bool WriteTo(const std::string& path) const;

    private:
        std::string   m_backend;
        bool          m_offscreen = false;
        std::uint64_t m_framesRendered = 0;
        std::string   m_exitReason;

        bool                       m_captureSet = false;
        std::uint32_t              m_captureWidth = 0, m_captureHeight = 0;
        std::vector<unsigned char> m_captureRgba;

        bool m_censusSet          = false;
        int  m_spriteReferenced   = 0;
        int  m_spriteBound        = 0;
        bool m_postReferenced     = false;
        bool m_postBound          = false;
        int  m_meshReferenced     = 0;
        int  m_meshBound          = 0;

        bool          m_pickSet        = false;
        std::int32_t  m_pickArmedX     = 0, m_pickArmedY = 0;
        bool          m_pickLanded     = false;
        std::uint32_t m_pickHitProxyId = 0;
        bool          m_pickResolved   = false;
        std::string   m_pickEntityName;
        std::string   m_pickEntityGuid;

        // Already-evaluated probe entries, in Evaluate() call order.
        nlohmann::json m_probes = nlohmann::json::array();
    };
}
