#include <Arcane/ImGui/ImDrawSnapshot.hpp>

#include <cstddef>   // offsetof
#include <cstdint>
#include <cstring>   // memcpy
#include <utility>

// Pin the Render-owned plain-data snapshot types (RenderFramePacket.hpp) to
// imgui's own, so the memcpy in both directions is byte-exact (ruling PR-1).
static_assert(sizeof(Arcane::ImDrawSnapshotVert) == sizeof(ImDrawVert));
static_assert(offsetof(Arcane::ImDrawSnapshotVert, pos) == offsetof(ImDrawVert, pos));
static_assert(offsetof(Arcane::ImDrawSnapshotVert, uv) == offsetof(ImDrawVert, uv));
static_assert(offsetof(Arcane::ImDrawSnapshotVert, col) == offsetof(ImDrawVert, col));
static_assert(sizeof(ImDrawIdx) == sizeof(std::uint16_t));
static_assert(sizeof(ImTextureID) == sizeof(std::uint64_t));

namespace Arcane
{
    bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out)
    {
        out = {};
        if (!src)
            return true;
        out.displayPos = { src->DisplayPos.x, src->DisplayPos.y };
        out.displaySize = { src->DisplaySize.x, src->DisplaySize.y };
        out.framebufferScale = { src->FramebufferScale.x, src->FramebufferScale.y };
        if (src->Textures != nullptr)
        {
            out.hasTextures = true;
            out.textures.assign(src->Textures->Data, src->Textures->Data + src->Textures->Size);
        }
        out.lists.reserve(static_cast<std::size_t>(src->CmdLists.Size));
        for (const ImDrawList* list : src->CmdLists)
        {
            if (!list)
                continue;
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback != nullptr)
                {
                    out = {};
                    return false;
                }
            }
            ImDrawListSnapshot snap;
            if (list->VtxBuffer.Size > 0)
            {
                snap.vertices.resize(static_cast<std::size_t>(list->VtxBuffer.Size));   // PR-1
                std::memcpy(snap.vertices.data(), list->VtxBuffer.Data,
                            sizeof(ImDrawVert) * static_cast<std::size_t>(list->VtxBuffer.Size));
            }
            if (list->IdxBuffer.Size > 0)
                snap.indices.assign(list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                ImDrawListSnapshot::Cmd c;
                c.clip = glm::vec4(cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w);   // PR-1
                c.idxOffset = cmd.IdxOffset;
                c.elemCount = cmd.ElemCount;
                c.vtxOffset = cmd.VtxOffset;
                // Do not call GetTexID here. A managed texture whose TexID is still
                // Invalid asserts (imgui.h:4073-4081). NewFrameTexUpdates stamps
                // that id later, at declaration.
                if (cmd.TexRef._TexData != nullptr)
                {
                    c.managed = cmd.TexRef._TexData;
                    c.texture = 0;
                }
                else
                {
                    c.managed = nullptr;
                    c.texture = cmd.TexRef._TexID;
                }
                snap.cmds.push_back(c);
            }
            out.lists.push_back(std::move(snap));
        }
        return true;
    }

    ImDrawSnapshotRebuild::ImDrawSnapshotRebuild(const ImDrawSnapshot& snap)
    {
        m_data.Valid = true;
        m_data.DisplayPos = ImVec2(snap.displayPos.x, snap.displayPos.y);
        m_data.DisplaySize = ImVec2(snap.displaySize.x, snap.displaySize.y);
        m_data.FramebufferScale = ImVec2(snap.framebufferScale.x, snap.framebufferScale.y);
        m_data.OwnerViewport = nullptr;
        m_data.CmdLists.resize(0);
        m_data.CmdListsCount = 0;
        m_data.TotalVtxCount = 0;
        m_data.TotalIdxCount = 0;
        if (snap.hasTextures)
        {
            m_textures.resize(static_cast<int>(snap.textures.size()));
            for (int i = 0; i < m_textures.Size; ++i)
                m_textures[i] = static_cast<ImTextureData*>(snap.textures[static_cast<std::size_t>(i)]);   // PR-1
            m_data.Textures = &m_textures;
        }
        else
            m_data.Textures = nullptr;
        for (const ImDrawListSnapshot& src : snap.lists)
        {
            ImDrawList* list = IM_NEW(ImDrawList(nullptr));
            if (!src.vertices.empty())
            {
                list->VtxBuffer.resize(static_cast<int>(src.vertices.size()));
                std::memcpy(list->VtxBuffer.Data, src.vertices.data(),
                            src.vertices.size() * sizeof(ImDrawVert));   // layout pinned by the static_asserts above
            }
            if (!src.indices.empty())
            {
                list->IdxBuffer.resize(static_cast<int>(src.indices.size()));
                std::memcpy(list->IdxBuffer.Data, src.indices.data(),
                            src.indices.size() * sizeof(ImDrawIdx));
            }
            for (const ImDrawListSnapshot::Cmd& c : src.cmds)
            {
                ImDrawCmd cmd{};
                cmd.ClipRect = ImVec4(c.clip.x, c.clip.y, c.clip.z, c.clip.w);   // PR-1
                if (c.managed != nullptr)
                {
                    cmd.TexRef._TexData = static_cast<ImTextureData*>(c.managed);   // PR-1
                    cmd.TexRef._TexID = ImTextureID_Invalid;
                }
                else
                    cmd.TexRef = ImTextureRef(c.texture);
                cmd.IdxOffset = c.idxOffset;
                cmd.ElemCount = c.elemCount;
                cmd.VtxOffset = c.vtxOffset;
                cmd.UserCallback = nullptr;
                list->CmdBuffer.push_back(cmd);
            }
            m_owned.push_back(list);
            m_data.CmdLists.push_back(list);
            m_data.CmdListsCount += 1;
            m_data.TotalVtxCount += list->VtxBuffer.Size;
            m_data.TotalIdxCount += list->IdxBuffer.Size;
        }
    }

    ImDrawSnapshotRebuild::~ImDrawSnapshotRebuild()
    {
        for (ImDrawList* list : m_owned)
            IM_DELETE(list);
    }
}
