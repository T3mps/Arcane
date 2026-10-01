// Node page + editor upgrades s3.2: the PreviewStatus model -- the compile and
// preview precedence tables and their report ids. Pure, no ImGui, no device.
#include <catch2/catch_test_macros.hpp>
#include <Documents/PreviewStatus.hpp>

#include <string>

using namespace Arcane::Editor;

TEST_CASE("PreviewStatus: a completed compile with no seam reads ok / no-device, never compiling", "[editor][preview]")
{
    PreviewStatusInputs in;              // compiler available, nothing in flight, no seam
    const PreviewStatus s = ComputePreviewStatus(in);
    CHECK(s.compile == CompileStatus::Ok);
    CHECK(s.preview == PreviewAvailability::NoDevice);
    CHECK_FALSE(s.image);
    in.jobsInFlight = 2;
    CHECK(ComputePreviewStatus(in).compile == CompileStatus::Compiling);
}

TEST_CASE("PreviewStatus: compile precedence -- not-compiled-here, compiler-unavailable, errors, compiling, ok", "[editor][preview]")
{
    PreviewStatusInputs in;
    in.notCompiledHere = true; in.compilerAvailable = false; in.hasErrors = true; in.jobsInFlight = 1;
    CHECK(ComputePreviewStatus(in).compile == CompileStatus::NotCompiledHere);
    in.notCompiledHere = false;
    CHECK(ComputePreviewStatus(in).compile == CompileStatus::CompilerUnavailable);   // no compiler beats everything below
    in.compilerAvailable = true;
    CHECK(ComputePreviewStatus(in).compile == CompileStatus::Errors);                // errors beat in-flight jobs
    in.hasErrors = false;
    CHECK(ComputePreviewStatus(in).compile == CompileStatus::Compiling);
    in.jobsInFlight = 0;
    CHECK(ComputePreviewStatus(in).compile == CompileStatus::Ok);
}

TEST_CASE("PreviewStatus: preview precedence -- no-device, vehicle-failed, frame-failed, ready", "[editor][preview]")
{
    PreviewStatusInputs in;
    in.vehicleFailed = true; in.frameFailed = true; in.imageBound = true;
    CHECK(ComputePreviewStatus(in).preview == PreviewAvailability::NoDevice);
    CHECK_FALSE(ComputePreviewStatus(in).image);                                     // image needs Ready
    in.deviceSeam = true;
    CHECK(ComputePreviewStatus(in).preview == PreviewAvailability::VehicleFailed);
    in.vehicleFailed = false;
    CHECK(ComputePreviewStatus(in).preview == PreviewAvailability::FrameFailed);
    in.frameFailed = false;
    CHECK(ComputePreviewStatus(in).preview == PreviewAvailability::Ready);
    CHECK(ComputePreviewStatus(in).image);
}

TEST_CASE("PreviewStatus: errors with a bound image keep image true; a frame failure then recovery", "[editor][preview]")
{
    PreviewStatusInputs in;
    in.deviceSeam = true; in.hasErrors = true; in.imageBound = true;
    PreviewStatus s = ComputePreviewStatus(in);
    CHECK(s.compile == CompileStatus::Errors);
    CHECK(s.preview == PreviewAvailability::Ready);
    CHECK(s.image);                                   // the last good image still shows

    in.hasErrors = false; in.frameFailed = true; in.imageBound = false;
    s = ComputePreviewStatus(in);
    CHECK(s.preview == PreviewAvailability::FrameFailed);
    CHECK_FALSE(s.image);
    in.frameFailed = false; in.imageBound = true;     // the next good frame cleared the latch
    s = ComputePreviewStatus(in);
    CHECK(s.preview == PreviewAvailability::Ready);
    CHECK(s.image);
}

TEST_CASE("PreviewStatus: the report ids are lowercase kebab", "[editor][preview]")
{
    CHECK(std::string(CompileStatusId(CompileStatus::NotCompiledHere))     == "not-compiled-here");
    CHECK(std::string(CompileStatusId(CompileStatus::CompilerUnavailable)) == "compiler-unavailable");
    CHECK(std::string(CompileStatusId(CompileStatus::Compiling))           == "compiling");
    CHECK(std::string(CompileStatusId(CompileStatus::Errors))              == "errors");
    CHECK(std::string(CompileStatusId(CompileStatus::Ok))                  == "ok");
    CHECK(std::string(PreviewAvailabilityId(PreviewAvailability::Ready))         == "ready");
    CHECK(std::string(PreviewAvailabilityId(PreviewAvailability::NoDevice))      == "no-device");
    CHECK(std::string(PreviewAvailabilityId(PreviewAvailability::VehicleFailed)) == "vehicle-failed");
    CHECK(std::string(PreviewAvailabilityId(PreviewAvailability::FrameFailed))   == "frame-failed");
}
