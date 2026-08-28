// VerifyReport: probe parsing, evaluation against a captured frame, and JSON
// emission -- the pure-CPU half of the headless-verify path (no GPU, no
// device, no window; [verify] never touches [gpu]). This is the boundary
// between the engine tier and the separate Servitor package, which parses
// the JSON this produces without linking the engine.
#include <Arcane/Host/VerifyReport.hpp>

#include <Json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The three TEST_CASEs immediately below are the Task-7 brief's original
// pinned cases, UPDATED for the fix-round rename: the brief's "luma" probe
// tested the unweighted (R+G+B)/765 sum, which is now named "brightness"
// (see VerifyReport.hpp's ProbeKind comment for why -- it is not perceptual).
// "luma@640,360" is kept as a second assertion in the first case below so the
// new kind's grammar gets the same pinned coverage the old one had.
TEST_CASE("verify: probe specs parse, and malformed ones are refused", "[verify]")
{
    std::string err;
    const auto brightness = Arcane::ParseProbe("brightness@640,360", err);
    REQUIRE(brightness.has_value());
    CHECK(brightness->kind == Arcane::ProbeKind::Brightness);
    CHECK(brightness->x == 640);
    CHECK(brightness->y == 360);

    // Luma is a second, distinct positional kind (Rec.709, added alongside
    // the rename) -- same grammar, same refusal rules, exercised here too.
    const auto luma = Arcane::ParseProbe("luma@640,360", err);
    REQUIRE(luma.has_value());
    CHECK(luma->kind == Arcane::ProbeKind::Luma);
    CHECK(luma->x == 640);
    CHECK(luma->y == 360);

    const auto census = Arcane::ParseProbe("census", err);
    REQUIRE(census.has_value());
    CHECK(census->kind == Arcane::ProbeKind::Census);

    CHECK_FALSE(Arcane::ParseProbe("census@1,2", err).has_value());        // args to an argless kind
    CHECK_FALSE(Arcane::ParseProbe("brightness", err).has_value());        // kind missing its args
    CHECK_FALSE(Arcane::ParseProbe("brightness@-1,2", err).has_value());   // negative is a typo
    CHECK_FALSE(Arcane::ParseProbe("bogus@1,2", err).has_value());         // unknown kind
    CHECK_FALSE(err.empty());
}

