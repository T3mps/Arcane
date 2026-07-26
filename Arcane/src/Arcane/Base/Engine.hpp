#pragma once

#include <Arcane/Base/Api.hpp>

#include <string>

namespace Arcane
{
    // Version + build flavor of the loaded engine DLL. Doubles as the
    // simplest possible export for proving the DLL boundary works.
    ARCANE_API const char* BuildInfo();

    // Absolute path of the running executable, UTF-8, forward-slashed. Empty if
    // the OS could not report it.
    //
    // Exists because argv[0] is NOT a usable answer for anything that records or
    // re-launches this exe: it is whatever the launcher put on the command line,
    // so the documented workflow (cd into the output dir, run the exe) yields a
    // bare relative name. Worse for correctness, MSVC hands main() an
    // ANSI-codepage argv, so under a non-ASCII install path those bytes are not
    // valid UTF-8 -- feeding them to a strict-UTF-8 JSON writer throws. This
    // resolves the WIDE path from the OS and converts it explicitly to UTF-8, so
    // callers get well-formed bytes regardless of the active codepage.
    ARCANE_API std::string ExecutablePathUtf8();
}
