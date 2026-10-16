#pragma once

// The suite's D3D12/Vulkan validation gate, split out of the Catch2 listener
// so a test can feed it counts directly.
//
// On this desk ID3D12InfoQueue1 does not register, so a stored D3D12 message
// reaches RenderErrorCount only inside ~NriDevice. A test that reads the
// latch while its device is still alive observes the old count. The listener
// records the count at testCaseStarting and compares it at testCaseEnded,
// which runs after that case's locals -- and so its device -- are destroyed.
// A rise in a case that is not tagged [expects-render-errors] is remembered.
// Catch2 listeners cannot fail a test, so main prints Summary() after
// session.run and returns ExitStatus() when it is non-zero.
//
// testRunEnded samples once more. A device kept in a static or shared
// fixture outlives the case; the run checks the latch after the teardown
// this process can still run. A rise between two cases is recorded at the
// next BeginCase, under the gap's name, so it is not charged to the later test.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Test
{
    class RenderErrorGate
    {
    public:
        struct Offender
        {
            std::string   name;
            std::uint64_t before = 0;
            std::uint64_t after  = 0;
        };

        // EndRun uses this name. It is not a test case; it is the sample
        // after the last case, for a device released with the run.
        static constexpr const char* kAfterLastCase = "(after the last test case)";

        void BeginRun(std::uint64_t count) { m_accounted = count; }

        void BeginCase(std::string name, bool expectsErrors, std::uint64_t count)
        {
            if (count > m_accounted)
            {
                Note(std::string("(between test cases, before '") + name + "')",
                     m_accounted, count);
            }
            m_inCase    = true;
            m_expects   = expectsErrors;
            m_caseName  = std::move(name);
            m_caseStart = count;
            m_accounted = count;
        }

        void EndCase(std::uint64_t count)
        {
            if (m_inCase && !m_expects && count > m_caseStart)
                Note(m_caseName, m_caseStart, count);
            m_accounted = count;
            m_inCase    = false;
        }

        void EndRun(std::uint64_t count)
        {
            if (count > m_accounted)
                Note(kAfterLastCase, m_accounted, count);
            m_accounted = count;
        }

        [[nodiscard]] int ExitStatus() const { return m_offenders.empty() ? 0 : 1; }

        [[nodiscard]] const std::vector<Offender>& Offenders() const { return m_offenders; }

        [[nodiscard]] std::string Summary() const
        {
            if (m_offenders.empty())
                return {};

            std::string out;
            out += "ArcaneTests: RenderErrorCount rose where no [expects-render-errors] tag allows it.\n";
            out += "The count is sampled at testCaseEnded, after the test's device is destroyed, and once more at testRunEnded.\n";
            for (const Offender& offender : m_offenders)
            {
                out += "  ";
                out += offender.name;
                out += ": RenderErrorCount ";
                out += std::to_string(offender.before);
                out += " -> ";
                out += std::to_string(offender.after);
                out += " (+";
                out += std::to_string(offender.after - offender.before);
                out += ")\n";
            }
            return out;
        }

    private:
        void Note(std::string name, std::uint64_t before, std::uint64_t after)
        {
            m_offenders.push_back(Offender{std::move(name), before, after});
        }

        std::uint64_t          m_accounted = 0;
        bool                   m_inCase    = false;
        bool                   m_expects   = false;
        std::string            m_caseName;
        std::uint64_t          m_caseStart = 0;
        std::vector<Offender>  m_offenders;
    };
}
