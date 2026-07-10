#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Json.hpp"
#include <Arcane/Util/Logger.hpp>
#include <Arcane/Net/TcpSocket.hpp>

namespace Arcane
{
    // ============================================================================
    // Message ID type
    // ============================================================================
    // IDs are loaded at runtime from data/protocol.json.
    // Use ProtocolLoader::Instance().Id("MessageName") to obtain an ID;
    // store it as a member variable rather than looking it up on every call.

    using MsgId = uint16_t;
    inline constexpr MsgId kInvalidMsgId = 0;  // sentinel -- never sent on the wire

    // ============================================================================
    // JSON Type Alias
    // ============================================================================

    using Json = nlohmann::json;

    // ============================================================================
    // JSON Parse Helper (with error handling)
    // ============================================================================

    // Lenient parse -- returns an empty object on malformed input. Use only
    // when downstream validation will catch every dangerous field (e.g.
    // Auth's username/password, read-only handlers that produce empty
    // responses). For mutating handlers that spend currency, edit party,
    // or otherwise change state, prefer ParseJsonStrict so a malformed
    // payload is rejected at the boundary instead of falling through to
    // defaults -- see audit M11 (2026-06-02).
    inline Json ParseJsonSafe(const std::string& str)
    {
        try
        {
            return Json::parse(str);
        }
        catch (const Json::parse_error&)
        {
            return Json::object();
        }
    }

    // Strict parse -- returns std::nullopt on malformed input. Mutating
    // handlers (SetParty, AddCurrency, leveling, pulls, claim) must use
    // this and reject with an error response on nullopt. The audit M11
    // example: AddCurrency with malformed payload previously fell through
    // to `amount=0` which the clamp turned into `amount=1`, silently
    // granting one unit instead of failing loud.
    inline std::optional<Json> ParseJsonStrict(const std::string& str)
    {
        try
        {
            return Json::parse(str);
        }
        catch (const Json::parse_error&)
        {
            return std::nullopt;
        }
    }

    // ============================================================================
    // Protocol Loader
    // ============================================================================
    // Loads protocol.json at startup. All message IDs, auth requirements, and
    // settings come from here -- no compiled enum to keep in sync.

    class ProtocolLoader
    {
    public:
        struct MessageDef
        {
            int id = 0;
            std::string name;
            std::string direction;
            std::string description;
            bool requiresAuth = false;
            bool debugOnly = false;
        };

        struct Settings
        {
            int defaultPort = 0;
            int maxMessageSize = 0;
            std::string messageFormat;
            int tokenLength = 0;
            int sessionLifetimeSeconds = 0;
            int idleTimeoutSeconds = 0;
            int heartbeatIntervalSeconds = 0;
            // Audit M-V5-6 networking (2026-06-04): public-facing TCP
            // connection caps, formerly compile-time constants in
            // TcpSocket.hpp::ServerConfig. Configurable so a launch-day
            // topology change (CGNAT presence, reverse-proxy, etc.)
            // doesn't require a rebuild. Missing keys in protocol.json
            // fall back to the ServerConfig defaults at load time, so
            // pre-existing protocol.json files keep working unchanged.
            int maxConnectionsPerIp = 0;
            int maxConnectionsTotal = 0;
        };

        static ProtocolLoader& Instance()
        {
            static ProtocolLoader instance;
            return instance;
        }

