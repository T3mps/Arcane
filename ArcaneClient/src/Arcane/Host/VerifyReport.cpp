#include <Arcane/Host/VerifyReport.hpp>

#include <Arcane/Base/Assert.hpp>

#include <fstream>
#include <utility>

namespace Arcane
{
    namespace
    {
        // One entry per ProbeKind, shared between ParseProbe (grammar) and
        // Evaluate/ToJson (the "kind" string in the JSON) so the two can
        // never drift apart into two different spellings of the same kind.
        struct KindInfo
        {
            std::string_view name;
            ProbeKind        kind;
            bool             positional;   // takes `@x,y`; Census does not
        };

        constexpr KindInfo kKinds[] = {
            { "brightness", ProbeKind::Brightness, true  },
            { "luma",       ProbeKind::Luma,       true  },
            { "rgba",       ProbeKind::Rgba,       true  },
            { "pick",       ProbeKind::Pick,       true  },
            { "census",     ProbeKind::Census,     false },
        };

        const KindInfo* FindKind(std::string_view name) noexcept
        {
            for (const auto& k : kKinds)
                if (k.name == name)
                    return &k;
            return nullptr;
        }

        std::string_view KindName(ProbeKind kind) noexcept
        {
            for (const auto& k : kKinds)
                if (k.kind == kind)
                    return k.name;
            return "unknown";   // unreachable for a ProbeKind this file produced
        }

        // Parses a run of ASCII digits as a non-negative integer, refusing
        // anything else (empty, a sign, a decimal point, overflow past
        // INT32_MAX). Lifted from HostConfig.cpp's --pick-probe parse
        // (the `whole` lambda there) so the two coordinate grammars in this
        // codebase read identically rather than accepting subtly different
        // inputs.
        bool ParseWholeNonNegative(std::string_view text, long long& out) noexcept
        {
            if (text.empty())
                return false;
            out = 0;
            for (const char c : text)
            {
                if (c < '0' || c > '9')
                    return false;
                out = out * 10 + (c - '0');
                if (out > 0x7FFFFFFFll)
                    return false;
            }
            return true;
        }

        // Reads one RGBA8 texel out of a tight-packed capture buffer
        // (row stride == w*4, the shape ReadCapture hands back -- same
        // indexing NriGraphPixelTest.cpp's `At()` uses). false on an
        // out-of-bounds (x, y) or a buffer shorter than w*h*4 -- a caller
        // reporting a fact must never read (or crash on) memory it does not
        // own, even when the probe pixel itself was a typo.
        bool ReadTexel(std::uint32_t w, std::uint32_t h, const std::vector<unsigned char>& rgba,
                       std::int32_t x, std::int32_t y,
                       unsigned char& r, unsigned char& g, unsigned char& b, unsigned char& a) noexcept
        {
            if (x < 0 || y < 0)
                return false;
            if (static_cast<std::uint32_t>(x) >= w || static_cast<std::uint32_t>(y) >= h)
                return false;
            const std::size_t i = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4u;
            if (i + 3u >= rgba.size())
                return false;
            r = rgba[i]; g = rgba[i + 1]; b = rgba[i + 2]; a = rgba[i + 3];
            return true;
        }
    }

    std::optional<ProbeSpec> ParseProbe(std::string_view text, std::string& error)
    {
        const std::size_t at = text.find('@');
        const bool hasArgs = at != std::string_view::npos;
        const std::string_view kindText = hasArgs ? text.substr(0, at) : text;
        const std::string_view argsText = hasArgs ? text.substr(at + 1) : std::string_view{};

        const KindInfo* found = FindKind(kindText);
        if (!found)
        {
            error = "unknown probe kind '" + std::string(kindText) +
                    "' -- expected one of brightness, luma, rgba, pick, census";
            return std::nullopt;
        }

        if (found->positional && !hasArgs)
        {
            error = "probe '" + std::string(found->name) + "' needs coordinates -- e.g. " +
                    std::string(found->name) + "@640,360";
            return std::nullopt;
        }
        if (!found->positional && hasArgs)
        {
            error = "probe '" + std::string(found->name) +
                    "' takes no coordinates -- write '" + std::string(found->name) +
                    "', not '" + std::string(found->name) + "@...'";
            return std::nullopt;
        }

        ProbeSpec spec;
        spec.kind = found->kind;
        spec.raw  = std::string(text);

        if (found->positional)
        {
            const std::size_t comma = argsText.find(',');
            bool bad = comma == std::string_view::npos || comma == 0 || comma + 1 >= argsText.size();
            long long x = 0, y = 0;
            if (!bad)
            {
                // Unsigned on purpose (see ParseWholeNonNegative): a negative
                // probe pixel is a typo, not a coordinate outside the frame.
                bad = !ParseWholeNonNegative(argsText.substr(0, comma), x) ||
                      !ParseWholeNonNegative(argsText.substr(comma + 1), y);
            }
            if (bad)
            {
                error = "probe '" + std::string(found->name) +
                        "' wants two non-negative integers as x,y -- e.g. " +
                        std::string(found->name) + "@640,360";
                return std::nullopt;
            }
            spec.x = static_cast<std::int32_t>(x);
            spec.y = static_cast<std::int32_t>(y);
        }

        return spec;
    }

