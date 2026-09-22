// Crash window plan 1 (Task 2): CrashArena implementation. See the header
// for the "why" -- this file is deliberately boring: pointer bumping,
// vsnprintf into a fixed slot, and a memcpy that stops at the reserved
// span's end. Nothing here allocates from the heap or throws.

#include <Arcane/Base/CrashArena.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // The static 256 KiB block backing CrashArena::Instance(). File-scope
    // (not function-local to Instance()) so the default constructor -- run
    // once, by Instance()'s function-local static CrashArena -- can point
    // m_buffer at it directly. Never freed; the process owns it for its
    // whole lifetime.
    alignas(64) char g_block[Arcane::Diagnostics::CrashArena::kCapacity];
}

namespace Arcane::Diagnostics
{
    CrashArena::CrashArena()
        : m_buffer(g_block)
        , m_capacity(kCapacity)
        , m_used(0)
        , m_exhausted(false)
        , m_owned(false)
    {
    }

    CrashArena::CrashArena(std::size_t capacityBytes)
        : m_buffer(static_cast<char*>(std::malloc(capacityBytes)))
        , m_capacity(capacityBytes)
        , m_used(0)
        , m_exhausted(false)
        , m_owned(true)
    {
    }

    CrashArena::~CrashArena()
    {
        if (m_owned && m_buffer != nullptr)
        {
            std::free(m_buffer);
        }
    }

    CrashArena& CrashArena::Instance() noexcept
    {
        static CrashArena s_instance;
        return s_instance;
    }

    void CrashArena::Reset() noexcept
    {
        m_used = 0;
        m_exhausted = false;
    }

    void* CrashArena::Alloc(std::size_t bytes, std::size_t align) noexcept
    {
        if (align == 0)
        {
            align = 1;
        }

        const auto base    = reinterpret_cast<std::uintptr_t>(m_buffer);
        const auto current = base + m_used;
        const auto aligned = (current + (align - 1)) & ~(align - 1);
        const auto padding = static_cast<std::size_t>(aligned - current);

        if (padding > m_capacity - m_used || bytes > m_capacity - m_used - padding)
        {
            m_exhausted = true;
            return nullptr;
        }

        m_used += padding + bytes;
        return m_buffer + (aligned - base);
    }

    const char* CrashArena::Format(const char* fmt, ...) noexcept
    {
        char* slot = static_cast<char*>(Alloc(512, 16));
        if (slot == nullptr)
        {
            return "";   // Alloc already marked the arena exhausted.
        }

        va_list args;
        va_start(args, fmt);
        std::vsnprintf(slot, 512, fmt, args);
        va_end(args);
        slot[511] = '\0';   // guarantee NUL-termination even on a truncated write.
        return slot;
    }

    CrashArena::Builder CrashArena::OpenBuilder(std::size_t reserveBytes) noexcept
    {
        char* p = static_cast<char*>(Alloc(reserveBytes, 1));
        if (p == nullptr)
        {
            // Failed carve: a zero-length span at the buffer's own end, which
            // is a valid one-past-the-end pointer -- never a bare nullptr, so
            // Builder::Append's pointer arithmetic stays well-defined. Alloc
            // already marked the arena exhausted.
            char* sentinel = m_buffer + m_capacity;
            return Builder{ sentinel, sentinel, sentinel, this };
        }
        return Builder{ p, p, p + reserveBytes, this };
    }

    bool CrashArena::Exhausted() const noexcept { return m_exhausted; }
    std::size_t CrashArena::Used() const noexcept { return m_used; }

    void CrashArena::Builder::Append(std::string_view s) noexcept
    {
        const std::size_t remaining = static_cast<std::size_t>(end - cursor);
        const std::size_t n = s.size() < remaining ? s.size() : remaining;
        if (n > 0)
        {
            std::memcpy(cursor, s.data(), n);
            cursor += n;
        }
        if (n < s.size() && owner != nullptr)
        {
            owner->m_exhausted = true;
        }
    }

    std::string_view CrashArena::Builder::View() const noexcept
    {
        return std::string_view(begin, static_cast<std::size_t>(cursor - begin));
    }
}