TEST_CASE("verify: a brightness probe reads the capture and lands in the JSON", "[verify]")
{
    // 2x1 capture: black pixel, white pixel.
    const std::vector<unsigned char> rgba = { 0,0,0,255,  255,255,255,255 };

    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", /*framesRendered=*/5, "frames-complete");
    rep.SetCapture(2, 1, rgba);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("brightness@0,0", err), *Arcane::ParseProbe("brightness@1,0", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    // The report is the boundary between the engine tier and the Servitor
    // package, which parses this file without linking the engine -- so the
    // version is part of the contract, not decoration -- bumped to 2 by
    // Task 8's --compare/--bless block.
    CHECK(doc["schemaVersion"] == 3);
    CHECK(doc["backend"] == "D3D12");
    CHECK(doc["mode"] == "headless");
    CHECK(doc["framesRendered"] == 5);
    CHECK(doc["exitReason"] == "frames-complete");
    REQUIRE(doc["probes"].size() == 2);
    CHECK(doc["probes"][0]["kind"] == "brightness");
    CHECK(doc["probes"][0]["value"].get<double>() == Catch::Approx(0.0).margin(0.01));
    CHECK(doc["probes"][1]["value"].get<double>() == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE("verify: an out-of-bounds probe reports a fact, it does not crash", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 1, "frames-complete");
    rep.SetCapture(2, 1, { 0,0,0,255, 255,255,255,255 });
    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("brightness@99,99", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    REQUIRE(doc["probes"].size() == 1);
    CHECK(doc["probes"][0].contains("error"));
    CHECK_FALSE(doc["probes"][0].contains("value"));
}

TEST_CASE("verify: brightness and luma diverge on a pure-green pixel -- the whole point of the rename", "[verify]")
{
    // Pure green: brightness (unweighted) reads the same ~0.33 it would for
    // pure red or pure blue -- exactly the blind spot that made "luma" the
    // wrong name for it. Luma (Rec.709, G weighted 0.7152) reads far higher.
    // If a future change ever made these two agree again, the rename would
    // have bought nothing -- this is the test that catches that.
    const std::vector<unsigned char> rgba = { 0, 255, 0, 255 };   // 1x1, pure green
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 1, "frames-complete");
    rep.SetCapture(1, 1, rgba);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("brightness@0,0", err), *Arcane::ParseProbe("luma@0,0", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const double brightnessValue = doc["probes"][0]["value"].get<double>();
    const double lumaValue       = doc["probes"][1]["value"].get<double>();

    CHECK(brightnessValue == Catch::Approx(255.0 / 765.0).margin(0.01));   // ~0.333
    CHECK(lumaValue       == Catch::Approx(0.7152).margin(0.01));          // Rec.709 G weight
    CHECK(lumaValue - brightnessValue > 0.3);   // clearly, not marginally, different
}

// ---------------------------------------------------------------------------
// Coverage beyond the pinned Task-7 cases above: the remaining probe kinds,
// WriteTo, and -- the one that matters most -- a sweep that would actually
// FAIL if the value/entity-xor-error invariant ever broke, rather than
// merely happening to hold for whichever cases someone thought to write.
// ---------------------------------------------------------------------------

TEST_CASE("verify: rgba probe reads all four channels", "[verify]")
{
    const std::vector<unsigned char> rgba = { 10, 20, 30, 40 };   // 1x1 capture
    Arcane::VerifyReport rep;
    rep.SetRun("Vulkan", 1, "frames-complete");
    rep.SetCapture(1, 1, rgba);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("rgba@0,0", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& v = doc["probes"][0]["value"];
    CHECK(v["r"] == 10);
    CHECK(v["g"] == 20);
    CHECK(v["b"] == 30);
    CHECK(v["a"] == 40);
}

// Fix 3 (final fix wave): "windowed" is GONE, not just untested. It used to
// be reachable only by calling SetRun's old bool `offscreen` parameter with
// false directly -- never through any live host, since HostConfig::Parse
// refuses --report without --headless unconditionally, for every host. That
// made "windowed" a value an out-of-process consumer (the Servitor package
// this JSON is the boundary for) would have had to handle for a state no
// real run could ever produce. SetRun no longer takes the parameter at all,
// so this is now a compile-time guarantee, not just a runtime one -- ToJson
// always emits one value.
//
// THAT VALUE IS NOW "headless", not "offscreen" (schemaVersion 3): the MODE is
// named --headless on every host's command line, and "offscreen" is the
// TECHNIQUE that mode happens to render with (CreateOffscreen, IsOffscreen).
// A wire value an out-of-process consumer switches on must carry the mode's
// own word, and a schemaVersion bump is exactly the versioned moment at which
// changing one is legitimate.
TEST_CASE("verify: mode is always headless -- there is no windowed report", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 42, "window-closed");
    const auto doc = nlohmann::json::parse(rep.ToJson());
    CHECK(doc["mode"] == "headless");
    CHECK(doc["framesRendered"] == 42);
    CHECK(doc["exitReason"] == "window-closed");
}

TEST_CASE("verify: a census probe reads AddCensus's data when set, and refuses when not", "[verify]")
{
    std::string err;

    // Not set: an honest refusal, not zeros that would read as a measured
    // empty scene.
    {
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.Evaluate({ *Arcane::ParseProbe("census", err) });
        const auto doc = nlohmann::json::parse(rep.ToJson());
        CHECK(doc["probes"][0].contains("error"));
        CHECK_FALSE(doc["probes"][0].contains("value"));
        CHECK_FALSE(doc.contains("census"));   // no top-level census either
    }

    // Set: the census probe's value matches the top-level census object,
    // both sourced from the same AddCensus call.
    {
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.AddCensus(/*spriteReferenced=*/4, /*spriteBound=*/1, /*postReferenced=*/false,
                      /*postBound=*/false, /*meshReferenced=*/1, /*meshBound=*/1);
        rep.Evaluate({ *Arcane::ParseProbe("census", err) });
        const auto doc = nlohmann::json::parse(rep.ToJson());

        REQUIRE(doc.contains("census"));
        CHECK(doc["census"]["spriteReferenced"] == 4);
        CHECK(doc["census"]["spriteBound"] == 1);
        CHECK(doc["census"]["postReferenced"] == false);
        CHECK(doc["census"]["postBound"] == false);
        CHECK(doc["census"]["meshReferenced"] == 1);
        CHECK(doc["census"]["meshBound"] == 1);

        const auto& probeValue = doc["probes"][0]["value"];
        CHECK_FALSE(doc["probes"][0].contains("error"));
        CHECK(probeValue["spriteReferenced"] == 4);
        CHECK(probeValue["meshBound"] == 1);
    }
}

TEST_CASE("verify: a pick probe is an honest refusal when SetPick was never called -- no fabricated entity id", "[verify]")
{
    // Task 9 gave Pick a real channel (SetPick), unlike Task 7 where none
    // existed at all. But a run that never armed a pick@ readback (or a
    // caller that evaluates before calling SetPick(), same as the header's
    // "call setters before Evaluate" contract for SetCapture/AddCensus) must
    // still refuse honestly rather than reporting a fabricated id -- this is
    // the same invariant Census's "not set" case upholds. Stays an error even
    // with a capture set, so an agent never mistakes "no pick armed" for
    // "measured zero".
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 1, "frames-complete");
    rep.SetCapture(1, 1, { 1, 2, 3, 4 });

    std::string err;
    const auto probe = Arcane::ParseProbe("pick@0,0", err);
    REQUIRE(probe.has_value());
    rep.Evaluate({ *probe });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    CHECK(doc["probes"][0]["kind"] == "pick");
    CHECK(doc["probes"][0].contains("error"));
    CHECK_FALSE(doc["probes"][0].contains("value"));
    CHECK_FALSE(doc["probes"][0].contains("entity"));
}

TEST_CASE("verify: FirstPickProbe finds the first pick@ spec among raw probes, skipping non-pick and malformed ones", "[verify]")
{
    CHECK_FALSE(Arcane::FirstPickProbe({}).has_value());
    CHECK_FALSE(Arcane::FirstPickProbe({ "brightness@0,0", "census" }).has_value());

    const auto found = Arcane::FirstPickProbe({ "brightness@0,0", "pick@640,360", "pick@1,1" });
    REQUIRE(found.has_value());
    CHECK(found->kind == Arcane::ProbeKind::Pick);
    CHECK(found->x == 640);
    CHECK(found->y == 360);

    // A malformed entry ahead of a real pick@ spec is skipped, not fatal --
    // the real parse-and-log pass (RuntimeApp.cpp's ShutdownGraphPath)
    // reports that malformed one separately; this helper only answers "is
    // there a pick request" and must not double that log.
    const auto skipsMalformed = Arcane::FirstPickProbe({ "bogus@1,2", "pick@10,20" });
    REQUIRE(skipsMalformed.has_value());
    CHECK(skipsMalformed->x == 10);
    CHECK(skipsMalformed->y == 20);
}

TEST_CASE("verify: SetPick(landed=false), surface UNKNOWN (no width/height given), reports the combined NO READBACK LANDED wording", "[verify]")
{
    // surfaceWidth/surfaceHeight default to 0/0 -- the "unknown" sentinel --
    // so this exercises the fallback branch: VerifyReport cannot tell
    // out-of-range from too-short without a real surface size, and says so
    // honestly rather than guessing.
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.SetPick(/*armedX=*/640, /*armedY=*/360, /*landed=*/false,
                /*hitProxyId=*/0, /*resolved=*/false, "", "");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    CHECK_FALSE(entry.contains("entity"));
    CHECK(entry["error"].get<std::string>().find("NO READBACK LANDED") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Fix round 1's design question: "is 'no capture was ever set' the same
// condition as 'the requested pixel is outside the capture'?" -- no. Applied
// to Pick: "the readback never landed" (too short) and "the pixel is outside
// the surface" (structural, can never land) are two different facts, and an
// agent needs to tell them apart -- exactly the split Brightness/Luma/Rgba's
// ReadTexel already makes between "no capture set" and "pixel outside
// capture". These two cases pin that split for Pick.
// ---------------------------------------------------------------------------

TEST_CASE("verify: SetPick(landed=false), surface KNOWN and the armed pixel is inside it, still reports the too-short wording", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.SetPick(640, 360, /*landed=*/false, 0, false, "", "", /*surfaceWidth=*/1280, /*surfaceHeight=*/720);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    const std::string& msg = entry["error"].get_ref<const std::string&>();
    CHECK(msg.find("NO READBACK LANDED") != std::string::npos);
    CHECK(msg.find("too short") != std::string::npos);
    // The surface IS known and the pixel IS inside it, so this must NOT be
    // misreported as an out-of-range refusal.
    CHECK(msg.find("outside the") == std::string::npos);
}

TEST_CASE("verify: SetPick(landed=false), surface KNOWN and the armed pixel is OUTSIDE it, reports a distinct out-of-range fact, not the too-short wording", "[verify]")
{
    // pick@9999,9999 against a 1280x720 surface -- structurally can never
    // land no matter how many frames the run had. Must not be conflated with
    // "too short".
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.SetPick(9999, 9999, /*landed=*/false, 0, false, "", "", /*surfaceWidth=*/1280, /*surfaceHeight=*/720);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@9999,9999", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    const std::string& msg = entry["error"].get_ref<const std::string&>();
    CHECK(msg.find("outside the 1280x720 surface") != std::string::npos);
    CHECK(msg.find("never land") != std::string::npos);
    // Distinct vocabulary from the too-short case -- an agent parsing this
    // must be able to tell the two apart without fragile substring games.
    CHECK(msg.find("too short") == std::string::npos);
}

TEST_CASE("verify: a background pick miss is a FACT (id 0 as the nil Guid), never an error", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.SetPick(20, 700, /*landed=*/true, /*hitProxyId=*/0, /*resolved=*/false, "", "");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@20,700", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    CHECK_FALSE(entry.contains("error"));
    REQUIRE(entry.contains("entity"));
    CHECK(entry["entity"].is_null());
    CHECK(entry["id"] == "00000000-0000-0000-0000-000000000000");
    CHECK(entry["hitProxyId"] == 0);
}

TEST_CASE("verify: a resolved pick reports the durable name+Guid, and the raw hit-proxy separately", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.SetPick(640, 360, /*landed=*/true, /*hitProxyId=*/3, /*resolved=*/true,
                "TealQuad", "3f2504e0-4f89-11d3-9a0c-0305e82c3301");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    CHECK_FALSE(entry.contains("error"));
    CHECK(entry["entity"] == "TealQuad");
    CHECK(entry["id"] == "3f2504e0-4f89-11d3-9a0c-0305e82c3301");
    CHECK(entry["hitProxyId"] == 3);
}

TEST_CASE("verify: a pick hit that could not resolve an Identity is an honest error, never a churny raw id", "[verify]")
{
    // hitProxyId is frame-scoped and meaningless across runs -- reporting it
    // under `id` (the field a durable-identity contract promises) would be a
    // stable-looking value that silently churns between runs. An error says
    // so instead.
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.SetPick(640, 360, /*landed=*/true, /*hitProxyId=*/3, /*resolved=*/false, "", "");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    CHECK_FALSE(entry.contains("entity"));
    CHECK_FALSE(entry.contains("value"));
    CHECK(entry["error"].get<std::string>().find("Identity") != std::string::npos);
}

TEST_CASE("verify: a pick@ probe at a pixel other than the one armed refuses rather than answering the wrong question", "[verify]")
{
    // Only the FIRST pick@x,y spec in a run is ever armed (FirstPickProbe) --
    // a second, differently-positioned pick@ probe in the same run's --probe
    // list must not silently be answered with the FIRST one's hit-proxy.
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.SetPick(640, 360, /*landed=*/true, /*hitProxyId=*/3, /*resolved=*/true, "TealQuad", "guid");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@10,10", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    CHECK_FALSE(entry.contains("entity"));
    CHECK(entry["error"].get<std::string>().find("never probed") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Fix round 1, item 2: PickPixelInRange -- the out-of-range guard
// RuntimeApp.cpp uses BEFORE trusting NriGraphContext::ProbeId(), because
// --pick-probe's own out-of-range latch (m_probeOutOfRange) is keyed to the
// OLD flag's state and is never armed on this path. Without this guard,
// `pick@9999,9999` would confidently report the CLAMPED EDGE TEXEL's entity
// as if it were a measurement of the pixel actually asked about.
// ---------------------------------------------------------------------------

TEST_CASE("verify: PickPixelInRange accepts every pixel inside the surface, including its edges", "[verify]")
{
    CHECK(Arcane::PickPixelInRange(0, 0, 1280, 720));
    CHECK(Arcane::PickPixelInRange(1279, 719, 1280, 720));      // last valid texel, both axes
    CHECK(Arcane::PickPixelInRange(640, 360, 1280, 720));
}

TEST_CASE("verify: PickPixelInRange refuses the whole out-of-range family -- past either edge, and negative", "[verify]")
{
    CHECK_FALSE(Arcane::PickPixelInRange(1280, 0, 1280, 720));   // x == width (one past the last column)
    CHECK_FALSE(Arcane::PickPixelInRange(0, 720, 1280, 720));    // y == height (one past the last row)
    CHECK_FALSE(Arcane::PickPixelInRange(9999, 9999, 1280, 720));
    // Negative coordinates never reach here through ParseProbe (which only
    // accepts non-negative ones), but a caller that bypassed that parse must
    // not be answered either.
    CHECK_FALSE(Arcane::PickPixelInRange(-1, 0, 1280, 720));
    CHECK_FALSE(Arcane::PickPixelInRange(0, -1, 1280, 720));
}

// ---------------------------------------------------------------------------
// Fix round 1, item 4: the cheap mesh-pick mitigation. CollectPickables
// (PickEmit.hpp) only ever walks SpriteRenderer/Collider2D entities -- a
// MeshRenderer entity is invisible to it -- so a hit is ALWAYS one of those
// two kinds, never a mesh. `pickableKinds` names that capability on every
// non-error pick result; `meshesNotPickable` additionally flags the run
// whenever the scene's own census (already carried by this function) reports
// a bound mesh, converting a silent wrong answer (e.g. Task 9's "Ground" for
// a teal MeshRenderer at the same screen position) into a visibly-qualified
// one.
// ---------------------------------------------------------------------------

TEST_CASE("verify: a resolved pick names its pickable kinds, and flags meshesNotPickable when the scene has a bound mesh", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.AddCensus(/*spriteReferenced=*/4, /*spriteBound=*/1, /*postReferenced=*/true,
                  /*postBound=*/true, /*meshReferenced=*/1, /*meshBound=*/1);
    rep.SetPick(640, 360, /*landed=*/true, /*hitProxyId=*/1, /*resolved=*/true,
                "Ground", "acd0708f-ffc1-47e9-a105-b10e0b3f8497");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("pickableKinds"));
    CHECK(entry["pickableKinds"] == std::vector<std::string>{ "sprite", "collider2d" });
    REQUIRE(entry.contains("meshesNotPickable"));
    CHECK(entry["meshesNotPickable"] == true);
}

TEST_CASE("verify: meshesNotPickable is omitted, not false, when the scene's census carries no bound mesh", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.AddCensus(4, 1, false, false, /*meshReferenced=*/0, /*meshBound=*/0);
    rep.SetPick(640, 360, true, 1, true, "Ground", "acd0708f-ffc1-47e9-a105-b10e0b3f8497");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    // `doc` OWNS the parsed json; `entry` is a reference INTO it, not into a
    // temporary -- binding `const auto&` straight to `parse(...)["probes"][0]`
    // in one expression dangles (operator[] chained on a temporary does not
    // lifetime-extend it), which is exactly the bug fix round 1 found here.
    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    CHECK(entry.contains("pickableKinds"));       // always present on a result
    CHECK_FALSE(entry.contains("meshesNotPickable"));   // omitted, not `false`
}

TEST_CASE("verify: a background pick miss also carries pickableKinds -- the mitigation applies to both result branches", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.AddCensus(4, 1, true, true, 1, 1);
    rep.SetPick(20, 700, /*landed=*/true, /*hitProxyId=*/0, /*resolved=*/false, "", "");

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@20,700", err) });

    // `doc` OWNS the parsed json; `entry` is a reference INTO it, not into a
    // temporary -- binding `const auto&` straight to `parse(...)["probes"][0]`
    // in one expression dangles (operator[] chained on a temporary does not
    // lifetime-extend it), which is exactly the bug fix round 1 found here.
    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    CHECK(entry["entity"].is_null());
    REQUIRE(entry.contains("pickableKinds"));
    CHECK(entry["pickableKinds"] == std::vector<std::string>{ "sprite", "collider2d" });
    REQUIRE(entry.contains("meshesNotPickable"));
    CHECK(entry["meshesNotPickable"] == true);
}

TEST_CASE("verify: an error branch never carries pickableKinds -- it is a result-only field, not a blanket addition", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 60, "frames-complete");
    rep.AddCensus(4, 1, true, true, 1, 1);
    rep.SetPick(640, 360, /*landed=*/false, 0, false, "", "");   // NO READBACK LANDED

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("pick@640,360", err) });

    // `doc` OWNS the parsed json; `entry` is a reference INTO it, not into a
    // temporary -- binding `const auto&` straight to `parse(...)["probes"][0]`
    // in one expression dangles (operator[] chained on a temporary does not
    // lifetime-extend it), which is exactly the bug fix round 1 found here.
    const auto doc = nlohmann::json::parse(rep.ToJson());
    const auto& entry = doc["probes"][0];
    REQUIRE(entry.contains("error"));
    CHECK_FALSE(entry.contains("pickableKinds"));
    CHECK_FALSE(entry.contains("meshesNotPickable"));
}

TEST_CASE("verify: every pick branch carries exactly one of entity or error, never both, never neither", "[verify]")
{
    // The XOR invariant, exercised across every branch Evaluate's Pick case
    // can take -- not just the ones the cases above happen to cover. If a
    // future edit ever sets both fields (or neither) on any of these, this
    // fails.
    std::string err;
    const auto spec           = *Arcane::ParseProbe("pick@640,360", err);
    const auto mismatchedSpec = *Arcane::ParseProbe("pick@1,1", err);

    auto checkXor = [](const nlohmann::json& entry)
    {
        const bool hasResult = entry.contains("value") || entry.contains("entity");
        const bool hasError  = entry.contains("error");
        CHECK(hasResult != hasError);
    };

    {   // SetPick never called.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Mismatched pixel.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.SetPick(640, 360, true, 3, true, "X", "guid");
        rep.Evaluate({ mismatchedSpec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Not landed.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.SetPick(640, 360, false, 0, false, "", "");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Background miss.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.SetPick(640, 360, true, 0, false, "", "");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Hit, unresolved.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.SetPick(640, 360, true, 3, false, "", "");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
    {   // Hit, resolved.
        Arcane::VerifyReport rep;
        rep.SetRun("D3D12", 1, "frames-complete");
        rep.SetPick(640, 360, true, 3, true, "X", "guid");
        rep.Evaluate({ spec });
        checkXor(nlohmann::json::parse(rep.ToJson())["probes"][0]);
    }
}

TEST_CASE("verify: every probe entry carries exactly one of value/entity or error, never both, never neither", "[verify]")
{
    // The invariant the whole report exists to uphold, exercised across every
    // kind and every set/unset combination of the data those kinds read --
    // brightness/luma/rgba with and without a capture, in and out of bounds;
    // census with and without AddCensus; pick, always refused here (SetPick
    // is never called in this sweep -- see the dedicated pick branch sweep
    // below for SetPick's own states). If a future edit to Evaluate() ever
    // sets both fields, or neither, on ANY branch --
    // not just the ones the pinned Task-7 cases happen to cover -- this fails.
    std::string err;
    const std::vector<Arcane::ProbeSpec> specs = {
        *Arcane::ParseProbe("brightness@0,0", err),
        *Arcane::ParseProbe("brightness@50,50", err),   // will be out of bounds
        *Arcane::ParseProbe("luma@0,0", err),
        *Arcane::ParseProbe("luma@50,50", err),          // out of bounds
        *Arcane::ParseProbe("rgba@0,0", err),
        *Arcane::ParseProbe("rgba@50,50", err),          // out of bounds
        *Arcane::ParseProbe("pick@0,0", err),            // always deferred
        *Arcane::ParseProbe("census", err),
    };

    for (const bool withCapture : { false, true })
    {
        for (const bool withCensus : { false, true })
        {
            Arcane::VerifyReport rep;
            rep.SetRun("D3D12", 1, "frames-complete");
            if (withCapture)
                rep.SetCapture(2, 2, { 0,0,0,255, 10,10,10,255, 20,20,20,255, 30,30,30,255 });
            if (withCensus)
                rep.AddCensus(1, 1, false, false, 1, 1);

            rep.Evaluate(specs);
            const auto doc = nlohmann::json::parse(rep.ToJson());
            REQUIRE(doc["probes"].size() == specs.size());
            for (const auto& entry : doc["probes"])
            {
                const bool hasResult = entry.contains("value") || entry.contains("entity");
                const bool hasError  = entry.contains("error");
                INFO("probe raw=" << entry["raw"].get<std::string>()
                     << " withCapture=" << withCapture << " withCensus=" << withCensus);
                CHECK(hasResult != hasError);
            }
        }
    }
}

TEST_CASE("verify: WriteTo round-trips through disk", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 3, "frames-complete");
    rep.SetCapture(1, 1, { 100, 150, 200, 255 });
    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("luma@0,0", err) });

    const auto path = (std::filesystem::temp_directory_path() / "arcane_verify_report_test.json").string();
    REQUIRE(rep.WriteTo(path));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream contents;
    contents << in.rdbuf();
    in.close();

    const auto doc = nlohmann::json::parse(contents.str());
    CHECK(doc["schemaVersion"] == 3);
    CHECK(doc["framesRendered"] == 3);

    std::remove(path.c_str());
}

TEST_CASE("verify: WriteTo fails, does not throw, when the path cannot be opened", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", 1, "frames-complete");
    // A directory that does not exist -- std::ofstream cannot create the
    // intermediate path component, so the open fails.
    const auto path = (std::filesystem::temp_directory_path() /
                        "arcane_verify_report_test_missing_dir" / "report.json").string();
    CHECK_FALSE(rep.WriteTo(path));
}

// ---- Task 8: --compare/--bless inside the settle loop, report schema 2 ----

TEST_CASE("verify: the report schema is version 3 once settle facts exist", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "frames-complete");
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["schemaVersion"] == 3);
}

