// ArcaneCore.cpp
// Ensures the static library produces a .lib file.
// All Core code is in headers; this file provides a compiled translation unit.
#include <Arcane/Version.hpp>
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Net/Protocol.hpp>
#include <Arcane/Net/RateLimiter.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Types/Types.hpp>
#include <Arcane/Util/Logger.hpp>
#include <Arcane/Util/LruCache.hpp>
