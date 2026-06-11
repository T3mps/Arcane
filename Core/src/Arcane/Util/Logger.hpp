#pragma once

// Arcane logging system
// Unified logging built on spdlog with multiple named loggers,
// console + file output, and structured JSON event logging.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

// Audit M-V3-1 security (2026-06-03): LogSessionEvent hashes the token
// rather than logging its prefix. picosha2 is header-only and already
// vendored under ThirdParty; Crypto.hpp uses it but includes Logger.hpp,
// so we depend on picosha2 directly here to avoid a circular include.
#include "picosha2.hpp"

// spdlog configuration - must be before spdlog includes
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/fmt/fmt.h>

namespace Arcane
{
    // ============================================================================
    // Log Level Enum (aliases spdlog levels)
    // ============================================================================

    enum class Level
    {
        Trace    = spdlog::level::trace,     // Finest-grained information
        Debug    = spdlog::level::debug,     // Debug information
        Info     = spdlog::level::info,      // General information
        Warn     = spdlog::level::warn,      // Warning conditions
        Error    = spdlog::level::err,       // Error conditions
        Critical = spdlog::level::critical,  // Critical failures
        Off      = spdlog::level::off        // Disable logging
    };

    // ============================================================================
    // Logger Categories
    // ============================================================================

    enum class LogCategory
    {
        Server,      // Core server operations (startup, shutdown, config)
        Net,         // Network operations (connections, sockets, send/recv)
        Auth,        // Authentication (login, logout, registration, sessions)
        Gacha,       // Gacha mechanics (pulls, pity, 50/50, guarantees)
        Data,        // Data persistence (accounts, items, banners, config files)
        Protocol     // Protocol parsing and validation
    };

    // ============================================================================
    // Logger Class
    // ============================================================================

    class Logger
    {
    public:
        // Initialize the logging system - call once at startup
        static void Init(Level consoleLevel = Level::Info, Level fileLevel = Level::Debug)
        {
            if (s_initialized)
                return;

            try
            {
                // Ensure logs directory exists
                std::filesystem::create_directories("logs");

                // Create sinks
                auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                consoleSink->set_level(static_cast<spdlog::level::level_enum>(consoleLevel));
                consoleSink->set_pattern("%^[%H:%M:%S.%e] [%n] [%l]%$ %v");

                auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    "logs/gacha_server.log",
                    5 * 1024 * 1024,  // 5 MB max file size
                    3                  // Keep 3 rotated files
                );
                fileSink->set_level(static_cast<spdlog::level::level_enum>(fileLevel));
                fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");

                std::vector<spdlog::sink_ptr> sinks = { consoleSink, fileSink };

                // Create named loggers
                CreateLogger("Server", sinks);
                CreateLogger("Net", sinks);
                CreateLogger("Auth", sinks);
                CreateLogger("Gacha", sinks);
                CreateLogger("Data", sinks);
                CreateLogger("Protocol", sinks);

                // Set default logger
                spdlog::set_default_logger(s_loggers[LogCategory::Server]);

                // Set global level to trace (individual sinks control filtering)
                spdlog::set_level(spdlog::level::trace);

                // Flush on info or higher (ensures JSON events are written immediately)
                spdlog::flush_on(spdlog::level::info);

                s_initialized = true;
            }
            catch (const spdlog::spdlog_ex& ex)
            {
                std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
            }
        }

        // Shutdown the logging system - call at exit
        static void Shutdown()
        {
            spdlog::shutdown();
            s_loggers.clear();
            s_initialized = false;
        }

        // Get a logger by category. Falls back to the default spdlog logger if the
        // requested category is missing (e.g., Init() not yet called), so macros
        // degrade gracefully rather than null-dereferencing.
        static std::shared_ptr<spdlog::logger> Get(LogCategory category)
        {
            auto it = s_loggers.find(category);
            if (it != s_loggers.end() && it->second)
                return it->second;
            return spdlog::default_logger();
        }

        // Set console log level at runtime
        static void SetConsoleLevel(Level level)
        {
            for (auto& [cat, logger] : s_loggers)
            {
                if (!logger->sinks().empty())
                {
                    logger->sinks()[0]->set_level(static_cast<spdlog::level::level_enum>(level));
                }
            }
        }

        // Set file log level at runtime
        static void SetFileLevel(Level level)
        {
            for (auto& [cat, logger] : s_loggers)
            {
                if (logger->sinks().size() > 1)
                {
                    logger->sinks()[1]->set_level(static_cast<spdlog::level::level_enum>(level));
                }
            }
        }

