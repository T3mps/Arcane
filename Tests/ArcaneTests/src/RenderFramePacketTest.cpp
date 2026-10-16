#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/ImGui/ImDrawSnapshot.hpp>   // CopyImDrawSnapshot / ImDrawSnapshotRebuild (ArcaneImGui) ruling PR-1

#include "Helpers/ConstantScan.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cstring>
#include <fstream>
#include <sstream>

TEST_CASE("a packet slot stays intact until FramesInFlight extracts have passed", "[nri][packet]")
{
    Arcane::LatchFramesInFlight();
    const std::uint32_t n = Arcane::FramesInFlight();
    REQUIRE(n >= 2u);
    REQUIRE(n <= Arcane::kMaxFramesInFlight);

    Arcane::PacketRing ring;
    Arcane::RenderFramePacket& first = ring.BeginExtract();
    CHECK(&ring.Latest() == &first);
    first.batchVertices.push_back(Arcane::Batch2DVertex{});
    first.batchVertices.back().pos = { 1.0f, 2.0f, 3.0f };
    first.origin.translation = { 0.0f, 0.0f, 0.0f };
    const Arcane::Batch2DVertex* span = first.batchVertices.data();
    const float kept = span->pos.x;

    for (std::uint32_t i = 0; i < n - 1; ++i)
    {
        Arcane::RenderFramePacket& slot = ring.BeginExtract();
        slot.batchVertices.push_back(Arcane::Batch2DVertex{});
        slot.batchVertices.back().pos = { 9.0f, 9.0f, 9.0f };
    }

    const Arcane::RenderFramePacket& still = ring.Slot(n - 1);
    REQUIRE(still.batchVertices.size() == 1u);
    CHECK(still.batchVertices[0].pos.x == kept);
    CHECK(still.origin.translation == glm::vec3(0.0f));
    // The span handed out at extract still addresses that slot's vector.
    CHECK(span == still.batchVertices.data());

    Arcane::RenderFramePacket& overwritten = ring.BeginExtract();
    CHECK(overwritten.batchVertices.empty());   // BeginExtract clears the recycled slot
}

TEST_CASE("mutating a source vector after the copy does not change the packet", "[nri][packet]")
{
    std::vector<Arcane::Batch2DVertex> source(1);
    source[0].pos = { 4.0f, 5.0f, 6.0f };
    Arcane::RenderFramePacket packet;
    packet.batchVertices = source;
    source[0].pos = { 0.0f, 0.0f, 0.0f };
    source.clear();
    CHECK(packet.batchVertices.size() == 1u);
    CHECK(packet.batchVertices[0].pos.x == 4.0f);
}

TEST_CASE("RenderFramePacket.hpp does not include the Astra registry", "[nri][packet]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.hpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("Registry.hpp") == std::string::npos);
    CHECK(text.find("Astra::Registry") == std::string::npos);
    CHECK(text.find("VisibilitySystem.hpp") == std::string::npos);
    CHECK(text.find("SceneCamera.hpp") == std::string::npos);
    CHECK(text.find("imgui.h") == std::string::npos);   // ruling PR-1
    CHECK(text.find("SceneCameraView.hpp") != std::string::npos);
    CHECK(text.find("optional<SceneCameraView> spriteView") != std::string::npos);
    CHECK(text.find("optional<ViewTransform> meshView") != std::string::npos);
}

TEST_CASE("spriteView is a SceneCameraView and meshView is a ViewTransform", "[nri][packet]")
{
    Arcane::RenderFramePacket packet;
    Arcane::SceneCameraView sprite;
    sprite.worldCenter = { 1.0f, 2.0f };
    sprite.halfHeight = 3.0f;
    sprite.view.view = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 0.0f, 0.0f));
    sprite.view.projection = glm::mat4(1.0f);
    sprite.view.viewport = { 8u, 9u };
    packet.spriteView = sprite;
    Arcane::ViewTransform mesh;
    mesh.projection = glm::mat4(2.0f);
    packet.meshView = mesh;
    REQUIRE(packet.spriteView.has_value());
    CHECK(packet.spriteView->worldCenter == glm::vec2(1.0f, 2.0f));
    CHECK(packet.spriteView->halfHeight == 3.0f);
    CHECK(packet.spriteView->view.view[3][0] == 4.0f);
    CHECK(packet.spriteView->view.viewport == glm::uvec2(8u, 9u));
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.meshView->projection[0][0] == 2.0f);
}

static void DummyImGuiCallback(const ImDrawList*, const ImDrawCmd*) {}

