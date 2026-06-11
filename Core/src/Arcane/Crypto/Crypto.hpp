#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "picosha2.hpp"
#include <Arcane/Util/Logger.hpp>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#else
    #include <fstream>
#endif

namespace Arcane
{
    class Crypto
    {
    public:
        // Configuration
        //
        // PBKDF2-HMAC-SHA256 iteration count. 200,000 puts us above the OWASP
        // 2023 floor for PBKDF2-SHA256 (600k is the current rec; 200k is the
        // lower bound still considered acceptable). Audit C5 (2026-06-02)
        // raised this from 10,000 â€” anything hashed at the old setting is
        // lazy-rehashed on its next successful VerifyPassword via the helpers
        // below (NeedsRehash, IterationsOfStoredHash) â€” the caller writes the
        // new hash back to storage and the upgrade happens organically per
        // user.
        // DEFER (audit L-V5-1 security, 2026-06-03, multi-cycle carry-forward):
        // OWASP 2023 PBKDF2-SHA256 recommendation is 600k. 200k is the
        // documented lower bound — pre-launch fine; bump before Release
        // launch (or before 2027, whichever is sooner). The lazy-rehash
        // pathway above means a bump rolls out organically per user on next
        // successful login; no migration window required.
        // Audit ref: docs/superpowers/audits/2026-06-03-v5-followup-security.md
        static constexpr int DEFAULT_ITERATIONS = 200000;
        static constexpr int SALT_LENGTH = 16;  // 128 bits
        static constexpr int HASH_LENGTH = 32;  // 256 bits (SHA-256 output)

        // ============================================================================
        // Public API
        // ============================================================================

        // Hash a password with a random salt
        // Returns format: "iterations:salt_hex:hash_hex"
        static std::string HashPassword(const std::string& password, int iterations = DEFAULT_ITERATIONS)
        {
            // Generate random salt
            std::vector<uint8_t> salt = GenerateRandomBytes(SALT_LENGTH);

            // Derive key using PBKDF2
            std::vector<uint8_t> hash = PBKDF2_HMAC_SHA256(password, salt, iterations, HASH_LENGTH);

            // Format: iterations:salt:hash
            std::ostringstream ss;
            ss << iterations << ":" << ToHex(salt) << ":" << ToHex(hash);
            return ss.str();
        }

        // Generate a cryptographically secure random hex token
        // byteCount determines entropy (e.g., 32 bytes = 64 hex chars = 256 bits)
        static std::string GenerateSecureToken(size_t byteCount)
        {
            static const char hexChars[] = "0123456789abcdef";

            std::vector<uint8_t> bytes = GenerateRandomBytes(byteCount);
            std::string token;
            token.reserve(byteCount * 2);

            for (uint8_t byte : bytes)
            {
                token += hexChars[(byte >> 4) & 0x0F];
                token += hexChars[byte & 0x0F];
            }

            return token;
        }

        // Parse the iteration count embedded in a stored hash. Returns 0 on
        // any parse failure (treat as "needs rehash" since the row is
        // malformed). Used by the lazy-rehash path to decide whether the
        // existing hash is below DEFAULT_ITERATIONS.
        static int IterationsOfStoredHash(const std::string& storedHash)
        {
            int iterations = 0;
            std::vector<uint8_t> salt, hash;
            if (!ParseStoredHash(storedHash, iterations, salt, hash))
                return 0;
            return iterations;
        }

        // Returns true when the stored hash should be re-derived because its
        // iteration count is below the current DEFAULT_ITERATIONS. Call this
        // after a successful VerifyPassword and, if true, rehash + persist.
        static bool NeedsRehash(const std::string& storedHash)
        {
            return IterationsOfStoredHash(storedHash) < DEFAULT_ITERATIONS;
        }

        // Verify a password against a stored hash
        static bool VerifyPassword(const std::string& password, const std::string& storedHash)
        {
            // Parse stored hash
            int iterations;
            std::vector<uint8_t> salt;
            std::vector<uint8_t> expectedHash;

            if (!ParseStoredHash(storedHash, iterations, salt, expectedHash))
            {
                LOG_AUTH_WARN("Password verification failed: malformed stored hash");
                // Audit M-V3-3 (2026-06-03): burn one PBKDF2 derivation
                // even on parse failure so a malformed stored hash (e.g.
                // a corrupted DB row, an init-order regression of the
                // H-V2-5 dummy hash that returned an empty string, or
                // any future code path that hands VerifyPassword a hash
                // it can't decode) doesn't short-circuit and reopen the
                // username-enumeration timing oracle. The derivation is
                // against a fixed dummy salt and DEFAULT_ITERATIONS so
                // the wall time matches the legitimate parse-then-derive
                // path. Result is intentionally discarded.
                std::vector<uint8_t> dummySalt(SALT_LENGTH, 0xA5);
                (void)PBKDF2_HMAC_SHA256(password, dummySalt, DEFAULT_ITERATIONS, HASH_LENGTH);
                return false;
            }

            // Derive key with same parameters
            std::vector<uint8_t> computedHash = PBKDF2_HMAC_SHA256(password, salt, iterations, HASH_LENGTH);

            // Constant-time comparison to prevent timing attacks
            bool result = ConstantTimeCompare(computedHash, expectedHash);
            if (!result)
            {
                LOG_AUTH_DEBUG("Password verification failed: hash mismatch");
            }
            return result;
        }