TEST_CASE("verify: a run with no --compare emits NO compare block", "[verify]")
{
    // Absence must be absence. An agent must be able to tell "no comparison was
    // asked for" from "a comparison ran and passed".
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "frames-complete");
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK_FALSE(doc.contains("compare"));
}

TEST_CASE("verify: a passing comparison reports its facts", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("vulkan", 60, "frames-complete");
    r.SetCompare("runtime-scene", "shared",
                 "ReferenceProject/Verify/References/runtime-scene.png",
                 /*passed*/ true, /*diffCount*/ 0, /*diffRatio*/ 0.0,
                 /*maxDiffPixels*/ 0, /*sizesMismatch*/ false,
                 /*diffPath*/ "", /*errorMessage*/ "");

    const auto doc = nlohmann::json::parse(r.ToJson());
    REQUIRE(doc.contains("compare"));
    CHECK(doc["compare"]["reference"] == "runtime-scene");
    CHECK(doc["compare"]["resolvedLevel"] == "shared");
    CHECK(doc["compare"]["passed"] == true);
    CHECK(doc["compare"]["diffCount"] == 0);
    // No diff artifact is written on success, so the field is empty, not absent.
    CHECK(doc["compare"]["diffPath"] == "");
}

TEST_CASE("verify: a failing comparison carries the count, the budget and the diff path", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "compare-failed");
    r.SetCompare("editor-ui", "backend",
                 "ReferenceProject/Verify/References/dx12/editor-ui.png",
                 false, 1234, 0.0134, 0, false,
                 "ReferenceProject/Saved/Verify/editor-ui-dx12-diff.png",
                 "1234 pixels (ratio 0.013400 of all image pixels) are different.");

    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["compare"]["passed"] == false);
    CHECK(doc["compare"]["diffCount"] == 1234);
    CHECK(doc["compare"]["maxDiffPixels"] == 0);
    CHECK(doc["compare"]["diffPath"] != "");
    CHECK(doc["exitReason"] == "compare-failed");
}