    std::optional<ProbeSpec> FirstPickProbe(const std::vector<std::string>& probes)
    {
        for (const std::string& raw : probes)
        {
            std::string ignoredError;   // the real parse-and-log pass reports this later
            if (std::optional<ProbeSpec> spec = ParseProbe(raw, ignoredError);
                spec && spec->kind == ProbeKind::Pick)
                return spec;
        }
        return std::nullopt;
    }

    bool PickPixelInRange(std::int32_t x, std::int32_t y,
                           std::uint32_t width, std::uint32_t height) noexcept
    {
        if (x < 0 || y < 0)
            return false;
        return static_cast<std::uint32_t>(x) < width && static_cast<std::uint32_t>(y) < height;
    }

    void VerifyReport::SetRun(std::string backend, std::uint64_t framesRendered,
                               std::string exitReason)
    {
        m_backend        = std::move(backend);
        m_framesRendered = framesRendered;
        m_exitReason     = std::move(exitReason);
    }

    void VerifyReport::SetCapture(std::uint32_t w, std::uint32_t h, const std::vector<unsigned char>& rgba)
    {
        m_captureSet    = true;
        m_captureWidth  = w;
        m_captureHeight = h;
        m_captureRgba   = rgba;
    }

    void VerifyReport::AddCensus(int spriteReferenced, int spriteBound, bool postReferenced,
                                  bool postBound, int meshReferenced, int meshBound)
    {
        m_censusSet        = true;
        m_spriteReferenced = spriteReferenced;
        m_spriteBound      = spriteBound;
        m_postReferenced   = postReferenced;
        m_postBound        = postBound;
        m_meshReferenced   = meshReferenced;
        m_meshBound        = meshBound;
    }

    void VerifyReport::SetPick(std::int32_t armedX, std::int32_t armedY, bool landed,
                                std::uint32_t hitProxyId, bool resolved,
                                std::string entityName, std::string entityGuid,
                                std::uint32_t surfaceWidth, std::uint32_t surfaceHeight)
    {
        m_pickSet          = true;
        m_pickArmedX       = armedX;
        m_pickArmedY       = armedY;
        m_pickLanded       = landed;
        m_pickSurfaceWidth  = surfaceWidth;
        m_pickSurfaceHeight = surfaceHeight;
        m_pickHitProxyId   = hitProxyId;
        m_pickResolved     = resolved;
        m_pickEntityName   = std::move(entityName);
        m_pickEntityGuid   = std::move(entityGuid);
    }

    void VerifyReport::SetCompare(std::string reference, std::string resolvedLevel,
                                   std::string referencePath, bool passed,
                                   std::uint64_t diffCount, double diffRatio,
                                   std::uint64_t maxDiffPixels, bool sizesMismatch,
                                   std::string diffPath, std::string errorMessage)
    {
        m_compareSet           = true;
        m_compareReference     = std::move(reference);
        m_compareResolvedLevel = std::move(resolvedLevel);
        m_compareReferencePath = std::move(referencePath);
        m_comparePassed        = passed;
        m_compareDiffCount     = diffCount;
        m_compareDiffRatio     = diffRatio;
        m_compareMaxDiffPixels = maxDiffPixels;
        m_compareSizesMismatch = sizesMismatch;
        m_compareDiffPath      = std::move(diffPath);
        m_compareErrorMessage  = std::move(errorMessage);
    }

