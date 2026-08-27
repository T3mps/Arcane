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

    // Whether canvas pixel (x, y) is inside a `width`x`height` surface (fix
    // round 1, item 2). Pulled out as its own pure, testable predicate
    // because --pick-probe's OWN out-of-range latch (NriGraphContext.cpp's
    // m_probeOutOfRange) is keyed to config.pickProbe -- the OLD flag's
    // state -- and is never armed on this path (FrameDesc::pickPixel "needs
    // no arming at all"). Left unchecked, NriGraphContext::ProbeId() would
    // happily return the CLAMPED EDGE TEXEL's hit-proxy id for an
    // out-of-range request like `pick@9999,9999` -- a confident id for a
    // pixel nobody asked about, exactly the failure the flag's own latch
    // exists to prevent. RuntimeApp.cpp's ShutdownGraphPath calls this
    // BEFORE trusting ProbeId() so an out-of-range probe never reaches it at
    // all. Negative coordinates never happen in practice (ParseProbe only
    // accepts non-negative ones), but are refused here too for a caller that
    // has not gone through that parse.
    [[nodiscard]] ARCANE_API bool PickPixelInRange(std::int32_t x, std::int32_t y,
                                                    std::uint32_t width, std::uint32_t height) noexcept;

    // Accumulates one host run's observations and renders them as one JSON
    // document. Every setter is independent and optional except Evaluate,
    // which reads back whatever SetCapture/AddCensus have been given so far
    // -- call them BEFORE Evaluate, not after, or a probe that needed that
    // data reports "not set" rather than picking up a later call.
    class ARCANE_API VerifyReport
    {
    public:
        // The run's own identity: which backend rendered it, how many frames
        // it completed, and why it stopped (mirrors the exit-reason
        // vocabulary the offscreen loop already logs -- "frames-complete", a
        // gpu-stall watchdog verdict, etc).
        //
        // NO `offscreen` PARAMETER (final fix wave, Fix 3 -- reconciling
        // windowed --report): this report can only ever describe an offscreen
        // run. HostConfig::Parse's wantsOffscreenOnly gate refuses --report
        // without --headless, unconditionally, for every host -- so
        // ArcaneRuntime's one real call site (RuntimeApp.cpp's
        // ShutdownGraphPath) could never have reached this with a windowed
        // run. A `bool offscreen` parameter that is provably always true is
        // not a fact worth carrying; ToJson's "mode" field reflects that by
        // always emitting "offscreen" rather than branching on a value that
        // can never be anything else. Older revisions of this component
        // modelled a "windowed" mode for a code path RuntimeApp.cpp never
        // actually had a way to reach -- see the plan's Task 9 desk item F,
        // which describes a scenario the parse-time gate makes unrunnable.
        void SetRun(std::string backend, std::uint64_t framesRendered,
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
        //                  run was too short, OR the pixel was outside the
        //                  surface). These are TWO DIFFERENT FACTS, not one
        //                  -- fix round 1 split them, mirroring how
        //                  Brightness/Luma/Rgba already distinguish "no
        //                  capture set" from "pixel outside capture"
        //                  (ReadTexel): "the run never captured anything" is
        //                  not the same claim as "you asked about a pixel
        //                  that does not exist", and an agent needs to tell
        //                  them apart. Evaluate derives WHICH of the two
        //                  applies itself, via PickPixelInRange against
        //                  surfaceWidth/surfaceHeight below -- it does not
        //                  trust a host-computed bool for that -- and reuses
        //                  RuntimeApp.cpp's "NO READBACK LANDED" wording only
        //                  for the genuine too-short case, now that the
        //                  out-of-range case has its own distinct message.
        //   surfaceWidth/surfaceHeight -- the pick id-buffer's own extent
        //                  (NriGraphContext::SurfaceWidth/Height), NOT the
        //                  same thing as SetCapture's capture buffer (a
        //                  different readback entirely) -- what
        //                  PickPixelInRange checks armedX/armedY against.
        //                  0/0 (the default) means "the caller did not know
        //                  or did not pass it", which Evaluate treats as
        //                  "cannot tell out-of-range from too-short" and
        //                  falls back to the combined wording -- a real
        //                  offscreen surface is never 0x0 (CreateOffscreen
        //                  refuses that extent), so 0/0 is an unambiguous
        //                  "unknown" sentinel, not a legitimate size.
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
                     std::string entityName, std::string entityGuid,
                     std::uint32_t surfaceWidth = 0, std::uint32_t surfaceHeight = 0);

        // The --compare verdict (Task 8). Emitted as a `compare` object ONLY
        // when this was actually called -- a run without --compare emits NO
        // such key, so an agent can distinguish "not asked" from "asked and
        // passed" (the same absence-must-be-absence contract SetCapture's
        // m_captureSet already upholds for probes). RuntimeApp::
        // ShutdownGraphPath is the one real caller, on every run that named
        // --compare -- converged-and-matched, converged-and-mismatched,
        // budget-exhausted, blessed, or resolution-missing all funnel
        // through this same call with different arguments, rather than each
        // growing its own reporting path.
        //
        //   reference      -- the bare --compare name, echoed back.
        //   resolvedLevel  -- "none" | "shared" | "backend", mirroring
        //                     Arcane::ReferenceLevel. "none" means no
        //                     reference existed (or, on a first --bless,
        //                     existed only after this run wrote it) -- NEVER
        //                     conflate this with a zero-difference pass; see
        //                     the missing-reference test case below.
        //   referencePath  -- the file this run actually compared against
        //                     (or blessed to), empty iff resolvedLevel is
        //                     "none" and nothing was blessed either.
        //   passed         -- true iff the comparison passed, OR a bless
        //                     wrote the reference this run (the reference
        //                     now IS the capture, by construction) -- never
        //                     true merely because nothing was compared.
        //   diffCount/diffRatio/maxDiffPixels/sizesMismatch -- lifted
        //                     verbatim from Arcane::ImageCompareResult
        //                     (CompareImages, Task 5); zero/false on a
        //                     bless or a missing-reference run, where no
        //                     comparison ever ran.
        //   diffPath       -- the diff artifact's path, written only on a
        //                     genuine mismatch; empty on a pass, a bless, or
        //                     a missing reference -- never absent, so an
        //                     agent can tell "no diff was needed" from
        //                     "SetCompare was never called" (the same
        //                     empty-not-absent contract diffPath's own test
        //                     case pins).
        //   errorMessage   -- human-readable context: CompareImages' own
        //                     message on a mismatch, or this component's
        //                     own wording for a missing reference / an
        //                     unconverged run; empty on a pass or a bless.
        void SetCompare(std::string reference, std::string resolvedLevel,
                        std::string referencePath, bool passed,
                        std::uint64_t diffCount, double diffRatio,
                        std::uint64_t maxDiffPixels, bool sizesMismatch,
                        std::string diffPath, std::string errorMessage);

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
        // 0/0 = "unknown" (see SetPick's doc comment) -- a real offscreen
        // surface is never 0x0, so this sentinel is unambiguous.
        std::uint32_t m_pickSurfaceWidth = 0, m_pickSurfaceHeight = 0;
        std::uint32_t m_pickHitProxyId = 0;
        bool          m_pickResolved   = false;
        std::string   m_pickEntityName;
        std::string   m_pickEntityGuid;

        // The --compare verdict (Task 8) -- see SetCompare's own comment for
        // what each field means and why m_compareSet gates emission the same
        // way m_captureSet/m_pickSet already gate theirs.
        bool          m_compareSet            = false;
        std::string   m_compareReference;
        std::string   m_compareResolvedLevel;
        std::string   m_compareReferencePath;
        bool          m_comparePassed         = false;
        std::uint64_t m_compareDiffCount      = 0;
        double        m_compareDiffRatio      = 0.0;
        std::uint64_t m_compareMaxDiffPixels  = 0;
        bool          m_compareSizesMismatch  = false;
        std::string   m_compareDiffPath;
        std::string   m_compareErrorMessage;

        // Already-evaluated probe entries, in Evaluate() call order.
        nlohmann::json m_probes = nlohmann::json::array();
    };
}