        // DEFER (audit L-V5-2 security, 2026-06-03, V3-M8 multi-cycle
        // carry-forward): BCryptGenRandom failure falls back to
        // std::random_device with WARN-once-per-call. On Windows + MSVC the
        // BCrypt path effectively never fails; on a future Linux build the
        // fallback becomes load-bearing for token + salt + dummy-hash
        // generation. The originally-recommended fix is a startup self-test
        // that proves BCryptGenRandom works once and refuses to start
        // otherwise. Pre-launch on Windows the risk is zero; revisit before
        // first non-Windows build target.
        // Audit ref: docs/superpowers/audits/2026-06-03-v5-followup-security.md
        static std::vector<uint8_t> GenerateRandomBytes(size_t count)
        {
            std::vector<uint8_t> bytes(count);

#ifdef _WIN32
            // Use Windows BCrypt API (CSPRNG)
            NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

            if (!BCRYPT_SUCCESS(status))
            {
                LOG_AUTH_WARN("BCryptGenRandom failed, falling back to std::random_device");
                // Fallback to std::random_device if BCrypt fails
                std::random_device rd;
                for (size_t i = 0; i < count; i++)
                {
                    bytes[i] = static_cast<uint8_t>(rd() & 0xFF);
                }
            }
#else
            // Use /dev/urandom on Unix-like systems
            std::ifstream urandom("/dev/urandom", std::ios::binary);
            if (urandom)
            {
                urandom.read(reinterpret_cast<char*>(bytes.data()), count);
            }
            else
            {
                // Fallback to std::random_device
                std::random_device rd;
                for (size_t i = 0; i < count; i++)
                {
                    bytes[i] = static_cast<uint8_t>(rd() & 0xFF);
                }
            }
#endif

            return bytes;
        }

        // ============================================================================
        // HMAC-SHA256 â€” public convenience for non-password use cases
        // ============================================================================
        //
        // Audit M4 (2026-06-02): exposes the HMAC primitive plus a hex wrapper so
        // callers like QuestHandlers::VerifyQuestToken can replace raw
        // SHA-256(secret || message) (length-extension vulnerable) with a proper
        // keyed MAC without reaching into the PBKDF2-private internals.
        //
        // The hex variant matches the existing on-wire format used by the quest
        // challenge token (`<tsHex>.<hash>`); pair it with HexEquals below for
        // constant-time comparison.

        static std::string HmacSha256Hex(const std::string& key, const std::string& message)
        {
            return ToHex(HMAC_SHA256(key, message));
        }

        // Constant-time comparison of two equal-length hex strings. Distinct
        // from ConstantTimeCompare so callers don't need to convert their hex
        // payloads to bytes first. Different-length inputs return false fast â€”
        // that doesn't leak anything useful since the lengths are known to the
        // attacker from the wire format.
        static bool HexEquals(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size()) return false;
            uint8_t result = 0;
            for (size_t i = 0; i < a.size(); ++i)
                result |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
            return result == 0;
        }

    private:
        // ============================================================================
        // PBKDF2 Implementation
        // ============================================================================

        static std::vector<uint8_t> PBKDF2_HMAC_SHA256(const std::string& password, const std::vector<uint8_t>& salt, int iterations, int keyLength)
        {
            std::vector<uint8_t> derivedKey;
            derivedKey.reserve(keyLength);

            int blockCount = (keyLength + HASH_LENGTH - 1) / HASH_LENGTH;

            for (int block = 1; block <= blockCount; block++)
            {
                std::vector<uint8_t> blockResult = PBKDF2_F(password, salt, iterations, block);
                derivedKey.insert(derivedKey.end(), blockResult.begin(), blockResult.end());
            }

            derivedKey.resize(keyLength);
            return derivedKey;
        }

        // F function: F(Password, Salt, c, i) = U1 ^ U2 ^ ... ^ Uc
        static std::vector<uint8_t> PBKDF2_F(const std::string& password, const std::vector<uint8_t>& salt, int iterations, int blockIndex)
        {
            // U1 = PRF(Password, Salt || INT_32_BE(i))
            std::vector<uint8_t> saltWithIndex = salt;
            saltWithIndex.push_back(static_cast<uint8_t>((blockIndex >> 24) & 0xFF));
            saltWithIndex.push_back(static_cast<uint8_t>((blockIndex >> 16) & 0xFF));
            saltWithIndex.push_back(static_cast<uint8_t>((blockIndex >> 8) & 0xFF));
            saltWithIndex.push_back(static_cast<uint8_t>(blockIndex & 0xFF));

            std::vector<uint8_t> u = HMAC_SHA256(password, saltWithIndex);
            std::vector<uint8_t> result = u;

            // U2 ... Uc
            for (int i = 2; i <= iterations; i++)
            {
                u = HMAC_SHA256(password, u);
                // XOR into result
                for (size_t j = 0; j < result.size(); j++)
                {
                    result[j] ^= u[j];
                }
            }

            return result;
        }