    void VerifyReport::SetSettle(std::uint64_t attemptsUsed, bool converged, SettleBail bail,
                                  bool captureFailed)
    {
        // NO VERDICT, NOTHING TO SAY -- see SetSettle's doc comment. The loop
        // neither converged nor gave up on a bound, so it never ran far enough
        // to have a fact worth stating: m_settleSet stays false and ToJson
        // emits neither key. Refused here as well as at both call sites so a
        // future host cannot reintroduce a "timeout-bound" on a run that never
        // spent a timeout.
        if (!SettleVerdictReached(converged, bail))
            return;

        m_settleSet           = true;
        m_settleAttemptsUsed  = attemptsUsed;
        m_settleConverged     = converged;
        m_settleBail          = bail;
        m_settleCaptureFailed = captureFailed;
    }

    void VerifyReport::Evaluate(const std::vector<ProbeSpec>& specs)
    {
        for (const auto& spec : specs)
        {
            nlohmann::json entry;
            entry["raw"]  = spec.raw;
            entry["kind"] = std::string(KindName(spec.kind));
            // x/y are only meaningful for the positional kinds (ProbeSpec's
            // own comment) -- Census reports a scene-wide fact, not a pixel,
            // so echoing a fixed (0, 0) back would read as a coordinate that
            // was never asked for.
            if (spec.kind != ProbeKind::Census)
            {
                entry["x"] = spec.x;
                entry["y"] = spec.y;
            }

            switch (spec.kind)
            {
            case ProbeKind::Brightness:
            {
                unsigned char r = 0, g = 0, b = 0, a = 0;
                if (!m_captureSet)
                {
                    entry["error"] = "no capture set -- Evaluate() was called before SetCapture()";
                }
                else if (!ReadTexel(m_captureWidth, m_captureHeight, m_captureRgba, spec.x, spec.y, r, g, b, a))
                {
                    entry["error"] = "pixel (" + std::to_string(spec.x) + "," + std::to_string(spec.y) +
                                      ") is outside the " + std::to_string(m_captureWidth) + "x" +
                                      std::to_string(m_captureHeight) + " capture";
                }
                else
                {
                    // The SUM `r + g + b` is VERBATIM from NriGraphPixelTest.cpp's
                    // Luma(const Rgba&): `static_cast<int>(p.r) +
                    // static_cast<int>(p.g) + static_cast<int>(p.b)`. That
                    // function's own comment calls this "deliberately crude
                    // and integer... never for a colour-accurate comparison"
                    // -- reused unchanged rather than reinvented, because two
                    // different definitions of brightness in one codebase is
                    // a bug generator, and the pixel tests are the reference
                    // this report has to agree with. The division by 765
                    // (255*3) is THIS component's own addition, not part of
                    // the reused expression: it normalises the sum's natural
                    // 0-765 range to [0,1] because this JSON is a
                    // cross-process contract Servitor parses without ever
                    // seeing the pixel test's raw integer scale.
                    //
                    // Named "brightness", not "luma": this is an UNWEIGHTED
                    // channel sum, not a perceptual quantity -- pure red and
                    // pure green both read ~0.33 here despite being visibly
                    // far apart in brightness. See ProbeKind::Luma below for
                    // the weighted formula that name actually promises.
                    const int sum = static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b);
                    entry["value"] = static_cast<double>(sum) / (255.0 * 3.0);
                }
                break;
            }
            case ProbeKind::Luma:
            {
                unsigned char r = 0, g = 0, b = 0, a = 0;
                if (!m_captureSet)
                {
                    entry["error"] = "no capture set -- Evaluate() was called before SetCapture()";
                }
                else if (!ReadTexel(m_captureWidth, m_captureHeight, m_captureRgba, spec.x, spec.y, r, g, b, a))
                {
                    entry["error"] = "pixel (" + std::to_string(spec.x) + "," + std::to_string(spec.y) +
                                      ") is outside the " + std::to_string(m_captureWidth) + "x" +
                                      std::to_string(m_captureHeight) + " capture";
                }
                else
                {
                    // Rec.709 luma (Y'): 0.2126*R + 0.7152*G + 0.0722*B. This
                    // is Y' (luma), not Y (luminance) -- Y' is DEFINED on
                    // gamma-encoded values, so the weights apply directly to
                    // these bytes with NO sRGB-to-linear conversion. That is
                    // correct here specifically because the capture is
                    // already gamma-encoded (see the header's capture-format
                    // note, sourced from kGraphOffscreenFormat's own
                    // comment) -- linearising first would be the right move
                    // for luminance, and the wrong one for luma. Channel
                    // bytes are already 0-255, and the weights sum to 1, so
                    // dividing by 255 alone lands the result in [0,1].
                    const double y = 0.2126 * static_cast<double>(r) +
                                      0.7152 * static_cast<double>(g) +
                                      0.0722 * static_cast<double>(b);
                    entry["value"] = y / 255.0;
                }
                break;
            }
            case ProbeKind::Rgba:
            {
                unsigned char r = 0, g = 0, b = 0, a = 0;
                if (!m_captureSet)
                {
                    entry["error"] = "no capture set -- Evaluate() was called before SetCapture()";
                }
                else if (!ReadTexel(m_captureWidth, m_captureHeight, m_captureRgba, spec.x, spec.y, r, g, b, a))
                {
                    entry["error"] = "pixel (" + std::to_string(spec.x) + "," + std::to_string(spec.y) +
                                      ") is outside the " + std::to_string(m_captureWidth) + "x" +
                                      std::to_string(m_captureHeight) + " capture";
                }
                else
                {
                    entry["value"] = { { "r", static_cast<int>(r) }, { "g", static_cast<int>(g) },
                                        { "b", static_cast<int>(b) }, { "a", static_cast<int>(a) } };
                }
                break;
            }
            case ProbeKind::Pick:
                if (!m_pickSet)
                {
                    // Either this run never armed a pick readback at all (no
                    // `pick@x,y` probe -- FirstPickProbe found nothing), or a
                    // caller evaluated before calling SetPick() (the header's
                    // own "call setters before Evaluate" contract).
                    entry["error"] = "no pick set -- Evaluate() was called before SetPick(), or "
                                      "this run never armed a pick readback";
                }
                else if (spec.x != m_pickArmedX || spec.y != m_pickArmedY)
                {
                    // Only the FIRST `pick@x,y` spec in a run is ever armed
                    // (RuntimeFrame.cpp's RenderGraph arms FrameDesc::pickPixel
                    // once, from FirstPickProbe) -- a second, differently
                    // positioned pick probe in the same run asked a question
                    // the readback never answered. Reporting THIS pixel's
                    // hit-proxy id as if it belonged to that one would be a
                    // wrong-but-plausible answer, not a fact.
                    entry["error"] = "pixel (" + std::to_string(spec.x) + "," + std::to_string(spec.y) +
                                      ") was never probed -- only the first pick@ probe in a run is "
                                      "armed (this run armed (" + std::to_string(m_pickArmedX) + "," +
                                      std::to_string(m_pickArmedY) + "))";
                }
                else if (!m_pickLanded)
                {
                    // Fix round 1: "the run never landed a readback" and "you
                    // asked about a pixel that does not exist on this run's
                    // surface" are TWO DIFFERENT FACTS -- mirrors Brightness/
                    // Luma/Rgba's own "no capture set" vs "pixel outside
                    // capture" split (ReadTexel above) rather than folding
                    // both into one ambiguous message. Derived HERE, from the
                    // surface size SetPick was given, rather than trusting a
                    // host-computed bool -- the same "VerifyReport derives
                    // the fact itself" shape ReadTexel already uses.
                    //
                    // 0x0 is the "surface size unknown" sentinel (SetPick's
                    // doc comment) -- a real offscreen surface is never 0x0 --
                    // so that case (and the genuine in-range-but-too-short
                    // case) both fall through to the combined wording, which
                    // is honest either way: it just can't narrow further.
                    const bool knowsSurface = m_pickSurfaceWidth != 0 && m_pickSurfaceHeight != 0;
                    if (knowsSurface &&
                        !PickPixelInRange(m_pickArmedX, m_pickArmedY, m_pickSurfaceWidth, m_pickSurfaceHeight))
                    {
                        entry["error"] = "pixel (" + std::to_string(m_pickArmedX) + "," +
                                          std::to_string(m_pickArmedY) + ") is outside the " +
                                          std::to_string(m_pickSurfaceWidth) + "x" +
                                          std::to_string(m_pickSurfaceHeight) + " surface -- this pixel "
                                          "can never land, regardless of --frames";
                    }
                    else
                    {
                        // Reuses RuntimeApp.cpp's own wording (ShutdownGraphPath's
                        // --pick-probe report) verbatim, so the two paths read
                        // alike rather than inventing a second vocabulary for the
                        // same fact. No longer hedges with "...or outside the
                        // surface" when the surface IS known -- that case is
                        // the branch above now.
                        entry["error"] = knowsSurface
                            ? "NO READBACK LANDED -- the run was too short (the copy lands a couple "
                              "of frames after the pass that wrote it)"
                            : "NO READBACK LANDED -- the run was too short (the copy lands a couple of "
                              "frames after the pass that wrote it) or the probe pixel was outside the "
                              "surface (surface size unknown to this report)";
                    }
                }
                else if (m_pickHitProxyId == 0)
                {
                    // A background miss is a FACT, not an error: the readback
                    // landed and measured nothing there. `id` is the NIL
                    // Guid's canonical string, never the integer 0 -- `id` is
                    // ALWAYS the durable Identity.id shape in this report,
                    // whether the pick hit or missed; `hitProxyId` is where
                    // the raw, frame-scoped number belongs.
                    entry["entity"]     = nullptr;
                    entry["id"]         = "00000000-0000-0000-0000-000000000000";
                    entry["hitProxyId"] = m_pickHitProxyId;
                    // fix round 1, item 4: see the resolved-hit branch below
                    // for why these two fields ride along on every RESULT
                    // (never an error) entry.
                    entry["pickableKinds"] = nlohmann::json::array({ "sprite", "collider2d" });
                    if (m_censusSet && m_meshReferenced > 0)
                        entry["meshesNotPickable"] = true;
                }
                else if (!m_pickResolved)
                {
                    // A live entity WAS hit (hitProxyId != 0) but the host
                    // could not resolve it all the way to an Identity
                    // component -- an honest error, not a stable-looking id
                    // that would silently churn between runs (this is
                    // load-bearing: see the header's own comment on why
                    // hitProxyId must never be reported under a field name
                    // that implies durability).
                    entry["error"] = "pick landed on hit-proxy " + std::to_string(m_pickHitProxyId) +
                                      " but it could not be resolved to an entity carrying an "
                                      "Identity component -- no durable id to report";
                }
                else
                {
                    entry["entity"]     = m_pickEntityName;
                    entry["id"]         = m_pickEntityGuid;
                    entry["hitProxyId"] = m_pickHitProxyId;
                    // fix round 1, item 4: an agent has no way to tell "Ground"
                    // is a wrong-but-durable answer for a pixel that is
                    // visibly a 3D mesh -- CollectPickables (PickEmit.hpp)
                    // only walks SpriteRenderer/Collider2D entities and has
                    // no knowledge of MeshRenderer ones at all, so a hit
                    // ALWAYS came from one of these two kinds, never a mesh.
                    // Naming that capability explicitly, and flagging when
                    // the scene has a bound mesh at all (the census this
                    // function already carries), converts a silent wrong
                    // answer into a visibly-qualified one without VerifyReport
                    // pretending to know WHICH mesh, if any, sits in front.
                    entry["pickableKinds"] = nlohmann::json::array({ "sprite", "collider2d" });
                    if (m_censusSet && m_meshReferenced > 0)
                        entry["meshesNotPickable"] = true;
                }
                break;
            case ProbeKind::Census:
                // NOT deferred: unlike Pick, AddCensus() already gives this
                // component a channel for the exact six counts a census asks
                // about (SceneRenderResolver::MaterialCensus's shape). A host
                // that has called AddCensus() before Evaluate() gets a real
                // answer today; one that has not gets an honest "not set"
                // rather than zeros that would read as a measured empty scene.
                if (!m_censusSet)
                {
                    entry["error"] = "no census set -- call AddCensus() before Evaluate()";
                }
                else
                {
                    entry["value"] = { { "spriteReferenced", m_spriteReferenced },
                                        { "spriteBound",      m_spriteBound },
                                        { "postReferenced",   m_postReferenced },
                                        { "postBound",        m_postBound },
                                        { "meshReferenced",   m_meshReferenced },
                                        { "meshBound",        m_meshBound } };
                }
                break;
            }

            // The invariant this whole report exists to uphold: an agent must
            // never have to guess whether a missing field means "zero" or
            // "failed". Assert it at the point every branch above converges,
            // not just in a JSON-shape test far away -- a future branch that
            // forgets to set either field fails loudly here, in Debug, at the
            // moment it was introduced.
            const bool hasResult = entry.contains("value") || entry.contains("entity");
            const bool hasError  = entry.contains("error");
            ARC_ASSERT(hasResult != hasError, "VerifyReport: probe entry must carry exactly one of value/entity or error");

            m_probes.push_back(std::move(entry));
        }
    }

    std::string VerifyReport::ToJson() const
    {
        nlohmann::json j;
        // Bumped 1 -> 2 by Task 8: a NEW required-for-the-mode section
        // (`compare`) is a contract change, not a silent addition -- this
        // JSON is the boundary Servitor parses without linking the engine,
        // and VerifyReportTest.cpp pins that it parses standalone.
        //
        // Bumped 2 -> 3 by Task 3 of the owed-defects arc, for TWO changes
        // that had to ship together: the settle facts below became reportable
        // (`settleAttemptsUsed` / `settleBailReason` -- they existed only in
        // logs, so an out-of-process agent could not see WHY a run gave up),
        // and `mode` changed VALUE. A version bump is the one moment at which
        // an existing wire value may legitimately change; doing it at any
        // other time is a silent break for a consumer that parses this
        // without linking us.
        j["schemaVersion"]   = 3;
        j["backend"]         = m_backend;
        // Always "headless" -- Fix 3 (final fix wave) removed the "windowed"
        // value from this contract: HostConfig::Parse refuses --report
        // without --headless unconditionally, so no live run can ever
        // produce anything else. Kept as a field (not just implied) because
        // an out-of-process consumer -- the Servitor package this JSON is the
        // boundary for -- should not have to infer the run kind from absence.
        //
        // The value was "offscreen" through schemaVersion 2. The MODE is
        // spelled --headless on every host's command line; "offscreen" is the
        // TECHNIQUE that mode renders with, and keeps the word everywhere it
        // genuinely describes the technique (CreateOffscreen, IsOffscreen,
        // kGraphOffscreenFormat). A machine-readable field a consumer
        // switches on must carry the mode's own name, so it now does.
        j["mode"]            = "headless";
        j["framesRendered"]  = m_framesRendered;
        j["exitReason"]      = m_exitReason;

        if (m_captureSet)
            j["capture"] = { { "width", m_captureWidth }, { "height", m_captureHeight } };

        if (m_censusSet)
        {
            j["census"] = { { "spriteReferenced", m_spriteReferenced },
                             { "spriteBound",      m_spriteBound },
                             { "postReferenced",   m_postReferenced },
                             { "postBound",        m_postBound },
                             { "meshReferenced",   m_meshReferenced },
                             { "meshBound",        m_meshBound } };
        }

        if (m_compareSet)
        {
            j["compare"] = { { "reference",     m_compareReference },
                              { "resolvedLevel", m_compareResolvedLevel },
                              { "referencePath", m_compareReferencePath },
                              { "passed",        m_comparePassed },
                              { "diffCount",     m_compareDiffCount },
                              { "diffRatio",     m_compareDiffRatio },
                              { "maxDiffPixels", m_compareMaxDiffPixels },
                              { "sizesMismatch", m_compareSizesMismatch },
                              { "diffPath",      m_compareDiffPath },
                              { "errorMessage",  m_compareErrorMessage } };
        }

        // The --settle verdict (Task 3). ABSENT unless SetSettle was called --
        // both keys or neither, exactly like `capture` and `compare` above --
        // so "settle was never asked for" can never be read as "asked, and
        // took 0 attempts".
        if (m_settleSet)
        {
            j["settleAttemptsUsed"] = m_settleAttemptsUsed;
            // captureFailed OUTRANKS the bound: it means no readback ever
            // landed, so the loop never had two frames to compare and NEITHER
            // bound got a fair test. Naming a bound there would send the caller
            // to a knob that cannot help. converged outranks the bound too,
            // because a converged run carries SettleBail::Keep -- there is no
            // governing bound to name on a run that never gave up.
            j["settleBailReason"]   =
                m_settleCaptureFailed                      ? "capture-failed"
              : m_settleConverged                          ? "converged"
              : m_settleBail == SettleBail::AttemptsBound   ? "attempts-bound"
                                                           : "timeout-bound";
        }

        j["probes"] = m_probes;

        // error_handler_t::replace, not the default throw -- same reasoning
        // as EngineInfoJson (ProjectBoot.hpp): this report is the ONE thing
        // an agent (or Servitor, parsing it without linking the engine) has
        // to learn what a host observed, so a malformed byte in an exit
        // reason or a probe's echoed raw text must degrade to U+FFFD in the
        // output, never throw out of a caller that is often already exiting.
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    bool VerifyReport::WriteTo(const std::string& path) const
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        const std::string json = ToJson();
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        return static_cast<bool>(out);
    }
}