        bool Load(const std::string& path = "data/protocol.json")
        {
            std::ifstream file(path);
            if (!file.is_open())
            {
                LOG_CORE_ERROR("Failed to open: {}", path);
                return false;
            }

            try
            {
                Json j = Json::parse(file);
                m_version = j.value("version", 1);
                m_name = j.value("name", "Unknown Protocol");

                if (!j.contains("settings") || !j["settings"].is_object())
                {
                    LOG_CORE_ERROR("Missing required 'settings' section");
                    return false;
                }

                const auto& settings = j["settings"];

                auto requireSetting = [&](const char* name) -> bool {
                    if (!settings.contains(name))
                    {
                        LOG_CORE_ERROR("Missing required setting: {}", name);
                        return false;
                    }
                    return true;
                };

                if (!requireSetting("default_port") ||
                    !requireSetting("max_message_size") ||
                    !requireSetting("message_format") ||
                    !requireSetting("token_length") ||
                    !requireSetting("session_lifetime_seconds") ||
                    !requireSetting("idle_timeout_seconds") ||
                    !requireSetting("heartbeat_interval_seconds"))
                {
                    return false;
                }

                m_settings.defaultPort = settings["default_port"].get<int>();
                m_settings.maxMessageSize = settings["max_message_size"].get<int>();
                m_settings.messageFormat = settings["message_format"].get<std::string>();
                m_settings.tokenLength = settings["token_length"].get<int>();
                m_settings.sessionLifetimeSeconds = settings["session_lifetime_seconds"].get<int>();
                m_settings.idleTimeoutSeconds = settings["idle_timeout_seconds"].get<int>();
                m_settings.heartbeatIntervalSeconds = settings["heartbeat_interval_seconds"].get<int>();

                // Audit M-V5-6 networking (2026-06-04): optional keys.
                // Default to the compile-time ServerConfig values when
                // absent so existing protocol.json files keep working
                // without modification. Casting through int is safe --
                // the ServerConfig constants are small (16, 2048).
                m_settings.maxConnectionsPerIp = settings.value(
                    "max_connections_per_ip",
                    static_cast<int>(ServerConfig::MAX_CONNECTIONS_PER_IP));
                m_settings.maxConnectionsTotal = settings.value(
                    "max_connections_total",
                    static_cast<int>(ServerConfig::MAX_CONNECTIONS_TOTAL));

                // E01-3c / reload semantics: Load REPLACES the protocol
                // definition. Clear prior message/id/enum state so a reload
                // does not merge into stale entries -- the loader is a process
                // singleton that persists across reloads (and across tests).
                m_messages.clear();
                m_idToName.clear();
                m_enums.clear();

                if (j.contains("messages") && j["messages"].is_object())
                {
                    for (auto& [name, msgJson] : j["messages"].items())
                    {
                        MessageDef msg;
                        msg.name = name;
                        msg.id = msgJson.value("id", 0);
                        msg.direction = msgJson.value("direction", "");
                        msg.description = msgJson.value("description", "");
                        msg.requiresAuth = msgJson.value("requires_auth", false);
                        msg.debugOnly = msgJson.value("debug_only", false);

                        // E01-3c: validate id BEFORE it is truncated into the
                        // uint16 MsgId. Valid range is [1, 65535]: id 0 (also the
                        // default for a missing "id") collides with kInvalidMsgId
                        // which is never sent on the wire, and any id > 65535 would
                        // silently wrap. A duplicate id would overwrite m_idToName
                        // and misroute GetMessageName / auth lookups. All three are
                        // authoring errors -- reject the whole load (return false,
                        // matching the missing-settings signal) so startup fails
                        // loud rather than misrouting messages later.
                        if (msg.id <= 0 || msg.id > 65535)
                        {
                            LOG_CORE_ERROR(
                                "Message '{}' has out-of-range id {} (valid range is 1..65535)",
                                name, msg.id);
                            return false;
                        }
                        auto dup = m_idToName.find(msg.id);
                        if (dup != m_idToName.end())
                        {
                            LOG_CORE_ERROR(
                                "Duplicate message id {} for '{}' (already used by '{}')",
                                msg.id, name, dup->second);
                            return false;
                        }

                        m_messages[name] = msg;
                        m_idToName[msg.id] = name;
                    }
                }

                if (j.contains("enums") && j["enums"].is_object())
                {
                    for (auto& [enumName, values] : j["enums"].items())
                    {
                        std::unordered_map<std::string, int> enumMap;
                        for (auto& [key, value] : values.items())
                        {
                            if (value.is_number_integer())
                                enumMap[key] = value.get<int>();
                        }
                        m_enums[enumName] = enumMap;
                    }
                }

                LOG_CORE_INFO("Loaded v{} with {} messages", m_version, m_messages.size());
                m_loaded = true;
                return true;
            }
            catch (const Json::parse_error& e)
            {
                LOG_CORE_ERROR("JSON parse error: {}", e.what());
                return false;
            }
            catch (const std::exception& e)
            {
                LOG_CORE_ERROR("Error: {}", e.what());
                return false;
            }
        }

        // Look up a message ID by name. Returns kInvalidMsgId (0) if not found.
        MsgId Id(const std::string& name) const
        {
            int id = GetMessageId(name);
            return (id > 0) ? static_cast<MsgId>(id) : kInvalidMsgId;
        }

        // Audit M-V5-4 cross-cutting (2026-06-03): handler startup paths
        // cache message IDs via `Id("MessageName")`. A typo or a renamed
        // entry in protocol.json silently returns kInvalidMsgId, so the
        // handler ends up registered against id 0 and IsRegistered(type)
        // / Dispatch checks miss it. This helper resolves a name and
        // throws on miss -- handler ctors can call it during startup so
        // the failure surfaces at boot instead of silently dropping
        // messages later. Wrap in a try at the call site if you want a
        // fatal log line instead of an exception escape.
        MsgId IdOrThrow(const std::string& name) const
        {
            const MsgId id = Id(name);
            if (id == kInvalidMsgId)
                throw std::logic_error("ProtocolLoader::IdOrThrow: unknown message name '" + name +
                                       "' -- protocol.json drift or typo");
            return id;
        }