TEST_CASE("verify: a missing reference is its own resolvedLevel, not a zero-diff pass", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "compare-missing-reference");
    r.SetCompare("brand-new", "none", "", false, 0, 0.0, 0, false, "",
                 "no reference image on disk; re-run with --bless to create one");

    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["compare"]["resolvedLevel"] == "none");
    CHECK(doc["compare"]["passed"] == false);
    CHECK(doc["compare"]["diffCount"] == 0);   // NOT a pass despite zero diffs
}

TEST_CASE("verify: a --bless run's compare block reports a pass at the level it blessed to", "[verify]")
{
    // Not one of the brief's pinned cases -- added because SetCompare's
    // contract has to cover the --bless caller too (RuntimeApp::
    // ShutdownGraphPath calls it on every --compare run, blessed or not),
    // and nothing above exercises "passed because we just wrote the
    // reference", as opposed to "passed because it genuinely matched".
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "compare-blessed");
    r.SetCompare("runtime-scene", "shared",
                 "ReferenceProject/Verify/References/runtime-scene.png",
                 /*passed*/ true, 0, 0.0, 0, false, "", "");

    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["exitReason"] == "compare-blessed");
    CHECK(doc["compare"]["passed"] == true);
    CHECK(doc["compare"]["resolvedLevel"] == "shared");
}

