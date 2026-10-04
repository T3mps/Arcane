#include <Arcane/ImGui/ConsoleInputLine.hpp>

#include <imgui.h>

#include <cstdio>
#include <string>

namespace Arcane
{
    namespace
    {
        struct LineContext
        {
            ConsoleModel*       model;
            const CVarRegistry* registry;
            CVarContext         ctx;   // completion lists only what this context may read
        };

        int ConsoleLineCallback(ImGuiInputTextCallbackData* data)
        {
            auto* ctx = static_cast<LineContext*>(data->UserData);
            ctx->model->SetInput(std::string(data->Buf, static_cast<std::size_t>(data->BufTextLen)));
            bool changed = false;
            if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
                changed = ctx->model->CompleteInput(*ctx->registry, ctx->ctx);
            else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
                changed = data->EventKey == ImGuiKey_UpArrow ? ctx->model->HistoryPrev() : ctx->model->HistoryNext();
            if (changed)
            {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, ctx->model->Input().c_str());
            }
            return 0;
        }
    }

    bool DrawConsoleInputLine(const char* id, ConsoleModel& model, CVarRegistry& registry, CVarContext ctx)
    {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "%s", model.Input().c_str());
        LineContext lineCtx{ &model, &registry, ctx };
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_CallbackCompletion |
                                          ImGuiInputTextFlags_CallbackHistory;
        ImGui::SetNextItemWidth(-1.0f);
        const bool entered = ImGui::InputTextWithHint(id, "cvar or command -- Tab completes, Up/Down history",
                                                      buffer, sizeof(buffer), flags, &ConsoleLineCallback, &lineCtx);
        model.SetInput(buffer);
        if (!entered)
            return false;
        model.Submit(registry, ctx);
        ImGui::SetKeyboardFocusHere(-1);   // keep typing: re-focus the line just submitted
        return true;
    }
}
