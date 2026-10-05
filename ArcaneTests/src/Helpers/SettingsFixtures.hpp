#pragma once

// Settings arc S3: fixtures for the settings-window tests -- register a fully
// described cvar on a PRIVATE registry, and the device-less ImGui harness
// (software atlas, 1920x1080, no ini) the row/window tests click through.
// Catch2-free.

#include <Arcane/Config/CVarRegistry.hpp>
#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Arcane::Test
{
    struct SettingSpec
    {
        CVarType type = CVarType::Int32;
        CVarValue def = CVarValue::Int32(0);
        SettingScope scope = SettingScope::Project;
        ApplyMode apply = ApplyMode::Live;
        CVarFlags flags = CVarFlags::None;
        std::string_view module = "test";
        std::string_view displayName, keywords, widget, categoryPath, group;
        std::optional<CVarValue> min, max;
        std::vector<std::string> enumNames;
        Audience audience = Audience::Game;
        std::int32_t order = 0;
        std::string_view help = "A test setting.";
    };

    inline CVarHandle AddSetting(CVarRegistry& reg, std::string_view name, const SettingSpec& s)
    {
        CVarDesc d;
        d.name = name;
        d.type = s.type;
        d.defaultValue = s.def;
        d.min = s.min;
        d.max = s.max;
        d.flags = s.flags;
        d.help = s.help;
        d.module = s.module;
        d.displayName = s.displayName;
        d.keywords = s.keywords;
        d.widget = s.widget;
        d.audience = s.audience;
        d.scope = s.scope;
        d.apply = s.apply;
        d.order = s.order;
        d.categoryPath = s.categoryPath;
        d.group = s.group;
        d.enumNames = s.enumNames;
        return reg.Register(d);
    }

    struct SettingsImGuiHarness
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        std::unordered_map<std::string, ImVec2> probe;

        explicit SettingsImGuiHarness(ImVec2 display = ImVec2(1920.0f, 1080.0f))
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = display;
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~SettingsImGuiHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        SettingsImGuiHarness(const SettingsImGuiHarness&) = delete;
        SettingsImGuiHarness& operator=(const SettingsImGuiHarness&) = delete;

        // One frame around `body`; the probe map is cleared first, so it holds THIS frame's centres.
        template <class Body> void Frame(Body&& body)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            body();
            ImGui::Render();
        }
    };

    template <class FrameFn> void ClickAt(ImVec2 at, FrameFn&& frame)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
    }
    // Press at `at`, move dx in three steps (each beats the drag threshold), optionally Escape, release.
    template <class FrameFn> void DragAt(ImVec2 at, float dx, FrameFn&& frame, bool escapeBeforeRelease = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        for (int i = 1; i <= 3; ++i) { io.AddMousePosEvent(at.x + dx * static_cast<float>(i) / 3.0f, at.y); frame(); }
        if (escapeBeforeRelease) { io.AddKeyEvent(ImGuiKey_Escape, true); frame(); io.AddKeyEvent(ImGuiKey_Escape, false); frame(); }
        io.AddMouseButtonEvent(0, false); frame();
    }
    template <class FrameFn> void PressChord(ImGuiKey mod, ImGuiKey key, FrameFn&& frame)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(mod, true); io.AddKeyEvent(key, true); frame();
        io.AddKeyEvent(key, false); io.AddKeyEvent(mod, false); frame();
    }
    template <class FrameFn> void PressKey(ImGuiKey key, FrameFn&& frame)
    {
        ImGui::GetIO().AddKeyEvent(key, true); frame();
        ImGui::GetIO().AddKeyEvent(key, false); frame();
    }
    template <class FrameFn> void TypeText(const char* text, FrameFn&& frame)
    {
        ImGui::GetIO().AddInputCharactersUTF8(text); frame();
    }
}