// ---- Task 3: schemaVersion 3 -- the settle facts, and the headless mode ----

TEST_CASE("verify report: schemaVersion 3 carries settle facts and the headless mode", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    r.SetSettle(94, /*converged=*/false, Arcane::SettleBail::TimeoutBound,
                /*captureFailed=*/false);
    const auto doc = nlohmann::json::parse(r.ToJson());

    CHECK(doc["schemaVersion"] == 3);
    // The MODE's machine-readable name, in the mode's own word. Changed on this
    // bump because a schemaVersion bump is exactly when a wire value may change.
    CHECK(doc["mode"] == "headless");
    CHECK(doc["settleAttemptsUsed"] == 94);
    CHECK(doc["settleBailReason"] == "timeout-bound");
}

// NOT "reports its real count, not its budget" (fix round 1, item 4): this TU
// links neither host, and VerifyReport never sees a budget -- SetSettle(7, ...)
// yielding 7 is argument pass-through, so a case with that title could not fail
// for the reason the title gives. The budget-forging hazard lives in the hosts
// and is desk-checked there. What this case CAN pin is the converged ->
// "converged" mapping and that the count round-trips into the JSON at all.
TEST_CASE("verify report: a converged run maps to \"converged\" and round-trips its count", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    r.SetSettle(7, /*converged=*/true, Arcane::SettleBail::Keep, /*captureFailed=*/false);
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["settleAttemptsUsed"] == 7);
    CHECK(doc["settleBailReason"] == "converged");
}

