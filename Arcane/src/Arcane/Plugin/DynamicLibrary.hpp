#pragma once

// The ONLY place the engine calls the OS dynamic-loader. Windows now; the Linux
// dlopen port (later milestone) is a single-file swap of the .cpp.

#include <filesystem>

namespace Arcane::Detail
{
    using LibHandle = void*;

    LibHandle DLOpen(const std::filesystem::path& path);  // null on failure
    void*     DLSym(LibHandle handle, const char* name);  // null if the symbol is missing
    void      DLClose(LibHandle handle);
}
