// Crypto behavioral coverage (E03-2), grown from the original 12-line
// smoke (this file was CryptoSmokeTest.cpp). Covers HmacSha256Hex
// against the RFC 4231 known-answer vectors, HexEquals, the lazy-rehash
// helpers (NeedsRehash / IterationsOfStoredHash), stored-hash format +
// malformed-hash rejection, tampered/wrong-length digest rejection
// (which drives the private ConstantTimeCompare through both of its
// reject branches -- the function itself is private, so it is covered
// via VerifyPassword and via the public HexEquals contract), and
// GenerateSecureToken / GenerateRandomBytes basics.
//
// Runtime budget: a single PBKDF2 derivation at DefaultIterations()
// (200k) costs ~17s in Debug, so only two cases pay it -- the
// production-config round-trip (kept from the smoke) and ONE malformed
// VerifyPassword case (whose M-V3-3 dummy burn is intentionally a full
// 200k derivation). Every other verify-path case uses 1000-iteration
// hashes, and the malformed-format matrix goes through
// IterationsOfStoredHash, which parses without deriving.

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Crypto/Crypto.hpp>

using Arcane::Crypto;

namespace
{
    // n copies of the byte b, as the std::string Crypto's HMAC API takes.
    std::string RepeatByte(uint8_t b, size_t n)
    {
        return std::string(n, static_cast<char>(b));
    }

    bool AllLowerHex(const std::string& s)
    {
        for (char c : s)
        {
            const bool digit = (c >= '0' && c <= '9');
            const bool lower = (c >= 'a' && c <= 'f');
            if (!digit && !lower) return false;
        }
        return true;
    }
}

// ---- password hashing -----------------------------------------------------

TEST_CASE("crypto: password hash round-trips", "[crypto]")
{
    // Kept from the original smoke: proves the production configuration
    // (DefaultIterations()) end to end. ~3 x 200k derivations.
    const std::string hash = Crypto::HashPassword("correct horse battery staple");
    REQUIRE(Crypto::VerifyPassword("correct horse battery staple", hash));
    REQUIRE_FALSE(Crypto::VerifyPassword("wrong password", hash));
}

TEST_CASE("crypto: stored hash is iterations:salt_hex:hash_hex with a fresh salt", "[crypto]")
{
    const std::string a = Crypto::HashPassword("pw", 1000);
    const std::string b = Crypto::HashPassword("pw", 1000);

    // Same password, fresh random salt -> different stored strings.
    REQUIRE(a != b);

    const size_t c1 = a.find(':');
    const size_t c2 = a.find(':', c1 + 1);
    REQUIRE(c1 != std::string::npos);
    REQUIRE(c2 != std::string::npos);

    REQUIRE(a.substr(0, c1) == "1000");
    const std::string saltHex = a.substr(c1 + 1, c2 - c1 - 1);
    const std::string hashHex = a.substr(c2 + 1);
    REQUIRE(saltHex.size() == Crypto::SALT_LENGTH * 2);   // 16 bytes -> 32 hex
    REQUIRE(hashHex.size() == Crypto::HASH_LENGTH * 2);   // 32 bytes -> 64 hex
    REQUIRE(AllLowerHex(saltHex));
    REQUIRE(AllLowerHex(hashHex));

    // Both independently-salted hashes verify; a wrong password does not.
    REQUIRE(Crypto::VerifyPassword("pw", a));
    REQUIRE(Crypto::VerifyPassword("pw", b));
    REQUIRE_FALSE(Crypto::VerifyPassword("pW", a));
}

// ---- HMAC-SHA256 known answers (RFC 4231) ---------------------------------

