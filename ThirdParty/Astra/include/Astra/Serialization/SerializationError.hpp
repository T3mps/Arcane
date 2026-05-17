#pragma once

#include <cstdint>

namespace Astra
{
    /**
    * Error codes for serialization operations
    */
    enum class SerializationError
    {
        None,
        InvalidMagic,
        UnsupportedVersion,
        CorruptedData,
        UnknownComponent,
        SizeMismatch,
        EndiannessMismatch,
        ChecksumMismatch,
        IOError,
        OutOfMemory
    };
}