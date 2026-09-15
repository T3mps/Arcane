#include <Arcane/Base/DiagEnvelope.hpp>

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <fstream>
#include <iterator>

namespace Arcane::Diag
{
    namespace
    {
        // is_string gates (not .value): a hand-edited/future-format field of
        // the wrong JSON type must fall back to empty, never throw a
        // type_error out of Parse -- same rule LoadMaterialAsset follows.
        std::string StrField(const nlohmann::json& doc, const char* key)
        {
            return (doc.contains(key) && doc[key].is_string())
                       ? doc[key].get<std::string>()
                       : std::string{};
        }

        nlohmann::json QueueToJson(const Envelope::Queue& q)
        {
            nlohmann::json j;
            j["name"] = q.name;
            j["lastCompleted"] = q.lastCompleted;
            j["inFlight"] = q.inFlight;
            return j;
        }

        Envelope::Queue QueueFromJson(const nlohmann::json& j)
        {
            Envelope::Queue q;
            q.name = StrField(j, "name");
            q.lastCompleted = StrField(j, "lastCompleted");
            if (j.contains("inFlight") && j["inFlight"].is_array())
                for (const nlohmann::json& s : j["inFlight"])
                    if (s.is_string())
                        q.inFlight.push_back(s.get<std::string>());
            return q;
        }
    }

    std::string Serialize(const Envelope& envelope)
    {
        nlohmann::json doc;
        doc["formatVersion"] = envelope.formatVersion;
        doc["guid"] = envelope.guid.ToString();

        doc["kind"] = envelope.kind;
        doc["timestampUtc"] = envelope.timestampUtc;
        doc["appName"] = envelope.appName;
        doc["phase"] = envelope.phase;
        doc["buildInfo"] = envelope.buildInfo;

        doc["cpuThreadSummary"] = envelope.cpuThreadSummary;

        nlohmann::json queues = nlohmann::json::array();
        for (const Envelope::Queue& q : envelope.queues)
            queues.push_back(QueueToJson(q));
        doc["queues"] = std::move(queues);

        doc["fault"] = nlohmann::json{
            { "type", envelope.fault.type },
            { "address", envelope.fault.address },
            { "resource", envelope.fault.resource } };

        doc["siblingTxt"] = envelope.siblingTxt;
        doc["siblingDmp"] = envelope.siblingDmp;
        doc["siblingGpuDump"] = envelope.siblingGpuDump;

        doc["activeLayers"] = envelope.activeLayers;

        // error_handler_t::replace: a snippet-adjacent field carrying
        // invalid UTF-8 must degrade to U+FFFD, never throw out of
        // Serialize (same rule SaveMaterialAsset follows).
        return doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    std::optional<Envelope> Parse(std::string_view json)
    {
        const auto doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object())
            return std::nullopt;

        // formatVersion: required, must be the exact supported version. Not
        // present, wrong-typed, or any other value all reject -- there is no
        // format 2 to migrate from yet.
        if (!doc.contains("formatVersion") || !doc["formatVersion"].is_number_unsigned())
            return std::nullopt;
        if (doc["formatVersion"].get<std::uint32_t>() != kFormatVersion)
            return std::nullopt;

        // guid: required, must parse, must not be nil. A nil guid is
        // indistinguishable from "no identity was ever minted" and Task 9's
        // AssetRegistry treats a nil id as absent, so refusing it here (not
        // downstream) keeps every consumer of Parse() honest about identity.
        if (!doc.contains("guid") || !doc["guid"].is_string())
            return std::nullopt;
        const auto guid = Guid::FromString(doc["guid"].get<std::string>());
        if (!guid || !guid->IsValid())
            return std::nullopt;

        Envelope e;
        e.formatVersion = kFormatVersion;
        e.guid = *guid;

        e.kind = StrField(doc, "kind");
        e.timestampUtc = StrField(doc, "timestampUtc");
        e.appName = StrField(doc, "appName");
        e.phase = StrField(doc, "phase");
        e.buildInfo = StrField(doc, "buildInfo");

        e.cpuThreadSummary = StrField(doc, "cpuThreadSummary");

        if (doc.contains("queues") && doc["queues"].is_array())
            for (const nlohmann::json& qj : doc["queues"])
                if (qj.is_object())
                    e.queues.push_back(QueueFromJson(qj));

        if (doc.contains("fault") && doc["fault"].is_object())
        {
            const nlohmann::json& fj = doc["fault"];
            e.fault.type = StrField(fj, "type");
            e.fault.address = StrField(fj, "address");
            e.fault.resource = StrField(fj, "resource");
        }

        e.siblingTxt = StrField(doc, "siblingTxt");
        e.siblingDmp = StrField(doc, "siblingDmp");
        e.siblingGpuDump = StrField(doc, "siblingGpuDump");

        if (doc.contains("activeLayers") && doc["activeLayers"].is_array())
            for (const nlohmann::json& s : doc["activeLayers"])
                if (s.is_string())
                    e.activeLayers.push_back(s.get<std::string>());

        return e;
    }

    bool WriteFile(const Envelope& envelope, const std::filesystem::path& path)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            ARC_WARN("Diag::WriteFile: cannot write '{}'", path.generic_string());
            return false;
        }
        // Binary-mode ofstream + dump()'s raw UTF-8 output: no BOM is ever
        // written, matching SaveMaterialAsset's convention.
        out << Serialize(envelope) << '\n';
        return out.good();
    }

    std::optional<Envelope> ReadFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            ARC_WARN("Diag::ReadFile: cannot read '{}'", path.generic_string());
            return std::nullopt;
        }
        const std::string content((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        return Parse(content);
    }
}
