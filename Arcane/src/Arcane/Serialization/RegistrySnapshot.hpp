#pragma once

// Registry snapshot framing + error propagation.
//
// SnapshotRegistry used to return an empty vector on a Registry::Save() failure,
// which masked real data loss as a generic downstream "reload lost state". The
// snapshot's raw Save() Result is finished here so a Save failure surfaces as a
// real Result error the caller can act on, not an empty-but-"ok" snapshot.
//
// This seam is pure + header-only on purpose: Registry::Save() to a memory
// buffer is effectively infallible, so a forced-failure test cannot go through a
// live registry. Driving FinishSnapshot with a synthetic Err is how the
// propagation contract is exercised deterministically.
//
// (E02-1 layers a serializable-resource section onto this frame; see
// ResourceSerialization.hpp. E02-4 keeps it to the error-propagation identity.)

#include <Astra/Core/Result.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace Arcane::Serialization
{
    using SnapshotResult = Astra::Result<std::vector<std::byte>, Astra::SerializationError>;

    // Propagates a Registry::Save() failure as a Result error; on success yields
    // the snapshot bytes unchanged.
    inline SnapshotResult FinishSnapshot(SnapshotResult&& saveResult)
    {
        if (saveResult.IsErr())
            return SnapshotResult::Err(*saveResult.GetError());
        return SnapshotResult::Ok(std::move(*saveResult.GetValue()));
    }
}
