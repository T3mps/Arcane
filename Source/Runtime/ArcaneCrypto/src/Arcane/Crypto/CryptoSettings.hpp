#pragma once

// crypto.* (settings arc S6-13; inventory Part 1 "Crypto"): the PBKDF2 round
// count Crypto::HashPassword uses for new hashes, and the bar NeedsRehash
// measures a stored hash against.
// - Server audience, Project scope, Live: a raise takes effect on the next
//   hash; stored hashes below it are re-derived lazily on their next
//   successful VerifyPassword (NeedsRehash), so no migration window is needed.
// - Raise-only: the range floor is the pre-sweep DEFAULT_ITERATIONS (200000),
//   so a lower value from any layer clamps up and can never weaken a hash.
// - Audit C5 (2026-06-02) raised the count from 10,000 to 200,000, the OWASP
//   2023 lower bound for PBKDF2-SHA256 (600,000 is the recommendation).
//   DEFER (audit L-V5-1 security, 2026-06-03): raise the Project value to
//   600k before Release launch (or before 2027, whichever is sooner).
//   Audit ref: docs/superpowers/audits/2026-06-03-v5-followup-security.md
// - The registration lives in CryptoSettings.cpp (ArcaneCore.dll), which every
//   consumer of the header-only Crypto.hpp links.

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    struct CryptoSettings
    {
        std::int32_t pbkdf2Iterations = 200000;   // PBKDF2-HMAC-SHA256 rounds for new hashes
    };

    ARC_REFLECT_TYPE(CryptoSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "crypto", SettingScope::Project, ApplyMode::Live, Audience::Server)
        ARC_REFLECT_FIELD(CryptoSettings, pbkdf2Iterations)
            ARC_REFLECT_ATTR(Range, 200000.0, 5000000.0)
            ARC_REFLECT_ATTR(Tooltip, "PBKDF2-SHA256 rounds for new hashes. Raise-only: values below 200000 clamp up; "
                                      "stored hashes rehash lazily on next login.")
    ARC_END_REFLECT_TYPE()
}