TEST_CASE("an ImDrawSnapshot matches the live draw data field for field and refuses a callback", "[nri][packet]")
{
    // A default ImDrawData has Textures == nullptr (imgui_draw.cpp:2284-2291).
    // That arm is not what ImGui::Render produces.
    ImDrawData cleared;
    Arcane::ImDrawSnapshot clearedSnap;
    REQUIRE(Arcane::CopyImDrawSnapshot(&cleared, clearedSnap));
    CHECK_FALSE(clearedSnap.hasTextures);
    CHECK(clearedSnap.textures.empty());
    Arcane::ImDrawSnapshotRebuild clearedRebuild(clearedSnap);
    CHECK(clearedRebuild.Data()->Textures == nullptr);
    CHECK(clearedRebuild.Data()->Valid);
    CHECK(clearedRebuild.Data()->CmdLists.Size == 0);

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(128.0f, 128.0f);
    // Before NewFrame. NewFrame copies this into ImDrawListFlags_AllowVtxOffset
    // (imgui.cpp:5706-5707). A window list created afterwards carries the flag.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    // Matches the live backend (ImGuiNri.cpp:119). Without it NewFrame demands a
    // built font atlas (imgui_draw.cpp:2778).
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    ImGui::NewFrame();
    // A window created without both sizes set by the API is hidden for its first
    // frame (imgui.cpp:8093-8095) and a hidden window is left out of the draw
    // data (imgui.cpp:6394-6395). Every window here is rendered on its first
    // frame, so each sets its position and size first.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(64.0f, 64.0f), ImGuiCond_Always);
    ImGui::Begin("one");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(42), ImVec2(0.0f, 0.0f), ImVec2(8.0f, 8.0f));
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(64.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(64.0f, 64.0f), ImGuiCond_Always);
    ImGui::Begin("two");
    ImDrawList* list = ImGui::GetWindowDrawList();
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(7), ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f));
    // Chrome already consumed some of the 16-bit index space. Pad from the
    // current _VtxCurrentIdx, not from zero, up to but not over the boundary.
    // The following AddRectFilled's PrimReserve then crosses it
    // (imgui_draw.cpp:737-743) and the new command's VtxOffset is the buffer
    // size at the crossing (imgui_draw.cpp:646-659, AddDrawCmd at :518).
    const int pad = (1 << 16) - static_cast<int>(list->_VtxCurrentIdx) - 1;
    REQUIRE(pad > 0);
    list->PrimReserve(0, pad);
    for (int i = 0; i < pad; ++i)
    {
        list->_VtxWritePtr->pos = ImVec2(0.0f, 0.0f);
        list->_VtxWritePtr->uv = ImVec2(0.0f, 0.0f);
        list->_VtxWritePtr->col = IM_COL32_WHITE;
        ++list->_VtxWritePtr;
        ++list->_VtxCurrentIdx;
    }
    list->AddRectFilled(ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f), IM_COL32(255, 0, 0, 255));
    ImGui::End();
    ImGui::Render();

    ImDrawData* src = ImGui::GetDrawData();
    REQUIRE(src != nullptr);
    REQUIRE(src->Valid);
    REQUIRE(src->CmdLists.Size >= 2);
    REQUIRE(src->Textures != nullptr);
    bool sawOffset = false;
    for (const ImDrawList* live : src->CmdLists)
    {
        REQUIRE(live != nullptr);
        for (const ImDrawCmd& cmd : live->CmdBuffer)
            if (cmd.UserCallback == nullptr && cmd.VtxOffset > 0u)
                sawOffset = true;
    }
    REQUIRE(sawOffset);

    Arcane::ImDrawSnapshot snap;
    REQUIRE(Arcane::CopyImDrawSnapshot(src, snap));
    REQUIRE(snap.lists.size() == static_cast<std::size_t>(src->CmdLists.Size));
    CHECK(snap.hasTextures);
    REQUIRE(snap.textures.size() == static_cast<std::size_t>(src->Textures->Size));
    for (int i = 0; i < src->Textures->Size; ++i)
        CHECK(snap.textures[static_cast<std::size_t>(i)] == (*src->Textures)[i]);
    CHECK(snap.displayPos == glm::vec2(src->DisplayPos.x, src->DisplayPos.y));
    CHECK(snap.displaySize == glm::vec2(src->DisplaySize.x, src->DisplaySize.y));
    CHECK(snap.framebufferScale == glm::vec2(src->FramebufferScale.x, src->FramebufferScale.y));

    bool saw42 = false;
    bool saw7 = false;
    for (std::size_t li = 0; li < snap.lists.size(); ++li)
    {
        const ImDrawList* live = src->CmdLists[static_cast<int>(li)];
        const Arcane::ImDrawListSnapshot& copied = snap.lists[li];
        REQUIRE(copied.vertices.size() == static_cast<std::size_t>(live->VtxBuffer.Size));
        REQUIRE(copied.indices.size() == static_cast<std::size_t>(live->IdxBuffer.Size));
        REQUIRE(copied.cmds.size() == static_cast<std::size_t>(live->CmdBuffer.Size));
        if (live->VtxBuffer.Size > 0)
            CHECK(std::memcmp(copied.vertices.data(), live->VtxBuffer.Data,
                              sizeof(ImDrawVert) * static_cast<std::size_t>(live->VtxBuffer.Size)) == 0);
        if (live->IdxBuffer.Size > 0)
            CHECK(std::memcmp(copied.indices.data(), live->IdxBuffer.Data,
                              sizeof(ImDrawIdx) * static_cast<std::size_t>(live->IdxBuffer.Size)) == 0);
        for (std::size_t ci = 0; ci < copied.cmds.size(); ++ci)
        {
            const ImDrawCmd& liveCmd = live->CmdBuffer[static_cast<int>(ci)];
            const Arcane::ImDrawListSnapshot::Cmd& c = copied.cmds[ci];
            CHECK(c.elemCount == liveCmd.ElemCount);
            CHECK(c.idxOffset == liveCmd.IdxOffset);
            CHECK(c.vtxOffset == liveCmd.VtxOffset);
            CHECK(c.clip.x == liveCmd.ClipRect.x);
            CHECK(c.clip.y == liveCmd.ClipRect.y);
            CHECK(c.clip.z == liveCmd.ClipRect.z);
            CHECK(c.clip.w == liveCmd.ClipRect.w);
            CHECK(c.managed == liveCmd.TexRef._TexData);
            if (liveCmd.TexRef._TexData == nullptr)
            {
                CHECK(c.texture == liveCmd.TexRef._TexID);
                if (c.texture == static_cast<ImTextureID>(42))
                    saw42 = true;
                if (c.texture == static_cast<ImTextureID>(7))
                    saw7 = true;
            }
            else
                CHECK(c.texture == static_cast<ImTextureID>(0));
        }
    }
    CHECK(saw42);
    CHECK(saw7);

    Arcane::ImDrawSnapshotRebuild rebuilt(snap);
    ImDrawData* data = rebuilt.Data();
    REQUIRE(data->Valid);
    CHECK(data->CmdListsCount == src->CmdListsCount);
    REQUIRE(data->CmdLists.Size == src->CmdLists.Size);
    CHECK(data->TotalVtxCount == src->TotalVtxCount);
    CHECK(data->TotalIdxCount == src->TotalIdxCount);
    CHECK(data->DisplayPos.x == src->DisplayPos.x);
    CHECK(data->DisplayPos.y == src->DisplayPos.y);
    CHECK(data->DisplaySize.x == src->DisplaySize.x);
    CHECK(data->DisplaySize.y == src->DisplaySize.y);
    CHECK(data->FramebufferScale.x == src->FramebufferScale.x);
    CHECK(data->FramebufferScale.y == src->FramebufferScale.y);
    REQUIRE(data->Textures != nullptr);
    REQUIRE(data->Textures->Size == src->Textures->Size);
    // The vector is the rebuild's. The ImTextureData objects are the live ones.
    CHECK(data->Textures != src->Textures);
    for (int i = 0; i < src->Textures->Size; ++i)
        CHECK((*data->Textures)[i] == (*src->Textures)[i]);
    for (int li = 0; li < data->CmdLists.Size; ++li)
    {
        const ImDrawList* live = src->CmdLists[li];
        const ImDrawList* copy = data->CmdLists[li];
        REQUIRE(copy->VtxBuffer.Size == live->VtxBuffer.Size);
        REQUIRE(copy->IdxBuffer.Size == live->IdxBuffer.Size);
        REQUIRE(copy->CmdBuffer.Size == live->CmdBuffer.Size);
        if (live->VtxBuffer.Size > 0)
            CHECK(std::memcmp(copy->VtxBuffer.Data, live->VtxBuffer.Data,
                              sizeof(ImDrawVert) * static_cast<std::size_t>(live->VtxBuffer.Size)) == 0);
        if (live->IdxBuffer.Size > 0)
            CHECK(std::memcmp(copy->IdxBuffer.Data, live->IdxBuffer.Data,
                              sizeof(ImDrawIdx) * static_cast<std::size_t>(live->IdxBuffer.Size)) == 0);
        for (int ci = 0; ci < copy->CmdBuffer.Size; ++ci)
        {
            const ImDrawCmd& a = live->CmdBuffer[ci];
            const ImDrawCmd& b = copy->CmdBuffer[ci];
            CHECK(b.ElemCount == a.ElemCount);
            CHECK(b.IdxOffset == a.IdxOffset);
            CHECK(b.VtxOffset == a.VtxOffset);
            CHECK(b.UserCallback == nullptr);
            CHECK(b.ClipRect.x == a.ClipRect.x);
            CHECK(b.ClipRect.y == a.ClipRect.y);
            CHECK(b.ClipRect.z == a.ClipRect.z);
            CHECK(b.ClipRect.w == a.ClipRect.w);
            CHECK(b.TexRef._TexData == a.TexRef._TexData);
            CHECK(b.TexRef._TexID == a.TexRef._TexID);
            // GetTexID asserts when _TexData is set and TexID is still Invalid
            // (imgui.h:4080-4081). This CPU frame never uploads the atlas.
            if (a.TexRef._TexData == nullptr)
                CHECK(b.GetTexID() == a.GetTexID());
        }
    }

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(64.0f, 64.0f), ImGuiCond_Always);
    ImGui::Begin("callback");
    ImGui::GetWindowDrawList()->AddCallback(&DummyImGuiCallback, nullptr);
    ImGui::End();
    ImGui::Render();
    Arcane::ImDrawSnapshot refused;
    CHECK_FALSE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), refused));
    CHECK(refused.lists.empty());
    CHECK_FALSE(refused.hasTextures);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
}