TEST_CASE("verify report: settle keys are ABSENT when settle was never asked for", "[verify]")
{
    // Absence-is-absence, the same contract SetCapture and SetCompare uphold:
    // an agent must be able to tell "not asked" from "asked and converged".
    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK_FALSE(doc.contains("settleAttemptsUsed"));
    CHECK_FALSE(doc.contains("settleBailReason"));
}

TEST_CASE("verify report: the attempts bound is named when it governed", "[verify]")
{
    // The two bounds are DIFFERENT KNOBS -- "attempts-bound" says raise
    // --settle, "timeout-bound" says raise --settle-timeout. A report that
    // named the wrong one would send an agent to the knob that cannot help.
    Arcane::VerifyReport r;
    r.SetRun("vulkan", 60, "settle-not-converged");
    r.SetSettle(30, /*converged=*/false, Arcane::SettleBail::AttemptsBound,
                /*captureFailed=*/false);
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["settleAttemptsUsed"] == 30);
    CHECK(doc["settleBailReason"] == "attempts-bound");
}

TEST_CASE("verify report: a broken capture path OUTRANKS whichever bound was spent", "[verify]")
{
    // No readback ever landed, so the loop never had two frames to compare and
    // NEITHER bound got a fair test. "capture-failed" is a different fact from
    // "the scene never stabilised" -- one is a broken capture path, the other a
    // genuinely unstable scene, and an agent has to tell them apart. So it wins
    // the ternary even though the bail decision itself still names a bound.
    Arcane::VerifyReport r;
    r.SetRun("dx12", 60, "settle-not-converged");
    r.SetSettle(30, /*converged=*/false, Arcane::SettleBail::AttemptsBound,
                /*captureFailed=*/true);
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["settleBailReason"] == "capture-failed");
}

