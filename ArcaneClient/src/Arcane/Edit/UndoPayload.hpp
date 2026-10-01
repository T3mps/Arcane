#pragma once

// Arcane/Edit: a move-only, spillable undo byte blob (spec 2026-09-30 s3.3e).
// Made ONLY by CommandStack::MakePayload / MakePayloadFromFile, which decide
// memory vs <spill dir>/<step>.bin. All spilled payloads of one step share
// that file at recorded offsets; the file is removed with its last holder.
//
// The file is append-only while any of its payloads lives: dropping one
// payload does not reclaim its bytes. Only when the step's LAST live payload
// goes (live == 0) is the file truncated, so the next payload rewrites it
// from offset 0. A consumer that re-makes a step's payloads (a re-capture
// on every Undo/Redo replay) MUST therefore drop all the old ones first,
// then make the new ones -- making before dropping grows the file by the
// re-made bytes on every replay, past what PayloadBytes() counts.

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
            // Bytes held by live payloads. Raised when a payload adopts the
            // file, lowered when one dies or is moved over; at 0 the file is
            // truncated and `size` resets (see the header comment).
            std::uint64_t         live = 0;
        };
    }

    class ARCANE_API UndoPayload
    {
    public:
        UndoPayload() = default;
        ~UndoPayload();   // gives its bytes back to the step file's `live`
        // A moved-from payload is empty: no file, no bytes, Size() == 0.
        UndoPayload(UndoPayload&& other) noexcept;
        UndoPayload& operator=(UndoPayload&& other) noexcept;
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
        void Release() noexcept;   // drops this payload's hold on its file
        std::vector<std::byte>                 m_bytes;   // empty once spilled
        std::shared_ptr<Detail::UndoSpillFile> m_file;
        std::uint64_t                          m_offset = 0;
        std::size_t                            m_size = 0;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
