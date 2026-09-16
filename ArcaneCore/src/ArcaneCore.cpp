// ArcaneCore.cpp
// The headless engine DLL's anchor translation unit (Core-DLL split, spec
// docs/specs/2026-09-15-core-dll-split-design.md s1). ArcaneCore is a SHARED
// library now, not a static one: exactly one copy per process by construction,
// and it is what ArcaneClient.dll, the host exes and every game module import.
// Most of Core still lives in headers, and the pieces that do have a .cpp carry
// their own (Base/ProcessContext.cpp, Plugin/PluginHost.cpp, ...). What this
// file does is force the header-only exported surfaces below to be INSTANTIATED
// and emitted into the DLL, so the matching import lib carries them.
#include <Arcane/Version.hpp>
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Net/Protocol.hpp>
#include <Arcane/Net/RateLimiter.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Util/Logger.hpp>
#include <Arcane/Util/LruCache.hpp>