        int GetMessageId(const std::string& name) const
        {
            auto it = m_messages.find(name);
            if (it != m_messages.end())
                return it->second.id;
            return -1;
        }

        std::string GetMessageName(int id) const
        {
            auto it = m_idToName.find(id);
            if (it != m_idToName.end())
                return it->second;
            return "";
        }

        const MessageDef* GetMessage(const std::string& name) const
        {
            auto it = m_messages.find(name);
            if (it != m_messages.end())
                return &it->second;
            return nullptr;
        }

        int GetEnumValue(const std::string& enumName, const std::string& key) const
        {
            auto enumIt = m_enums.find(enumName);
            if (enumIt != m_enums.end())
            {
                auto valueIt = enumIt->second.find(key);
                if (valueIt != enumIt->second.end())
                    return valueIt->second;
            }
            return -1;
        }

        bool IsLoaded() const { return m_loaded; }
        int GetVersion() const { return m_version; }
        const std::string& GetName() const { return m_name; }
        const Settings& GetSettings() const { return m_settings; }
        const std::unordered_map<std::string, MessageDef>& GetAllMessages() const { return m_messages; }

    private:
        ProtocolLoader() = default;
        ProtocolLoader(const ProtocolLoader&) = delete;
        ProtocolLoader& operator=(const ProtocolLoader&) = delete;

        bool m_loaded = false;
        int m_version = 0;
        std::string m_name;
        Settings m_settings;
        std::unordered_map<std::string, MessageDef> m_messages;
        std::unordered_map<int, std::string> m_idToName;
        std::unordered_map<std::string, std::unordered_map<std::string, int>> m_enums;
    };

    // ============================================================================
    // Network Message
    // ============================================================================

    struct Message
    {
        MsgId       type    = kInvalidMsgId;
        std::string token;    // session token (empty for unauthenticated requests)
        std::string payload;

        // Format: LENGTH:TYPE|TOKEN|PAYLOAD\n
        std::string Serialize() const
        {
            std::string body = std::to_string(static_cast<int>(type)) + "|" + token + "|" + payload;
            return std::to_string(body.length()) + ":" + body + "\n";
        }

        // Parse the body portion ("TYPE|TOKEN|PAYLOAD") after the length prefix is stripped.
        static Message ParseBody(const std::string& body)
        {
            Message msg;

            size_t firstPipe = body.find('|');
            if (firstPipe == std::string::npos)
                return msg;

            size_t secondPipe = body.find('|', firstPipe + 1);

            try
            {
                msg.type = static_cast<MsgId>(std::stoi(body.substr(0, firstPipe)));

                if (secondPipe != std::string::npos)
                {
                    msg.token   = body.substr(firstPipe + 1, secondPipe - firstPipe - 1);
                    msg.payload = body.substr(secondPipe + 1);
                }
                else
                {
                    msg.token   = "";
                    msg.payload = body.substr(firstPipe + 1);
                }
            }
            catch (const std::exception&)
            {
                msg.type = kInvalidMsgId;
            }

            return msg;
        }

        std::string ToString() const
        {
            std::string name = ProtocolLoader::Instance().GetMessageName(static_cast<int>(type));
            if (name.empty()) name = "Unknown";
            std::string tokenPreview = token.empty() ? "(no token)" :
                (token.length() < 12 ? token
                                     : token.substr(0, 8) + "..." + token.substr(token.length() - 4));
            return name + "(" + std::to_string(static_cast<int>(type)) + ") token=" + tokenPreview + ": " +
                payload.substr(0, 100) + (payload.length() > 100 ? "..." : "");
        }

        bool HasToken() const
        {
            return !token.empty() && token.length() >= 64;
        }
    };

    // ============================================================================
    // Helper predicates
    // ============================================================================

    inline bool IsRequestMessage(MsgId id)  { return id > 0 && id < 100; }
    inline bool IsResponseMessage(MsgId id) { return id >= 100; }

    // Returns true if this message type requires an authenticated session.
    // Reads requires_auth from protocol.json; defaults to true for unknown IDs.
    inline bool RequiresAuthentication(MsgId id)
    {
        if (id == kInvalidMsgId) return false;
        auto& proto = ProtocolLoader::Instance();
        const std::string& name = proto.GetMessageName(static_cast<int>(id));
        if (name.empty()) return true;
        const auto* def = proto.GetMessage(name);
        return def ? def->requiresAuth : true;
    }

} // namespace Arcane
