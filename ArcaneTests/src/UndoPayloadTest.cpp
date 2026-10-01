// Undo payloads + disk spill (spec 2026-09-30 s3.3e) -- CPU only, temp dirs.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/UndoPayload.hpp>
#include <Arcane/Guid.hpp>

#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    struct TempDir
    {
        fs::path path = fs::temp_directory_path() / ("arcane-undo-" + Arcane::Guid::Generate().ToString());
        ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    };

    std::vector<std::byte> Bytes(std::size_t n, unsigned char seed)
    {
        std::vector<std::byte> b(n);
        for (std::size_t i = 0; i < n; ++i) b[i] = std::byte(static_cast<unsigned char>(seed + i));
        return b;
    }

    void WriteFile(const fs::path& p, const std::vector<std::byte>& b)
    {
        std::ofstream o(p, std::ios::binary);
        o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }

    struct PayloadCommand final : Arcane::ICommand
    {
        std::vector<Arcane::UndoPayload> payloads;
        void Undo() override {}
        void Redo() override {}
        const char* Label() const override { return "payload"; }
        std::size_t PayloadBytes() const override
        {
            std::size_t n = 0;
            for (const auto& p : payloads) n += p.Size();
            return n;
        }
    };

    struct Fixture
    {
        TempDir dir;                     // declared first: outlives the stack
        Astra::Registry registry;
        Arcane::CommandStack stack{[this]() -> Astra::Registry& { return registry; }};

        explicit Fixture(std::uint64_t threshold = 16)
        {
            Arcane::UndoLimits l;
            l.spillThreshold = threshold;
            stack.SetLimits(l);
            stack.SetSpillDirectory(dir.path);
        }
        Arcane::UndoPayload Make(std::size_t n, unsigned char seed = 1) { return stack.MakePayload(Bytes(n, seed)); }
        void PushStep(std::vector<Arcane::UndoPayload> ps)
        {
            auto c = std::make_unique<PayloadCommand>();
            c->payloads = std::move(ps);
            stack.Push(std::move(c));
        }
    };
}

TEST_CASE("UndoPayload: above the threshold it spills to <step>.bin and round-trips", "[edit][undo][spill]")
{
    Fixture fx;
    const Arcane::UndoPayload p = fx.Make(64, 7);
    REQUIRE(p.Spilled());
    CHECK(p.Size() == 64);
    CHECK(p.SpillPath().parent_path() == fx.dir.path);
    CHECK(p.SpillPath().extension() == ".bin");
    CHECK(fs::exists(p.SpillPath()));
    const auto back = p.Load();
    REQUIRE(back.has_value());
    CHECK(*back == Bytes(64, 7));

    const Arcane::UndoPayload small = fx.Make(16, 3);   // AT the threshold: memory
    CHECK_FALSE(small.Spilled());
    CHECK(*small.Load() == Bytes(16, 3));
}

TEST_CASE("UndoPayload: two spilled payloads of one step share its file; the next step gets its own", "[edit][undo][spill]")
{
    Fixture fx;
    std::vector<Arcane::UndoPayload> ps;
    ps.push_back(fx.Make(40, 1));
    ps.push_back(fx.Make(50, 2));
    const fs::path first = ps[0].SpillPath();
    CHECK(ps[1].SpillPath() == first);
    CHECK(*ps[0].Load() == Bytes(40, 1));
    CHECK(*ps[1].Load() == Bytes(50, 2));
    fx.PushStep(std::move(ps));

    const Arcane::UndoPayload next = fx.Make(40, 3);
    CHECK(next.SpillPath() != first);
}

TEST_CASE("UndoPayload: the step's file is deleted on eviction, Clear and redo truncation", "[edit][undo][spill]")
{
    Fixture fx;
    std::vector<Arcane::UndoPayload> ps;
    ps.push_back(fx.Make(64));
    const fs::path file = ps[0].SpillPath();
    fx.PushStep(std::move(ps));
    REQUIRE(fs::exists(file));

    SECTION("eviction")
    {
        Arcane::UndoLimits l = fx.stack.Limits();
        l.maxSteps = 1;
        fx.stack.SetLimits(l);
        fx.PushStep({});
        CHECK_FALSE(fs::exists(file));
    }
    SECTION("Clear")
    {
        fx.stack.Clear("test");
        CHECK_FALSE(fs::exists(file));
    }
    SECTION("redo truncation")
    {
        fx.stack.Undo();
        REQUIRE(fs::exists(file));
        fx.PushStep({});
        CHECK_FALSE(fs::exists(file));
    }
}

TEST_CASE("UndoPayload: spilled bytes count toward the byte budget", "[edit][undo][spill]")
{
    Fixture fx;
    Arcane::UndoLimits l = fx.stack.Limits();
    l.byteBudget = 100;
    fx.stack.SetLimits(l);
    std::vector<Arcane::UndoPayload> a;
    a.push_back(fx.Make(60));
    REQUIRE(a[0].Spilled());
    const fs::path fa = a[0].SpillPath();
    fx.PushStep(std::move(a));
    std::vector<Arcane::UndoPayload> b;
    b.push_back(fx.Make(60));
    fx.PushStep(std::move(b));
    CHECK_FALSE(fs::exists(fa));                 // 120 > 100: the oldest went, file and all
    fx.stack.Undo();
    CHECK_FALSE(fx.stack.CanUndo());
}

TEST_CASE("MakePayloadFromFile: streams to the spill file above the threshold, reads to memory below", "[edit][undo][spill]")
{
    Fixture fx(1024);
    fs::create_directories(fx.dir.path);
    const std::vector<std::byte> bigData = Bytes((3u << 20) + 5u, 11);   // > 3 one-MB chunks
    const std::vector<std::byte> smallData = Bytes(100, 12);
    WriteFile(fx.dir.path / "big.src", bigData);
    WriteFile(fx.dir.path / "small.src", smallData);

    const auto big = fx.stack.MakePayloadFromFile(fx.dir.path / "big.src");
    REQUIRE(big.has_value());
    CHECK(big->Spilled());
    CHECK(big->Size() == bigData.size());
    CHECK(*big->Load() == bigData);

    const auto small = fx.stack.MakePayloadFromFile(fx.dir.path / "small.src");
    REQUIRE(small.has_value());
    CHECK_FALSE(small->Spilled());
    CHECK(*small->Load() == smallData);

    CHECK_FALSE(fx.stack.MakePayloadFromFile(fx.dir.path / "missing.src").has_value());
}

TEST_CASE("UndoPayload: an empty spill directory means memory-only; a failed write keeps it in memory", "[edit][undo][spill]")
{
    Fixture fx;
    SECTION("empty directory")
    {
        fx.stack.SetSpillDirectory({});
        CHECK_FALSE(fx.Make(4096).Spilled());
    }
    SECTION("failed write")
    {
        fs::create_directories(fx.dir.path);
        const fs::path occupied = fx.dir.path / "occupied";
        WriteFile(occupied, Bytes(1, 0));
        fx.stack.SetSpillDirectory(occupied);    // a FILE: no <step>.bin can live under it
        const Arcane::UndoPayload p = fx.Make(64, 9);
        CHECK_FALSE(p.Spilled());
        CHECK(*p.Load() == Bytes(64, 9));
    }
}