TEST_CASE("verify report: a run that died BEFORE the settle loop emits no settle keys", "[verify]")
{
    // FIX ROUND 1, ITEM 1 -- the case that used to emit a FALSE "timeout-bound".
    // A --compare reference that is missing or undecodable fails FAST, at
    // RuntimeApp::MainLoop's pre-loop resolve, before the settle loop runs even
    // once; device-lost, render-failed and validation-errors all exit the same
    // way. Such a run still has settleAttempts != 0, but the bail decision was
    // never evaluated, so SettleBail is untouched at Keep. Guarding emission on
    // the attempt count alone reported settleAttemptsUsed 0 with a
    // settleBailReason of "timeout-bound" -- an instruction to raise
    // --settle-timeout on a run that never spent one, for a fault that was
    // actually a missing file.
    //
    // No verdict, no keys. exitReason carries what really happened.
    Arcane::VerifyReport r;
    r.SetRun("dx12", 0, "compare-missing-reference");
    r.SetSettle(0, /*converged=*/false, Arcane::SettleBail::Keep, /*captureFailed=*/false);
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK_FALSE(doc.contains("settleAttemptsUsed"));
    CHECK_FALSE(doc.contains("settleBailReason"));
    // The run's own account of itself is untouched -- absence here costs an
    // agent nothing it did not already have.
    CHECK(doc["exitReason"] == "compare-missing-reference");
}

TEST_CASE("verify report: captureFailed alone is not a verdict", "[verify]")
{
    // captureFailed is a QUALIFIER on a bail, not a verdict of its own: both
    // hosts write it only inside the bail branch, so SettleBail::Keep with
    // captureFailed set is an incoherent pair no run can produce ("no readback
    // landed when we gave up" + "we never gave up"). Pinned because the ranking
    // in ToJson puts captureFailed FIRST, and it would be easy to conclude from
    // that it can stand alone. It cannot -- the no-verdict rule wins.
    Arcane::VerifyReport r;
    r.SetRun("dx12", 12, "stopped-early");
    r.SetSettle(3, /*converged=*/false, Arcane::SettleBail::Keep, /*captureFailed=*/true);
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK_FALSE(doc.contains("settleAttemptsUsed"));
    CHECK_FALSE(doc.contains("settleBailReason"));
}
