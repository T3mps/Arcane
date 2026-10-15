#include <Arcane/Edit/UndoPayload.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace Arcane
{
    Detail::UndoSpillFile::~UndoSpillFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);   // gone with its last payload
    }

    UndoPayload::~UndoPayload()
    {
        Release();
    }

    UndoPayload::UndoPayload(UndoPayload&& other) noexcept
        : m_bytes(std::move(other.m_bytes))
        , m_file(std::move(other.m_file))
        , m_offset(std::exchange(other.m_offset, 0))
        , m_size(std::exchange(other.m_size, 0))
    {
        other.m_bytes.clear();
        other.m_file.reset();
    }

    UndoPayload& UndoPayload::operator=(UndoPayload&& other) noexcept
    {
        if (this == &other)
            return *this;
        Release();   // a spilled target gives its bytes back first
        m_bytes  = std::move(other.m_bytes);
        m_file   = std::move(other.m_file);
        m_offset = std::exchange(other.m_offset, 0);
        m_size   = std::exchange(other.m_size, 0);
        other.m_bytes.clear();
        other.m_file.reset();
        return *this;
    }

    void UndoPayload::Release() noexcept
    {
        if (!m_file)
            return;
        Detail::UndoSpillFile& f = *m_file;
        f.live -= std::min<std::uint64_t>(f.live, m_size);
        if (f.live == 0)
        {
            // The step holds nothing live: the next payload rewrites from 0.
            f.size = 0;
            if (m_file.use_count() > 1)   // else the file goes with us anyway
            {
                std::error_code ec;
                std::filesystem::resize_file(f.path, 0, ec);
            }
        }
        m_file.reset();
        m_offset = 0;
    }

    std::filesystem::path UndoPayload::SpillPath() const
    {
        return m_file ? m_file->path : std::filesystem::path{};
    }

    std::optional<std::vector<std::byte>> UndoPayload::Load() const
    {
        if (!m_file)
            return m_bytes;
        std::ifstream in(m_file->path, std::ios::binary);
        if (!in)
            return std::nullopt;
        in.seekg(static_cast<std::streamoff>(m_offset));
        std::vector<std::byte> out(m_size);
        if (!in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(m_size)))
            return std::nullopt;
        return out;
    }
}
