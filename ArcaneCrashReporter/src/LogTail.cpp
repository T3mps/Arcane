#include "LogTail.hpp"
#include "FileText.hpp"
#include "ReportView.hpp"

namespace Arcane::Reporter
{
    std::filesystem::path ResolveLogPath(const std::filesystem::path& stem, const std::filesystem::path& livePath)
    {
        // <stem>.log.txt: path::operator+= appends to the last component with
        // no separator inserted, which is what turns ".../foo" into
        // ".../foo.log.txt" rather than ".../foo/.log.txt".
        std::filesystem::path folderCopy = stem;
        folderCopy += ".log.txt";

        std::error_code ec;
        if (std::filesystem::is_regular_file(folderCopy, ec))
            return folderCopy;

        ec.clear();
        if (!livePath.empty() && std::filesystem::is_regular_file(livePath, ec))
            return livePath;

        return {};
    }

    std::string ReadLogTail(const std::filesystem::path& stem, const std::filesystem::path& livePath, std::size_t lines)
    {
        const std::filesystem::path log = ResolveLogPath(stem, livePath);
        return log.empty() ? std::string{} : LastLines(Slurp(log), lines);
    }
}
