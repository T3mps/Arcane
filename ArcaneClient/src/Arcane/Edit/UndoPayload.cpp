#include <Arcane/Edit/UndoPayload.hpp>

#include <fstream>
#include <system_error>

namespace Arcane
{
    Detail::UndoSpillFile::~UndoSpillFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);   // gone with its last payload
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
