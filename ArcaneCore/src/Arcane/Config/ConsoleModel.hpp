#pragma once

// The console's state, with no widgets. The editor tab and the runtime overlay
// both drive this. Presentation-free, so it lives in ArcaneCore next to the
// registry (the core split; the spec's "model lives in ArcaneClient" predates that).

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>

#include <deque>
#include <string>
#include <vector>

namespace Arcane
{
    struct ConsoleLine
    {
        std::string text;
        bool ok = true;
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif
    class ARCANE_CORE_API ConsoleModel
    {
    public:
        void Submit(CVarRegistry& registry, Permission permission);
        void SetInput(std::string text) { m_input = std::move(text); }
        [[nodiscard]] const std::string& Input() const { return m_input; }
        [[nodiscard]] const std::vector<ConsoleLine>& Lines() const { return m_lines; }
        // Prefix matches over List() + ListCommands(), sorted, de-duplicated.
        [[nodiscard]] std::vector<std::string> Complete(const CVarRegistry& registry) const;
        // Tab: one match -> "name "; several -> their longest common prefix and one
        // reply line listing them. Returns whether the input changed.
        bool CompleteInput(const CVarRegistry& registry);
        // Up/Down. The draft is stashed on the first Up and restored past the newest.
        bool HistoryPrev();
        bool HistoryNext();
        [[nodiscard]] const std::deque<std::string>& History() const { return m_history; }

    private:
        std::string m_input;
        std::vector<ConsoleLine> m_lines;
        std::deque<std::string> m_history;
        std::string m_draft;
        int m_historyPos = -1;   // -1 = editing the draft
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
