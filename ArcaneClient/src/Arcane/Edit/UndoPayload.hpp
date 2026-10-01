#pragma once

// Arcane/Edit: a move-only, spillable undo byte blob (spec 2026-09-30 s3.3e).
// Made ONLY by CommandStack::MakePayload / MakePayloadFromFile, which decide
// memory vs <spill dir>/<step>.bin. All spilled payloads of one step share
// that file at recorded offsets; the file is removed with its last holder.

#include <Arcane/Base/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD
#endif
    namespace Detail
    {
        struct ARCANE_API UndoSpillFile
        {
            explicit UndoSpillFile(std::filesystem::path p) : path(std::move(p)) {}
            ~UndoSpillFile();   // removes the file
            UndoSpillFile(const UndoSpillFile&) = delete;
            UndoSpillFile& operator=(const UndoSpillFile&) = delete;

            std::filesystem::path path;
            std::uint64_t         size = 0;   // committed bytes = the next payload's offset
        };
    }

    class ARCANE_API UndoPayload
    {
    public:
        UndoPayload() = default;
        UndoPayload(UndoPayload&&) noexcept = default;
        UndoPayload& operator=(UndoPayload&&) noexcept = default;
        UndoPayload(const UndoPayload&) = delete;
        UndoPayload& operator=(const UndoPayload&) = delete;

        // Counted whether in memory or spilled (byteBudgetMB bounds RAM + disk).
        [[nodiscard]] std::size_t Size() const noexcept { return m_size; }
        [[nodiscard]] bool Empty() const noexcept { return m_size == 0; }
        [[nodiscard]] bool Spilled() const noexcept { return m_file != nullptr; }
        [[nodiscard]] std::filesystem::path SpillPath() const;
        // nullopt = the spill file could not be read; the caller logs and no-ops.
        [[nodiscard]] std::optional<std::vector<std::byte>> Load() const;

    private:
        friend class CommandStack;
        std::vector<std::byte>                 m_bytes;   // empty once spilled
        std::shared_ptr<Detail::UndoSpillFile> m_file;
        std::uint64_t                          m_offset = 0;
        std::size_t                            m_size = 0;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