        // ============================================================================
        // HMAC-SHA256 Implementation
        // ============================================================================

        static std::vector<uint8_t> HMAC_SHA256(const std::string& key, const std::vector<uint8_t>& message)
        {
            constexpr size_t BLOCK_SIZE = 64;  // SHA-256 block size

            // Prepare key
            std::vector<uint8_t> keyBytes(key.begin(), key.end());

            // If key > block size, hash it
            if (keyBytes.size() > BLOCK_SIZE)
            {
                keyBytes = SHA256_Raw(keyBytes);
            }

            // Pad key to block size
            keyBytes.resize(BLOCK_SIZE, 0x00);

            // Create inner and outer padded keys
            std::vector<uint8_t> innerPadded(BLOCK_SIZE);
            std::vector<uint8_t> outerPadded(BLOCK_SIZE);

            for (size_t i = 0; i < BLOCK_SIZE; i++)
            {
                innerPadded[i] = keyBytes[i] ^ 0x36;
                outerPadded[i] = keyBytes[i] ^ 0x5c;
            }

            // Inner hash: H(innerPadded || message)
            std::vector<uint8_t> innerData = innerPadded;
            innerData.insert(innerData.end(), message.begin(), message.end());
            std::vector<uint8_t> innerHash = SHA256_Raw(innerData);

            // Outer hash: H(outerPadded || innerHash)
            std::vector<uint8_t> outerData = outerPadded;
            outerData.insert(outerData.end(), innerHash.begin(), innerHash.end());

            return SHA256_Raw(outerData);
        }

        // Overload for string message
        static std::vector<uint8_t> HMAC_SHA256(const std::string& key, const std::string& message)
        {
            return HMAC_SHA256(key, std::vector<uint8_t>(message.begin(), message.end()));
        }

        // ============================================================================
        // SHA-256 Wrapper (using picosha2)
        // ============================================================================

        static std::vector<uint8_t> SHA256_Raw(const std::vector<uint8_t>& data)
        {
            std::vector<uint8_t> hash(picosha2::k_digest_size);
            picosha2::hash256(data.begin(), data.end(), hash.begin(), hash.end());
            return hash;
        }

        // ============================================================================
        // Utility Functions
        // ============================================================================

        static std::string ToHex(const std::vector<uint8_t>& bytes)
        {
            std::ostringstream ss;
            ss << std::hex << std::setfill('0');
            for (uint8_t byte : bytes)
            {
                ss << std::setw(2) << static_cast<int>(byte);
            }
            return ss.str();
        }

        static std::vector<uint8_t> FromHex(const std::string& hex)
        {
            std::vector<uint8_t> bytes;
            bytes.reserve(hex.length() / 2);

            for (size_t i = 0; i + 1 < hex.length(); i += 2)
            {
                uint8_t byte = static_cast<uint8_t>((HexCharToInt(hex[i]) << 4) | HexCharToInt(hex[i + 1]));
                bytes.push_back(byte);
            }

            return bytes;
        }

        static int HexCharToInt(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        }

        static bool ParseStoredHash(const std::string& stored, int& iterations, std::vector<uint8_t>& salt, std::vector<uint8_t>& hash)
        {
            // Format: "iterations:salt_hex:hash_hex"
            size_t firstColon = stored.find(':');
            if (firstColon == std::string::npos)
                return false;

            size_t secondColon = stored.find(':', firstColon + 1);
            if (secondColon == std::string::npos)
                return false;

            try
            {
                iterations = std::stoi(stored.substr(0, firstColon));
                const auto saltHex = stored.substr(firstColon + 1, secondColon - firstColon - 1);
                const auto hashHex = stored.substr(secondColon + 1);
                // Audit M-V4-6 security (2026-06-03): FromHex's loop guard
                // `i + 1 < hex.length()` silently truncates odd-length input
                // — a corrupted DB row would produce a short byte vector
                // that ConstantTimeCompare always rejects (different length)
                // after a wasted PBKDF2 derivation. Reject the format up
                // front so the caller surfaces the corruption rather than
                // burning CPU on a guaranteed mismatch.
                if ((saltHex.size() % 2) != 0 || (hashHex.size() % 2) != 0)
                    return false;
                salt = FromHex(saltHex);
                hash = FromHex(hashHex);
                return !salt.empty() && !hash.empty() && iterations > 0;
            }
            catch (...)
            {
                return false;
            }
        }

        // Constant-time comparison to prevent timing attacks
        static bool ConstantTimeCompare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
        {
            if (a.size() != b.size())
                return false;

            uint8_t result = 0;
            for (size_t i = 0; i < a.size(); i++)
            {
                result |= a[i] ^ b[i];
            }
            return result == 0;
        }
    };
} // namespace Arcane