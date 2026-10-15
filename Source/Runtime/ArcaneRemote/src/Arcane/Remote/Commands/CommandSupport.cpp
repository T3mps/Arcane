// Crypto.hpp before any spdlog include: it includes Util/Logger.hpp, which
// defines SPDLOG_ACTIVE_LEVEL first.
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Remote/Commands/CommandSupport.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace Arcane::Remote::Detail
{

namespace
{

struct ArtifactDirState {
    std::mutex mutex;
    std::filesystem::path dir;
};

ArtifactDirState& DirState()
{
    static ArtifactDirState state;
    return state;
}

} // namespace

std::filesystem::path ArtifactDir()
{
    ArtifactDirState& state = DirState();
    std::lock_guard lock(state.mutex);
    return state.dir;
}

void SetArtifactDir(const std::filesystem::path& dir)
{
    ArtifactDirState& state = DirState();
    std::lock_guard lock(state.mutex);
    state.dir = dir;
}

void ReleaseArtifactDir(const std::filesystem::path& dir)
{
    ArtifactDirState& state = DirState();
    std::lock_guard lock(state.mutex);
    if (state.dir == dir)
        state.dir.clear();
}

std::optional<Envelope> CheckKeys(const Call& call, std::initializer_list<std::string_view> known,
                                  std::string_view usage)
{
    if (call.args.is_null())
        return std::nullopt;
    if (!call.args.is_object())
        return MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                         std::string(usage));
    for (const auto& [key, value] : call.args.items()) {
        (void)value;
        if (std::find(known.begin(), known.end(), std::string_view(key)) == known.end())
            return MakeError(call.command, "arg.unknown", call.command + " has no argument '" + key + "'",
                             std::string(usage));
    }
    return std::nullopt;
}

Astra::Registry* PickRegistry(const Call& call, Envelope& refusal)
{
    PickedWorld picked = ResolveWorld(CommandWorlds(), call.world);
    if (picked.pick != WorldPick::Ok) {
        refusal = std::move(picked.error);   // the dispatcher stamps call.command
        return nullptr;
    }
    if (picked.world.registry == nullptr) {
        refusal = MakeError(call.command, "host.unavailable", "world '" + picked.world.name + "' has no registry yet",
                            "retry once the host has loaded its scene");
        return nullptr;
    }
    return picked.world.registry;
}

Error MakeIssue(std::string code, std::string message, std::string remediation)
{
    Error e;
    e.code = std::move(code);
    e.message = std::move(message);
    e.remediation = std::move(remediation);
    return e;
}

Envelope ErrorEnvelope(const Call& call, Error issue)
{
    Envelope env = MakeError(call.command, issue.code, issue.message, issue.remediation);
    env.errors.front() = std::move(issue);
    return env;
}

std::optional<Artifact> WriteArtifact(const std::filesystem::path& dir, std::string_view stem, std::string_view kind,
                                      const std::string& json, std::string& why)
{
    static std::atomic<std::uint64_t> serial{0};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::string name = std::string(stem) + "-" + std::to_string(unixMs) + "-" + std::to_string(++serial);
    const std::filesystem::path file = dir / (name + ".json");
    const std::filesystem::path temp = dir / (name + ".json.tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (out.is_open())
            out.write(json.data(), static_cast<std::streamsize>(json.size()));
        // close() flushes: a failed flush (disk full) sets failbit here, so a
        // short file is never renamed and reported with the full size and hash.
        const bool opened = out.is_open();
        out.close();
        if (!opened || out.fail()) {
            std::filesystem::remove(temp, ec);
            why = "cannot write " + temp.string();
            return std::nullopt;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        why = "cannot rename into " + file.string();
        return std::nullopt;
    }
    Artifact artifact;
    artifact.kind = std::string(kind);
    const std::u8string utf8 = file.generic_u8string();
    artifact.path.assign(utf8.begin(), utf8.end());
    artifact.bytes = json.size();
    artifact.sha256 = Crypto::Sha256Hex(json);
    artifact.format = "json";
    return artifact;
}

std::optional<Artifact> WritePngArtifact(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
                                         const unsigned char* rgba, std::string_view kind, std::string& why)
{
    const auto utf8 = [](const std::filesystem::path& p) {
        const std::u8string s = p.generic_u8string();
        return std::string(s.begin(), s.end());
    };
    std::vector<unsigned char> png;
    if (!EncodePngRgba(width, height, rgba, png)) {
        why = "the PNG encoder failed";
        return std::nullopt;
    }
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);   // a failure surfaces as the open below
    const std::filesystem::path temp = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (out.is_open())
            out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        // close() flushes: a full disk sets failbit here, so a short file is
        // never renamed and reported with the full size and hash.
        const bool opened = out.is_open();
        out.close();
        if (!opened || out.fail()) {
            std::filesystem::remove(temp, ec);
            why = "cannot write " + utf8(temp);
            return std::nullopt;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        why = "cannot rename into " + utf8(file);
        return std::nullopt;
    }
    Artifact artifact;
    artifact.kind = std::string(kind);
    artifact.path = utf8(file);
    artifact.bytes = png.size();
    artifact.sha256 = Crypto::Sha256Hex(std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
    artifact.width = width;
    artifact.height = height;
    artifact.format = "RGBA8_UNORM";   // the PNG is tight RGBA, whatever the target's channel order
    return artifact;
}

} // namespace Arcane::Remote::Detail

#endif
