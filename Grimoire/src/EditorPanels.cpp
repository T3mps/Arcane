#include "EditorPanels.hpp"
#include "ConsoleBuffer.hpp"

#include <Arcane/Sim/RunLoop.hpp>

#include <imgui.h>

namespace Grimoire
{
    void BeginDockSpace()
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("GrimoireDockHost", nullptr, flags);
        ImGui::PopStyleVar(3);
        ImGui::DockSpace(ImGui::GetID("GrimoireDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
        ImGui::End();
    }

    void DrawSimTimeToolbar(Arcane::RunLoop& loop)
    {
        ImGui::Begin("Sim");
        const bool paused = loop.IsPaused();
        if (ImGui::Button(paused ? "Play" : "Pause")) loop.SetPaused(!paused);
        ImGui::SameLine();
        if (ImGui::Button("Step")) loop.RequestSingleStep();
        ImGui::SameLine();
        float scale = static_cast<float>(loop.TimeScale());
        if (ImGui::SliderFloat("time-scale", &scale, 0.0f, 2.0f, "%.2f"))
            loop.SetTimeScale(scale);
        ImGui::End();
    }

    void DrawConsolePanel(const ConsoleBuffer& console)
    {
        ImGui::Begin("Console");
        console.ForEach([](const std::string& line)
        {
            ImGui::TextUnformatted(line.c_str());
        });
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);   // autoscroll while pinned to bottom
        ImGui::End();
    }
}
