#pragma once

#include "Output.hpp"
#include "ProjectLayout.hpp"

#include <string_view>

namespace arcbuild
{
    class IProjectCleaner
    {
    public:
        virtual ~IProjectCleaner() = default;

        [[nodiscard]]
        virtual int Clean(
            const ProjectLayout& project,
            std::string_view config) const = 0;
    };

    class ProjectCleaner final : public IProjectCleaner
    {
    public:
        explicit ProjectCleaner(IOutput& output) : output_(output) {}

        [[nodiscard]] int Clean(const ProjectLayout& project, std::string_view config) const override;

    private:
        IOutput& output_;
    };
}
