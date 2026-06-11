// Crypto smoke: proves the moved Crypto compiles, links bcrypt via its
// pragma, and round-trips a password at the current iteration setting.

#include <catch2/catch_test_macros.hpp>
#include <Arcane/Crypto/Crypto.hpp>

TEST_CASE("crypto: password hash round-trips", "[crypto]")
{
    const std::string hash = Arcane::Crypto::HashPassword("correct horse battery staple");
    REQUIRE(Arcane::Crypto::VerifyPassword("correct horse battery staple", hash));
    REQUIRE_FALSE(Arcane::Crypto::VerifyPassword("wrong password", hash));
}
