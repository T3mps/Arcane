#pragma once

// Arcane/Base/Posix/PosixCrashSupport.hpp -- PRIVATE to ArcaneCore's POSIX
// crash path (Diagnostics POSIX port, 2026-10-05). Excluded from Windows
// builds by premake5.lua (removefiles Base/Posix/**).
//
// The POSIX twins of the Win32 primitives Base/Diagnostics.cpp's Windows half
// uses, and the same discipline: everything below may run on the crash thread
// while the process is already misbehaving, so none of it touches the heap,
// takes a lock another thread could be holding, or calls anything that might
// (no stdio streams, no localtime, no dl_iterate_phdr). Raw syscalls, fixed
// storage, and reads that FAIL instead of faulting.

#include <Arcane/Platform/Platform.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <ucontext.h>

namespace Arcane::Diagnostics::Internal::Posix
{
    // ---- heap-free file IO (open/write/close; the CreateFileW twins) --------
    [[nodiscard]] int OpenForWrite(const char* path, bool append) noexcept;   // -1 on failure
    bool WriteAll(int fd, const void* data, std::size_t size) noexcept;
    inline bool WriteAll(int fd, std::string_view s) noexcept { return WriteAll(fd, s.data(), s.size()); }
    void CloseFd(int fd) noexcept;

    // Reads a whole (small) file into `buf`, NUL-terminated; returns the byte
    // count (0 on failure). /proc files report size 0, so this reads until EOF.
    std::size_t ReadFileInto(const char* path, char* buf, std::size_t cap) noexcept;

    // Reads [address, address + size) of THIS process without risking a
    // fault: process_vm_readv, falling back to the pipe trick where a sandbox
    // filters it. Returns the bytes actually read (a prefix on a partial).
    std::size_t SafeRead(std::uint64_t address, void* out, std::size_t size) noexcept;

    // ---- contexts -----------------------------------------------------------
    // A ucontext_t that stands alone: a kernel signal frame's (or
    // getcontext's) uc_mcontext.fpregs points OUTSIDE the struct, so a plain
    // copy would dangle the moment that frame is gone. This copies the FP
    // state into the copy's own __fpregs_mem and re-points it there.
    void CopyContext(ucontext_t& dst, const ucontext_t& src) noexcept;

    [[nodiscard]] std::uint64_t ContextPc(const ucontext_t& uc) noexcept;
    [[nodiscard]] std::uint64_t ContextSp(const ucontext_t& uc) noexcept;

    // ---- other threads ------------------------------------------------------
    // The thread ids of this process, read from /proc/self/task with raw
    // getdents64 (opendir allocates). Returns the count written (<= cap).
    std::size_t ListThreads(std::uint32_t* out, std::size_t cap) noexcept;

    // The POSIX reading of SuspendThread + GetThreadContext. A real-time
    // signal (SIGRTMIN + 4, engine-reserved) is sent to `tid`; its handler
    // copies the thread's ucontext into fixed storage and PARKS -- spinning
    // on an atomic in 1 ms nanosleeps, both async-signal-safe -- until the
    // requester calls `whileParked` and releases it. The stack of a parked
    // thread does not move, which is what lets the minidump copy it.
    //
    // One thread is held at a time, never the caller. A thread that has the
    // signal blocked (some libraries block everything in their own workers)
    // is simply not captured: false after `timeoutMs`. Requests are
    // serialized by the caller (the crash thread is the only one).
    using ParkedFn = void (*)(std::uint32_t tid, const ucontext_t& context, void* user);
    bool SnapshotThread(std::uint32_t tid, std::uint32_t timeoutMs, ParkedFn whileParked, void* user) noexcept;

    // Installed at Install REGARDLESS of installCrashHandler: like the crash
    // thread, it is part of the report ENGINE (a hang report walks the main
    // thread through it), not a fault handler. Restored at Shutdown.
    bool InstallSnapshotSignal() noexcept;
    void RemoveSnapshotSignal() noexcept;
    [[nodiscard]] int SnapshotSignal() noexcept;

    // Blocks every ASYNCHRONOUS signal on the calling thread (console signals,
    // the snapshot signal). The engine's own service threads (crash thread,
    // watchdog, console forwarder) call it first, so a process-directed
    // SIGINT/SIGTERM lands on a host thread and a snapshot never targets one.
    void BlockAsyncSignalsOnThisThread() noexcept;

    // ---- the minidump (MinidumpWriter.cpp) ---------------------------------
    struct DumpThread
    {
        std::uint32_t     tid     = 0;
        const ucontext_t* context = nullptr;
    };

    struct DumpRequest
    {
        const char* path = nullptr;

        // Never in the dump: the crash thread itself.
        std::uint32_t excludeTid = 0;

        // Threads whose context the report already holds (the faulting
        // thread, the parked submitter); everything else is snapshotted.
        const DumpThread* known      = nullptr;
        std::size_t       knownCount = 0;

        // The exception stream. A real fault carries the signal; a hang or a
        // manual report carries Breakpad's DUMP_REQUESTED code around the
        // walked thread's context -- the Windows path's STILL_ACTIVE record,
        // and for the same reason: every dump opens on a stored event.
        std::uint32_t     exceptionTid     = 0;
        const ucontext_t* exceptionContext = nullptr;
        std::uint32_t     exceptionCode    = 0;   // signal number, or kDumpRequested
        std::uint32_t     exceptionFlags   = 0;   // si_code
        std::uint64_t     exceptionAddress = 0;   // si_addr (or the pc)
    };

    inline constexpr std::uint32_t kDumpRequested = 0xFFFFFFFFu;   // MD_EXCEPTION_CODE_LIN_DUMP_REQUESTED

    // Writes a Breakpad-format minidump (the Microsoft MDMP container with
    // Breakpad's Linux streams) -- see MinidumpWriter.cpp for the layout.
    bool WriteMinidump(const DumpRequest& request) noexcept;

    // Off the crash path, at Install: uname, CPU count/vendor for the
    // SystemInfo stream.
    void SnapshotSystemInfoForDump() noexcept;
}
