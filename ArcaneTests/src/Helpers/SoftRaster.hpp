#pragma once

// SoftRaster (T3-D6 fix round 1): a CPU rasterizer for a device-less ImGui
// frame's draw data, so a headless harness that drives the REAL panels can
// leave a picture of the frame it judged -- desk-pass evidence for the items a
// scripted editor host cannot reach (a stationary pin hover, a marquee drag,
// a dialog opened from a menu). TEST EVIDENCE ONLY, never a golden: it is a
// straightforward triangle fill (nearest-texel sampling of the font atlas,
// per-vertex colour, straight-alpha blending, clip rects honoured), not the
// GPU's exact rasterization rules.
//
// OPT IN: CaptureFrameIfRequested writes nothing unless the environment
// variable ARCANE_TEST_CAPTURE_DIR names a directory, so an ordinary suite
// run touches no files. An image whose texture is not an ImGui-owned
// ImTextureData (a thumbnail id; device-less it is always 0) draws as a flat
// grey placeholder.

#include <imgui.h>

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Arcane::Test
{
    // RGB8 pixels of `dd`'s display rectangle (DisplayPos/DisplaySize, scale 1).
    inline std::vector<unsigned char> RasterizeDrawData(const ImDrawData* dd, int& outW, int& outH)
    {
        outW = dd ? static_cast<int>(dd->DisplaySize.x) : 0;
        outH = dd ? static_cast<int>(dd->DisplaySize.y) : 0;
        if (outW <= 0 || outH <= 0)
            return {};
        const int W = outW, H = outH;
        std::vector<float> rgb(static_cast<std::size_t>(W) * H * 3, 0.0f);
        for (std::size_t i = 0; i < rgb.size(); i += 3)
        {
            rgb[i + 0] = 0.11f; rgb[i + 1] = 0.11f; rgb[i + 2] = 0.12f;   // the empty display
        }
        const ImVec2 origin = dd->DisplayPos;
        for (const ImDrawList* list : dd->CmdLists)
        {
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback != nullptr || cmd.ElemCount == 0)
                    continue;
                const ImTextureData* tex = cmd.TexRef._TexData;
                const bool sampled = tex && tex->Pixels && tex->Width > 0 && tex->Height > 0;
                const int cx0 = std::max(0, static_cast<int>(std::floor(cmd.ClipRect.x - origin.x)));
                const int cy0 = std::max(0, static_cast<int>(std::floor(cmd.ClipRect.y - origin.y)));
                const int cx1 = std::min(W, static_cast<int>(std::ceil(cmd.ClipRect.z - origin.x)));
                const int cy1 = std::min(H, static_cast<int>(std::ceil(cmd.ClipRect.w - origin.y)));
                if (cx0 >= cx1 || cy0 >= cy1)
                    continue;
                for (unsigned int e = 0; e + 2 < cmd.ElemCount; e += 3)
                {
                    const ImDrawVert* v[3];
                    for (int k = 0; k < 3; ++k)
                        v[k] = &list->VtxBuffer[static_cast<int>(cmd.VtxOffset + list->IdxBuffer[static_cast<int>(cmd.IdxOffset + e + k)])];
                    const float x0 = v[0]->pos.x - origin.x, y0 = v[0]->pos.y - origin.y;
                    const float x1 = v[1]->pos.x - origin.x, y1 = v[1]->pos.y - origin.y;
                    const float x2 = v[2]->pos.x - origin.x, y2 = v[2]->pos.y - origin.y;
                    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                    if (std::fabs(area) < 1e-6f)
                        continue;
                    const int bx0 = std::max(cx0, static_cast<int>(std::floor(std::min({ x0, x1, x2 }))));
                    const int by0 = std::max(cy0, static_cast<int>(std::floor(std::min({ y0, y1, y2 }))));
                    const int bx1 = std::min(cx1, static_cast<int>(std::ceil(std::max({ x0, x1, x2 }))));
                    const int by1 = std::min(cy1, static_cast<int>(std::ceil(std::max({ y0, y1, y2 }))));
                    float col[3][4];
                    for (int k = 0; k < 3; ++k)
                    {
                        const ImU32 c = v[k]->col;
                        col[k][0] = static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
                        col[k][1] = static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
                        col[k][2] = static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
                        col[k][3] = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
                    }
                    for (int py = by0; py < by1; ++py)
                        for (int px = bx0; px < bx1; ++px)
                        {
                            const float sx = static_cast<float>(px) + 0.5f, sy = static_cast<float>(py) + 0.5f;
                            const float w0 = ((x1 - sx) * (y2 - sy) - (x2 - sx) * (y1 - sy)) / area;
                            const float w1 = ((x2 - sx) * (y0 - sy) - (x0 - sx) * (y2 - sy)) / area;
                            const float w2 = 1.0f - w0 - w1;
                            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
                                continue;
                            float c[4];
                            for (int ch = 0; ch < 4; ++ch)
                                c[ch] = w0 * col[0][ch] + w1 * col[1][ch] + w2 * col[2][ch];
                            if (sampled)
                            {
                                const float u = w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x;
                                const float t = w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y;
                                const int tx = std::clamp(static_cast<int>(u * static_cast<float>(tex->Width)), 0, tex->Width - 1);
                                const int ty = std::clamp(static_cast<int>(t * static_cast<float>(tex->Height)), 0, tex->Height - 1);
                                const unsigned char* p = tex->Pixels +
                                    (static_cast<std::size_t>(ty) * tex->Width + tx) * tex->BytesPerPixel;
                                if (tex->BytesPerPixel >= 4)
                                {
                                    for (int ch = 0; ch < 4; ++ch)
                                        c[ch] *= static_cast<float>(p[ch]) / 255.0f;
                                }
                                else
                                {
                                    c[3] *= static_cast<float>(p[0]) / 255.0f;
                                }
                            }
                            else
                            {
                                c[0] *= 0.35f; c[1] *= 0.35f; c[2] *= 0.37f;   // an unresolvable image: grey placeholder
                            }
                            float* d = &rgb[(static_cast<std::size_t>(py) * W + px) * 3];
                            for (int ch = 0; ch < 3; ++ch)
                                d[ch] = c[ch] * c[3] + d[ch] * (1.0f - c[3]);
                        }
                }
            }
        }
        std::vector<unsigned char> out(rgb.size());
        for (std::size_t i = 0; i < rgb.size(); ++i)
            out[i] = static_cast<unsigned char>(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        return out;
    }

    inline bool WriteDrawDataPng(const ImDrawData* dd, const std::filesystem::path& path)
    {
        int w = 0, h = 0;
        const std::vector<unsigned char> rgb = RasterizeDrawData(dd, w, h);
        if (rgb.empty())
            return false;
        std::vector<unsigned char> png;
        const auto sink = [](void* ctx, void* data, int size)
        {
            auto* bytes = static_cast<std::vector<unsigned char>*>(ctx);
            bytes->insert(bytes->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
        };
        if (stbi_write_png_to_func(sink, &png, w, h, 3, rgb.data(), w * 3) == 0)
            return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        return static_cast<bool>(f);
    }

    // The CURRENT context's last rendered frame (call after ImGui::Render), as
    // <ARCANE_TEST_CAPTURE_DIR>/<name>.png -- only when that variable is set.
    inline void CaptureFrameIfRequested(const char* name)
    {
        const char* dir = std::getenv("ARCANE_TEST_CAPTURE_DIR");
        if (dir == nullptr || *dir == '\0')
            return;
        (void)WriteDrawDataPng(ImGui::GetDrawData(),
                               std::filesystem::path(dir) / (std::string(name) + ".png"));
    }
}