        // Set level for a specific category
        static void SetCategoryLevel(LogCategory category, Level level)
        {
            if (s_loggers.contains(category))
            {
                s_loggers[category]->set_level(static_cast<spdlog::level::level_enum>(level));
            }
        }

        // Check if initialized
        static bool IsInitialized() { return s_initialized; }

        // ========================================================================
        // Structured JSON Event Logging (for analytics)
        // ========================================================================

        // Log a gacha pull event as JSON
        static void LogPullEvent(
            const std::string& playerId,
            const std::string& banner,
            int pullNumber,
            const std::string& itemId,
            const std::string& itemName,
            int rarity,
            bool wasPity,
            bool wasGuarantee,
            bool wasFeatured,
            bool wonFiftyFifty)
        {
            auto logger = Get(LogCategory::Gacha);
            logger->info(
                R"({{"event":"pull","player":"{}","banner":"{}","pull_num":{},"item_id":"{}","item_name":"{}","rarity":{},"pity":{},"guarantee":{},"featured":{},"won_5050":{}}})",
                playerId, banner, pullNumber, itemId, itemName, rarity,
                wasPity, wasGuarantee, wasFeatured, wonFiftyFifty
            );
        }

        // Log an authentication event as JSON
        static void LogAuthEvent(
            const std::string& eventType,  // "login", "logout", "register"
            const std::string& playerId,
            const std::string& ip,
            bool success,
            const std::string& reason = "")
        {
            auto logger = Get(LogCategory::Auth);
            if (reason.empty())
            {
                logger->info(
                    R"({{"event":"{}","player":"{}","ip":"{}","success":{}}})",
                    eventType, playerId, ip, success
                );
            }
            else
            {
                logger->info(
                    R"({{"event":"{}","player":"{}","ip":"{}","success":{},"reason":"{}"}})",
                    eventType, playerId, ip, success, reason
                );
            }
        }

        // Log a session event as JSON.
        //
        // Audit M-V3-1 security (2026-06-03): the third argument used to be
        // the first 8 hex chars of the raw token (= 32 bits of token
        // entropy leaked per RPC validation). Replaced with the first 8
        // hex chars of SHA-256(token) so the correlation property is
        // preserved (same token -> same log marker, distinguishing two
        // sessions of the same player) but no token bytes are exposed.
        // SHA-256 preimage resistance means even the full set of log
        // entries leaks nothing about the underlying token.
        // Audit M-V4-2 security (2026-06-03): optional `reason` so the
        // invalidate path can distinguish idle / expired / invalid /
        // forced-logout in the structured stream. Empty reason emits no
        // "reason" field — backwards compatible with existing callers.
        static void LogSessionEvent(
            const std::string& eventType,  // "created", "expired", "validated", "invalidated"
            const std::string& playerId,
            const std::string& token = "",
            const std::string& reason = "")
        {
            std::string tokenHashPrefix;
            if (!token.empty())
            {
                std::vector<unsigned char> digest(picosha2::k_digest_size);
                picosha2::hash256(token.begin(), token.end(), digest.begin(), digest.end());
                tokenHashPrefix = picosha2::bytes_to_hex_string(digest).substr(0, 8);
            }
            auto logger = Get(LogCategory::Auth);
            if (reason.empty())
            {
                logger->debug(
                    R"({{"event":"session_{}","player":"{}","token_hash":"{}"}})",
                    eventType, playerId, tokenHashPrefix
                );
            }
            else
            {
                logger->debug(
                    R"({{"event":"session_{}","player":"{}","token_hash":"{}","reason":"{}"}})",
                    eventType, playerId, tokenHashPrefix, reason
                );
            }
        }

        // Log a connection event as JSON
        static void LogConnectionEvent(
            const std::string& eventType,  // "connected", "disconnected"
            const std::string& ip,
            const std::string& reason = "")
        {
            auto logger = Get(LogCategory::Net);
            if (reason.empty())
            {
                logger->info(R"({{"event":"{}","ip":"{}"}})", eventType, ip);
            }
            else
            {
                logger->info(R"({{"event":"{}","ip":"{}","reason":"{}"}})", eventType, ip, reason);
            }
        }

    private:
        static void CreateLogger(const std::string& name, const std::vector<spdlog::sink_ptr>& sinks)
        {
            auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
            logger->set_level(spdlog::level::trace);
            spdlog::register_logger(logger);

            // Map name to category
            if (name == "Server")      s_loggers[LogCategory::Server] = logger;
            else if (name == "Net")    s_loggers[LogCategory::Net] = logger;
            else if (name == "Auth")   s_loggers[LogCategory::Auth] = logger;
            else if (name == "Gacha")  s_loggers[LogCategory::Gacha] = logger;
            else if (name == "Data")   s_loggers[LogCategory::Data] = logger;
            else if (name == "Protocol") s_loggers[LogCategory::Protocol] = logger;
        }

