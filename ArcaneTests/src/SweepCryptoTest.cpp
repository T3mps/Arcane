// Settings arc S6-13: crypto.pbkdf2Iterations -- the PBKDF2 round count for
// new password hashes is a Server cvar. Raise-only: the range floor is the
// pre-sweep DEFAULT_ITERATIONS (200000), so no layer can weaken it.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Crypto/CryptoSettings.hpp>

using namespace Arcane;

TEST_CASE("sweep: pbkdf2 iterations default 200000 and never go below it", "[sweep][crypto]")
{
    CHECK(CryptoSettings{}.pbkdf2Iterations == 200000);
    Test::RequireDefault("crypto.pbkdf2Iterations", CVarValue::Int32(200000));
    CHECK(Crypto::DefaultIterations() == 200000);
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("crypto.pbkdf2Iterations"), CVarValue::Int32(1000), SetBy::Code);   // clamped up to the floor
    reg.PublishImmediate();
    CHECK(Crypto::DefaultIterations() == 200000);
    reg.Set(reg.Find("crypto.pbkdf2Iterations"), CVarValue::Int32(600000), SetBy::Code);
    reg.PublishImmediate();
    CHECK(Crypto::DefaultIterations() == 600000);
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}
