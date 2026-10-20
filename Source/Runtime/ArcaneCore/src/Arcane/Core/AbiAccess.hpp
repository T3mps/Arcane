#pragma once

// Forward declaration of the ABI manifest's accessor. A class whose private
// members game code reaches through inline accessors befriends it:
//     friend struct ::Arcane::Detail::AbiManifest::Access;
// so Core/AbiManifest.hpp can hash those members' offsets and sizes. This is
// the only thing the friend grants; Access has no behaviour (it holds
// constexpr hashing functions evaluated at compile time).

namespace Arcane::Detail::AbiManifest { struct Access; }
