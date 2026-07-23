#include <Arcane/Project/AssetRegistry.hpp>

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <fstream>
#include <system_error>

namespace Arcane
{
    std::size_t AssetRegistry::ScanContent(const std::filesystem::path& contentDir, std::string_view scheme)
    {
        m_byGuid.clear();

        std::error_code ec;
        if (!std::filesystem::is_directory(contentDir, ec))
            return 0;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;

            // Read + parse (no exceptions).
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) { ARC_WARN("AssetRegistry: cannot read '{}'", entry.path().generic_string()); continue; }
            auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
            in.close();
            if (!doc.is_object())
            {
                ARC_WARN("AssetRegistry: '{}' is not a JSON object -- skipped", entry.path().generic_string());
                continue;
            }

            // Read or assign the stable id. A missing/invalid id is generated and written
            // back (auto-import), so the asset is born with a durable identity.
            Guid id;
            bool write = false;
            if (auto it = doc.find("id"); it != doc.end() && it->is_string())
            {
                if (auto parsed = Guid::FromString(it->get<std::string>()); parsed && parsed->IsValid())
                    id = *parsed;
            }
            if (!id.IsValid())
            {
                id = Guid::Generate();
                doc["id"] = id.ToString();
                write = true;
            }
            if (write)
            {
                std::ofstream out(entry.path(), std::ios::binary);
                if (out) out << doc.dump(2) << '\n';
                else ARC_WARN("AssetRegistry: cannot write id back to '{}'", entry.path().generic_string());
            }

            // Mount path: "<scheme>://<relative-to-contentDir, forward slashes>".
            const auto rel = std::filesystem::relative(entry.path(), contentDir, ec);
            std::string mountPath = std::string(scheme) + "://" + rel.generic_string();

            if (auto [_, inserted] = m_byGuid.try_emplace(id, std::move(mountPath)); !inserted)
                ARC_WARN("AssetRegistry: duplicate id {} (also '{}') -- keeping first",
                         id.ToString(), entry.path().generic_string());
        }

        return m_byGuid.size();
    }

    std::optional<std::string> AssetRegistry::Resolve(const Guid& id) const
    {
        auto it = m_byGuid.find(id);
        if (it == m_byGuid.end())
            return std::nullopt;
        return it->second;
    }
}