        static inline bool s_initialized = false;
        static inline std::unordered_map<LogCategory, std::shared_ptr<spdlog::logger>> s_loggers;
    };

    // ============================================================================
    // Convenience Macros
    // ============================================================================

    // Server logger
    #define LOG_SERVER_TRACE(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Server)->trace(__VA_ARGS__)
    #define LOG_SERVER_DEBUG(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Server)->debug(__VA_ARGS__)
    #define LOG_SERVER_INFO(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Server)->info(__VA_ARGS__)
    #define LOG_SERVER_WARN(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Server)->warn(__VA_ARGS__)
    #define LOG_SERVER_ERROR(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Server)->error(__VA_ARGS__)
    #define LOG_SERVER_CRITICAL(...) ::Arcane::Logger::Get(::Arcane::LogCategory::Server)->critical(__VA_ARGS__)

    // Network logger
    #define LOG_NET_TRACE(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Net)->trace(__VA_ARGS__)
    #define LOG_NET_DEBUG(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Net)->debug(__VA_ARGS__)
    #define LOG_NET_INFO(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Net)->info(__VA_ARGS__)
    #define LOG_NET_WARN(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Net)->warn(__VA_ARGS__)
    #define LOG_NET_ERROR(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Net)->error(__VA_ARGS__)
    #define LOG_NET_CRITICAL(...) ::Arcane::Logger::Get(::Arcane::LogCategory::Net)->critical(__VA_ARGS__)

    // Auth logger
    #define LOG_AUTH_TRACE(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Auth)->trace(__VA_ARGS__)
    #define LOG_AUTH_DEBUG(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Auth)->debug(__VA_ARGS__)
    #define LOG_AUTH_INFO(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Auth)->info(__VA_ARGS__)
    #define LOG_AUTH_WARN(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Auth)->warn(__VA_ARGS__)
    #define LOG_AUTH_ERROR(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Auth)->error(__VA_ARGS__)
    #define LOG_AUTH_CRITICAL(...) ::Arcane::Logger::Get(::Arcane::LogCategory::Auth)->critical(__VA_ARGS__)

    // Gacha logger
    #define LOG_GACHA_TRACE(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Gacha)->trace(__VA_ARGS__)
    #define LOG_GACHA_DEBUG(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Gacha)->debug(__VA_ARGS__)
    #define LOG_GACHA_INFO(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Gacha)->info(__VA_ARGS__)
    #define LOG_GACHA_WARN(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Gacha)->warn(__VA_ARGS__)
    #define LOG_GACHA_ERROR(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Gacha)->error(__VA_ARGS__)
    #define LOG_GACHA_CRITICAL(...) ::Arcane::Logger::Get(::Arcane::LogCategory::Gacha)->critical(__VA_ARGS__)

    // Data logger
    #define LOG_DATA_TRACE(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Data)->trace(__VA_ARGS__)
    #define LOG_DATA_DEBUG(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Data)->debug(__VA_ARGS__)
    #define LOG_DATA_INFO(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Data)->info(__VA_ARGS__)
    #define LOG_DATA_WARN(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Data)->warn(__VA_ARGS__)
    #define LOG_DATA_ERROR(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Data)->error(__VA_ARGS__)
    #define LOG_DATA_CRITICAL(...) ::Arcane::Logger::Get(::Arcane::LogCategory::Data)->critical(__VA_ARGS__)

    // Protocol logger
    #define LOG_PROTOCOL_TRACE(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Protocol)->trace(__VA_ARGS__)
    #define LOG_PROTOCOL_DEBUG(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Protocol)->debug(__VA_ARGS__)
    #define LOG_PROTOCOL_INFO(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Protocol)->info(__VA_ARGS__)
    #define LOG_PROTOCOL_WARN(...)     ::Arcane::Logger::Get(::Arcane::LogCategory::Protocol)->warn(__VA_ARGS__)
    #define LOG_PROTOCOL_ERROR(...)    ::Arcane::Logger::Get(::Arcane::LogCategory::Protocol)->error(__VA_ARGS__)
    #define LOG_PROTOCOL_CRITICAL(...) ::Arcane::Logger::Get(::Arcane::LogCategory::Protocol)->critical(__VA_ARGS__)

} // namespace Arcane
