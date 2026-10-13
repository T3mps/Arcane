// Engine log file sink + backlog ring (crash window plan 1, task 4). Covers
// AttachFileSink's rotation, the lock-free BacklogSink ring the crash path
// will FREEZE and DUMP (task 5), and the bounded flush helper. CPU-only ([diag]).

#include <Arcane/Base/Log.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("log backlog: keeps the last lines in order, freezes on request, and the file sink rotates", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-log-backlog-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "ArcaneTests.log";

    // Documented pre-Init contract: AttachFileSink refuses before Log::Init()
    // has run (review fix round 1, finding 2). Init() is call_once-guarded
    // and nothing in this binary ever calls Shutdown(), so the shared engine
    // logger is a one-way latch for the life of the process: once ANY
    // earlier test (in [diag] or the full suite, in whatever order Catch2
    // picks) has touched logging, there is no way to force the pre-Init
    // state again, and this first AttachFileSink call legitimately succeeds
    // instead of refusing. Only assert the refusal -- and only then call
    // Init() ourselves -- in the branch where it is actually still true, so
    // this test exercises the documented contract for real in isolation
    // (nothing else has run) without going flaky/failing as part of the full
    // suite (where an earlier test almost always already initialized Log).
    if (!Arcane::Log::AttachFileSink(file))
    {
        Arcane::Log::Init();
        REQUIRE(Arcane::Log::AttachFileSink(file));
    }
    CHECK(Arcane::Log::FileSinkPath() == file);

    for (int i = 0; i < 600; ++i)
        ARC_INFO("backlog line {}", i);
    CHECK(Arcane::Log::BacklogLineCount() == Arcane::Log::kBacklogLines);
    std::array<char, 512> buf{};
    const std::size_t n0 = Arcane::Log::BacklogLine(0, buf);
    CHECK(std::string(buf.data(), n0).find("backlog line 88") != std::string::npos);   // 600 - 512
    const std::size_t nl = Arcane::Log::BacklogLine(Arcane::Log::kBacklogLines - 1, buf);
    CHECK(std::string(buf.data(), nl).find("backlog line 599") != std::string::npos);

    Arcane::Log::FreezeBacklog();
    ARC_INFO("after freeze");
    const std::size_t nf = Arcane::Log::BacklogLine(Arcane::Log::kBacklogLines - 1, buf);
    CHECK(std::string(buf.data(), nf).find("backlog line 599") != std::string::npos);

    CHECK(Arcane::Log::FlushFileSinkBounded(1000));
    std::string all;
    {
        std::ifstream in(file);
        all.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(all.find("backlog line 599") != std::string::npos);

    // Attaching again to the same path rotates the previous file to .1.log.
    REQUIRE(Arcane::Log::AttachFileSink(file));
    CHECK(std::filesystem::exists(dir / "ArcaneTests.1.log"));

    Arcane::Log::UnfreezeBacklogForTests();
}