TEST_CASE("crypto: HmacSha256Hex matches the RFC 4231 known-answer vectors", "[crypto]")
{
    // Test Case 1: 20 x 0x0b key.
    REQUIRE(Crypto::HmacSha256Hex(RepeatByte(0x0b, 20), "Hi There")
            == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // Test Case 2: short ASCII key ("Jefe").
    REQUIRE(Crypto::HmacSha256Hex("Jefe", "what do ya want for nothing?")
            == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // Test Case 3: 20 x 0xaa key, 50 x 0xdd data (binary-safe strings).
    REQUIRE(Crypto::HmacSha256Hex(RepeatByte(0xaa, 20), RepeatByte(0xdd, 50))
            == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // Test Case 4: 25-byte incrementing key 0x01..0x19, 50 x 0xcd data.
    std::string tc4Key;
    for (uint8_t b = 0x01; b <= 0x19; ++b) tc4Key += static_cast<char>(b);
    REQUIRE(Crypto::HmacSha256Hex(tc4Key, RepeatByte(0xcd, 50))
            == "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

    // Test Case 5: RFC gives only the 128-bit truncated output; compare
    // the leading 32 hex chars of the full digest.
    REQUIRE(Crypto::HmacSha256Hex(RepeatByte(0x0c, 20), "Test With Truncation").substr(0, 32)
            == "a3b6167473100ee06e0c796c2955552b");

    // Test Case 6: 131-byte key -- exercises the hash-key-first path
    // (key larger than the 64-byte SHA-256 block).
    REQUIRE(Crypto::HmacSha256Hex(RepeatByte(0xaa, 131),
                                  "Test Using Larger Than Block-Size Key - Hash Key First")
            == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // Test Case 7: large key AND multi-block data.
    REQUIRE(Crypto::HmacSha256Hex(RepeatByte(0xaa, 131),
                                  "This is a test using a larger than block-size key and a "
                                  "larger than block-size data. The key needs to be hashed "
                                  "before being used by the HMAC algorithm.")
            == "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

TEST_CASE("crypto: HexEquals compares equal-length strings byte-wise", "[crypto]")
{
    REQUIRE(Crypto::HexEquals("deadbeef", "deadbeef"));
    REQUIRE(Crypto::HexEquals("", ""));

    // Single-nibble difference anywhere -> unequal.
    REQUIRE_FALSE(Crypto::HexEquals("deadbeef", "deadbeee"));
    REQUIRE_FALSE(Crypto::HexEquals("0eadbeef", "deadbeef"));

    // Byte-wise, not case-folded: uppercase hex is NOT equal. Callers
    // (on-wire signed-token MACs) always compare lowercase against lowercase.
    REQUIRE_FALSE(Crypto::HexEquals("DEADBEEF", "deadbeef"));

    // Length mismatch returns false (fast; lengths are public anyway).
    REQUIRE_FALSE(Crypto::HexEquals("deadbeef", "deadbeef00"));
    REQUIRE_FALSE(Crypto::HexEquals("deadbeef", ""));
}

// ---- lazy-rehash helpers ---------------------------------------------------

TEST_CASE("crypto: NeedsRehash / IterationsOfStoredHash drive the lazy-rehash decision", "[crypto]")
{
    REQUIRE(Crypto::DefaultIterations() == 200000);

    // A real below-default hash wants a rehash.
    const std::string low = Crypto::HashPassword("pw", 1000);
    REQUIRE(Crypto::IterationsOfStoredHash(low) == 1000);
    REQUIRE(Crypto::NeedsRehash(low));

    // At/above default: no rehash. Crafted strings -- these helpers only
    // parse, they never derive, so this costs nothing.
    REQUIRE(Crypto::IterationsOfStoredHash("200000:aabb:ccdd") == 200000);
    REQUIRE_FALSE(Crypto::NeedsRehash("200000:aabb:ccdd"));
    REQUIRE(Crypto::IterationsOfStoredHash("300000:aabb:ccdd") == 300000);
    REQUIRE_FALSE(Crypto::NeedsRehash("300000:aabb:ccdd"));
}

TEST_CASE("crypto: IterationsOfStoredHash rejects malformed stored hashes", "[crypto]")
{
    // Parse-only rejection matrix (returns 0 = "malformed, rehash it").
    // VerifyPassword shares the same parser (ParseStoredHash), so each
    // of these is also a VerifyPassword-fails-closed shape; only one of
    // them pays the 200k dummy burn below.
    const char* malformed[] = {
        "",                                  // empty
        "nocolons",                          // no separators
        "1000:onlyonecolon",                 // one separator
        "abc:aabb:ccdd",                     // non-numeric iterations
        "0:aabb:ccdd",                       // zero iterations
        "-5:aabb:ccdd",                      // negative iterations
        "99999999999999999999:aabb:ccdd",    // stoi overflow
        "1000:aab:ccdd",                     // odd-length salt hex (M-V4-6)
        "1000:aabb:ccd",                     // odd-length hash hex (M-V4-6)
        "1000::ccdd",                        // empty salt
        "1000:aabb:",                        // empty hash
    };

    for (const char* stored : malformed)
    {
        INFO("stored hash: \"" << stored << "\"");
        REQUIRE(Crypto::IterationsOfStoredHash(stored) == 0);
        REQUIRE(Crypto::NeedsRehash(stored));
    }
}

TEST_CASE("crypto: VerifyPassword fails closed on a malformed stored hash", "[crypto]")
{
    // One representative case (~17s in Debug): the M-V3-3 path burns a
    // full DefaultIterations() dummy derivation on parse failure so a
    // malformed row is as slow as a real verify (no username-enumeration
    // timing oracle). The full malformed-format matrix is asserted
    // cheaply through IterationsOfStoredHash above.
    REQUIRE_FALSE(Crypto::VerifyPassword("pw", "not-a-valid-stored-hash"));
}

TEST_CASE("crypto: VerifyPassword rejects tampered or wrong-length digests", "[crypto]")
{
    const std::string good = Crypto::HashPassword("pw", 1000);

    // Flip the last hex nibble of the stored digest: same length,
    // one byte differs -> ConstantTimeCompare's unequal-bytes branch.
    std::string tampered = good;
    tampered.back() = (tampered.back() == '0') ? '1' : '0';
    REQUIRE_FALSE(Crypto::VerifyPassword("pw", tampered));

    // Truncate the digest field to 16 bytes of valid even-length hex:
    // parses fine, derives, then hits the size-mismatch branch.
    const size_t lastColon = good.rfind(':');
    const std::string shortDigest =
        good.substr(0, lastColon + 1) + good.substr(lastColon + 1, 32);
    REQUIRE_FALSE(Crypto::VerifyPassword("pw", shortDigest));

    // Positive control: the untouched hash still verifies.
    REQUIRE(Crypto::VerifyPassword("pw", good));
}

// ---- token / RNG surface ---------------------------------------------------

TEST_CASE("crypto: GenerateSecureToken emits lowercase hex of the requested size", "[crypto]")
{
    REQUIRE(Crypto::GenerateSecureToken(0).empty());
    REQUIRE(Crypto::GenerateSecureToken(1).size() == 2);
    REQUIRE(Crypto::GenerateSecureToken(16).size() == 32);

    // 32 bytes -> the 64-char session-token shape.
    const std::string a = Crypto::GenerateSecureToken(32);
    const std::string b = Crypto::GenerateSecureToken(32);
    REQUIRE(a.size() == 64);
    REQUIRE(AllLowerHex(a));
    REQUIRE(AllLowerHex(b));

    // Two 256-bit draws colliding means the RNG is broken.
    REQUIRE(a != b);
}

TEST_CASE("crypto: GenerateRandomBytes returns the requested count with variation", "[crypto]")
{
    REQUIRE(Crypto::GenerateRandomBytes(0).empty());
    REQUIRE(Crypto::GenerateRandomBytes(1).size() == 1);
    REQUIRE(Crypto::GenerateRandomBytes(33).size() == 33);

    const auto a = Crypto::GenerateRandomBytes(32);
    const auto b = Crypto::GenerateRandomBytes(32);
    REQUIRE(a.size() == 32);
    REQUIRE(a != b);
}

// ---- E03-3: RNG fail-closed hardening + entropy self-test ------------------
//
// The hardened /dev/urandom branch itself cannot execute on Windows, so
// its decision logic is factored into the platform-neutral
// ReadExactFromStream and the pure EntropySamplesDegenerate predicate,
// both fully unit-tested here. Runtime validation of the POSIX branch
// on real ARM/Linux hardware is deferred to the Linux-port milestone.
//
// The Windows chunking + fail-closed policy lives in the
// platform-neutral FillRandomBytesChunked helper (an injected fill, so
// a generator that refuses a chunk is unit-tested here without waiting
// for BCryptGenRandom to actually fail). A zero-length request must
// not call the fill. A false fill throws and must not hand back a
// buffer. Chunks tile the request exactly once per byte.

TEST_CASE("crypto: ReadExactFromStream fails closed on short or bad sources", "[crypto]")
{
    // Exact-length source: fulfilled, buffer filled with source bytes.
    {
        std::istringstream src(std::string(8, '\x42'));
        std::vector<uint8_t> buf(8, 0);
        REQUIRE(Crypto::ReadExactFromStream(src, buf.data(), buf.size()));
        REQUIRE(buf == std::vector<uint8_t>(8, 0x42));
    }

    // Longer source: takes exactly what was asked for.
    {
        std::istringstream src("abcdefgh");
        std::vector<uint8_t> buf(4, 0);
        REQUIRE(Crypto::ReadExactFromStream(src, buf.data(), buf.size()));
        REQUIRE(buf == std::vector<uint8_t>({'a', 'b', 'c', 'd'}));
    }

    // SHORT source: the exact L-V5-2 bug shape -- the old code would
    // silently leave the buffer tail zeroed. Must fail closed.
    {
        std::istringstream src("abc");
        std::vector<uint8_t> buf(16, 0);
        REQUIRE_FALSE(Crypto::ReadExactFromStream(src, buf.data(), buf.size()));
    }

    // Unopenable stream (failbit set), like a missing /dev/urandom.
    {
        std::ifstream missing("e033_no_such_file_anywhere__");
        std::vector<uint8_t> buf(8, 0);
        REQUIRE_FALSE(Crypto::ReadExactFromStream(missing, buf.data(), buf.size()));
    }

    // A zero-byte request against a good stream is trivially fulfilled.
    {
        std::istringstream src("x");
        std::vector<uint8_t> buf(1, 0);
        REQUIRE(Crypto::ReadExactFromStream(src, buf.data(), 0));
    }
}

TEST_CASE("crypto: FillRandomBytesChunked fails closed and covers every byte once", "[crypto]")
{
    const char* const kFailure = "Crypto: test fill failed";

    // count 0 returns empty and never calls the fill, even when the
    // injected chunk size would be unusable for a real draw.
    {
        int calls = 0;
        const auto out = Crypto::FillRandomBytesChunked(
            0, 0,
            [&](uint8_t*, size_t) {
                ++calls;
                return true;
            },
            kFailure);
        REQUIRE(out.empty());
        REQUIRE(calls == 0);
    }

    // A fill that fails on the first chunk throws. The caller's buffer
    // is left untouched -- the helper must not return a buffer.
    {
        int calls = 0;
        std::vector<uint8_t> returned(1, 0xFF);
        bool threw = false;
        try
        {
            returned = Crypto::FillRandomBytesChunked(
                8, 3,
                [&](uint8_t*, size_t) {
                    ++calls;
                    return false;
                },
                kFailure);
        }
        catch (const std::runtime_error& ex)
        {
            threw = true;
            REQUIRE(std::string(ex.what()) == kFailure);
        }
        REQUIRE(threw);
        REQUIRE(calls == 1);
        REQUIRE(returned == std::vector<uint8_t>(1, 0xFF));
    }

    // A fill that succeeds once and then fails must still throw, and
    // must not return the bytes already written.
    {
        int calls = 0;
        std::vector<uint8_t> returned(1, 0xFF);
        bool threw = false;
        try
        {
            returned = Crypto::FillRandomBytesChunked(
                8, 3,
                [&](uint8_t* dst, size_t n) {
                    ++calls;
                    REQUIRE(n > 0);
                    for (size_t i = 0; i < n; ++i)
                        dst[i] = 0x11;
                    return calls < 2;
                },
                kFailure);
        }
        catch (const std::runtime_error& ex)
        {
            threw = true;
            REQUIRE(std::string(ex.what()) == kFailure);
        }
        REQUIRE(threw);
        REQUIRE(calls == 2);
        REQUIRE(returned == std::vector<uint8_t>(1, 0xFF));
    }

    // Success fills every byte. One chunk larger than the request is a
    // single fill of the whole buffer.
    {
        int calls = 0;
        const auto out = Crypto::FillRandomBytesChunked(
            7, 64,
            [&](uint8_t* dst, size_t n) {
                ++calls;
                REQUIRE(n == 7);
                for (size_t i = 0; i < n; ++i)
                    dst[i] = 0x5A;
                return true;
            },
            kFailure);
        REQUIRE(calls == 1);
        REQUIRE(out == std::vector<uint8_t>(7, 0x5A));
    }

    // Chunking: a count larger than the injected chunk size is covered
    // exactly once per byte (10 bytes, 3-byte chunks -> 3,3,3,1).
    {
        struct ChunkNote
        {
            uint8_t stamp;
            size_t n;
        };
        std::vector<ChunkNote> notes;
        uint8_t stamp = 1;
        constexpr size_t kCount = 10;
        constexpr size_t kChunk = 3;
        const auto out = Crypto::FillRandomBytesChunked(
            kCount, kChunk,
            [&](uint8_t* dst, size_t n) {
                REQUIRE(n > 0);
                REQUIRE(n <= kChunk);
                for (size_t i = 0; i < n; ++i)
                    dst[i] = stamp;
                notes.push_back(ChunkNote{ stamp, n });
                ++stamp;
                return true;
            },
            kFailure);

        REQUIRE(out.size() == kCount);
        REQUIRE(notes.size() == 4);
        REQUIRE(notes[0].n == 3);
        REQUIRE(notes[1].n == 3);
        REQUIRE(notes[2].n == 3);
        REQUIRE(notes[3].n == 1);

        size_t offset = 0;
        for (const ChunkNote& note : notes)
        {
            REQUIRE(offset + note.n <= out.size());
            for (size_t i = 0; i < note.n; ++i)
                REQUIRE(out[offset + i] == note.stamp);
            offset += note.n;
        }
        REQUIRE(offset == out.size());
    }
}

TEST_CASE("crypto: EntropySamplesDegenerate rejects stuck or echoing RNG output", "[crypto]")
{
    const std::vector<uint8_t> zeros(32, 0x00);
    const std::vector<uint8_t> constant(32, 0xA5);
    std::vector<uint8_t> varied(32);
    for (size_t i = 0; i < varied.size(); ++i)
        varied[i] = static_cast<uint8_t>(i * 37 + 11);
    std::vector<uint8_t> varied2 = varied;
    varied2[0] ^= 0xFF;

    // All-zero draws: the classic unchecked-short-read residue.
    REQUIRE(Crypto::EntropySamplesDegenerate(zeros, zeros));
    REQUIRE(Crypto::EntropySamplesDegenerate(zeros, varied));
    REQUIRE(Crypto::EntropySamplesDegenerate(varied, zeros));

    // A draw stuck at any single byte value, not just zero.
    REQUIRE(Crypto::EntropySamplesDegenerate(constant, varied));
    REQUIRE(Crypto::EntropySamplesDegenerate(varied, constant));

    // Two "independent" draws that collide: echoing/seeded-deterministic
    // generator.
    REQUIRE(Crypto::EntropySamplesDegenerate(varied, varied));

    // Empty or size-mismatched draws did not fulfil the request.
    REQUIRE(Crypto::EntropySamplesDegenerate({}, {}));
    const std::vector<uint8_t> half(varied.begin(), varied.begin() + 16);
    REQUIRE(Crypto::EntropySamplesDegenerate(half, varied));

    // Two healthy-looking distinct draws pass.
    REQUIRE_FALSE(Crypto::EntropySamplesDegenerate(varied, varied2));
}

TEST_CASE("crypto: the live RNG passes the startup entropy self-test", "[crypto]")
{
    // On this Windows/BCrypt machine the self-test must pass, the
    // enforcement latch must not throw, and the guarded production path
    // must keep serving bytes afterwards.
    REQUIRE(Crypto::EntropySelfTest());
    REQUIRE_NOTHROW(Crypto::EnsureEntropySelfTest());
    REQUIRE(Crypto::GenerateRandomBytes(16).size() == 16);
}
