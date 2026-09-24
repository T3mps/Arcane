#pragma once

// The console's state, with no widgets. The editor tab and the runtime overlay
// both drive this. Presentation-free, so it lives in ArcaneCore next to the
// registry (the core split; the spec's "model lives in ArcaneClient" predates that).

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>

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
        [[nodiscard]] std::vector<std::string> Complete(const CVarRegistry& registry) const;

    private:
        std::string m_input;
        std::vector<ConsoleLine> m_lines;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
