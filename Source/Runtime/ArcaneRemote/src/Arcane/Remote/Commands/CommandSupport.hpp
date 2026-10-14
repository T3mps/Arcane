#pragma once
// ArcaneRemote-internal helpers shared by the world-reading commands (entity.*
// and ecs.*, Task 23; resource.*, Task 24). Not exported: nothing outside this
// module includes it. Moved out of EcsCommands.cpp so resource.get reuses the
// same world pick, argument checks and inline-cap artifact path.
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Core/Constant.hpp>

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Astra
{
class Registry;
}

namespace Arcane::Remote::Detail
{

ARC_CONSTANT("spec s4.8: ECS and resource dumps over 256 KiB go to an artifact, never inline in the envelope")
inline constexpr std::size_t kInlineCapBytes = 262144;

// The session's artifact directory (<remote dir>/out; SetEcsArtifactDir sets
// it). Empty: no session armed one.
std::filesystem::path ArtifactDir();
void SetArtifactDir(const std::filesystem::path& dir);
// Clears the directory only when it is still `dir`.
void ReleaseArtifactDir(const std::filesystem::path& dir);

// args must be an object (null reads as empty) whose keys are all in `known`:
// arg.invalid / arg.unknown otherwise.
std::optional<Envelope> CheckKeys(const Call& call, std::initializer_list<std::string_view> known,
                                  std::string_view usage);

// The registry --world names; null with the pick's refusal otherwise
// (arg.missing, arg.unknown, host.unavailable).
Astra::Registry* PickRegistry(const Call& call, Envelope& refusal);

Error MakeIssue(std::string code, std::string message, std::string remediation);

// An error envelope whose one error is `issue` (detail and candidates kept).
Envelope ErrorEnvelope(const Call& call, Error issue);

// Writes `json` to <dir>/<stem>-<unix ms>-<n>.json through <file>.tmp, a close
// whose failure is checked, and a rename; the artifact (`kind`, format json,
// bytes, sha256), or nullopt with *why when any step fails.
std::optional<Artifact> WriteArtifact(const std::filesystem::path& dir, std::string_view stem, std::string_view kind,
                                      const std::string& json, std::string& why);

} // namespace Arcane::Remote::Detail

#endif
