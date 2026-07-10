#pragma once

// Arcane logging system
// Generic engine logging built on spdlog: lazily-created string-keyed named
// loggers over a shared console + optional rotating-file sink stack, plus the
// JsonEscape kernel consumers use to build structured JSON log events.
// Game/service vocabulary (categories, analytics events, log file names)
// lives with the consumer (e.g. the Aphelyon facade in Server/Common).

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

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
    // Logger Class
    // ============================================================================

    class Logger
    {
    public:
        // Initialize the logging system - call once at startup.
        // An empty logFilePath skips the rotating-file sink (console only).
        static void Init(Level consoleLevel = Level::Info, Level fileLevel = Level::Trace, const std::string& logFilePath = "")
        {
            if (s_initialized)
                return;

            try
            {
                // Create sinks
                auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                consoleSink->set_level(static_cast<spdlog::level::level_enum>(consoleLevel));
                consoleSink->set_pattern("%^[%H:%M:%S.%e] [%n] [%l]%$ %v");

                std::vector<spdlog::sink_ptr> sinks = { consoleSink };

                if (!logFilePath.empty())
                {
                    // Ensure the log directory exists
                    auto logDir = std::filesystem::path(logFilePath).parent_path();
                    if (!logDir.empty())
                        std::filesystem::create_directories(logDir);

                    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                        logFilePath,
                        5 * 1024 * 1024,  // 5 MB max file size
                        3                  // Keep 3 rotated files
                    );
                    fileSink->set_level(static_cast<spdlog::level::level_enum>(fileLevel));
                    fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
                    sinks.push_back(fileSink);
                }

                // Remember the sink stack so lazily-created named loggers
                // (Get(std::string_view) below) share the same outputs. No
                // named logger is created eagerly -- Get is lazy and the
                // category vocabulary belongs to the consumer.
                s_sinks = sinks;

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
            s_sinks.clear();
            s_initialized = false;
        }

        // Generic named-logger access. Creates the logger on first use with the
        // sinks configured at Init (console always; file sink when Init was given
        // a log file path). Category names are the CONSUMER's vocabulary -- the
        // engine core itself logs only under "Core" (LOG_CORE_* below).
        static spdlog::logger* Get(std::string_view name)
        {
            if (!IsInitialized())
                Init();
            std::string key(name);
            if (auto existing = spdlog::get(key))
                return existing.get();
            return CreateLogger(key, s_sinks).get();
        }

        // Set console log level at runtime. All named loggers share the sink
        // objects captured at Init (s_sinks), so setting the shared sink's
        // level covers every logger -- including lazily-created ones.
        static void SetConsoleLevel(Level level)
        {
            if (!s_sinks.empty())
                s_sinks[0]->set_level(static_cast<spdlog::level::level_enum>(level));
        }

        // Set file log level at runtime (no-op when Init had no file path).
        static void SetFileLevel(Level level)
        {
            if (s_sinks.size() > 1)
                s_sinks[1]->set_level(static_cast<spdlog::level::level_enum>(level));
        }

        // Check if initialized
        static bool IsInitialized() { return s_initialized; }

        // E01-4: minimal JSON string-value escaper. Consumers that build
        // structured JSON log events (e.g. the Aphelyon facade's analytics
        // methods in Server/Common) splice content-derived fields into
        // hand-built JSON format strings; a raw '"', '\\' or control
        // character in any of those would break the JSON and permit
        // field/log injection into the analytics stream. Escape
        // exactly the JSON string-value special characters (matching
        // nlohmann/json dump() for ASCII: short forms for the common control
        // chars, \u00XX for the rest, plus '"' and '\\'). Kept hand-rolled so
        // this very widely-included header does not pull in the heavy
        // nlohmann/json header. UTF-8 continuation bytes (>= 0x80) pass through
        // unchanged, which is valid in a UTF-8 JSON document.
        static std::string JsonEscape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 8);
            for (unsigned char c : s)
            {
                switch (c)
                {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20)
                        {
                            static constexpr char kHex[] = "0123456789abcdef";
                            out += "\\u00";
                            out += kHex[(c >> 4) & 0x0F];
                            out += kHex[c & 0x0F];
                        }
                        else
                        {
                            out += static_cast<char>(c);
                        }
                        break;
                }
            }
            return out;
        }

    private:
        static std::shared_ptr<spdlog::logger> CreateLogger(const std::string& name, const std::vector<spdlog::sink_ptr>& sinks)
        {
            auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
            logger->set_level(spdlog::level::trace);
            spdlog::register_logger(logger);
            return logger;
        }

        static inline bool s_initialized = false;
        static inline std::vector<spdlog::sink_ptr> s_sinks;
    };

    // ============================================================================
    // Convenience Macros
    // ============================================================================

    // Engine-core neutral logging (generic mechanisms: crypto, rate limiting,
    // protocol framing). Game/service category macros live with the consumer.
    #define LOG_CORE_TRACE(...)    ::Arcane::Logger::Get("Core")->trace(__VA_ARGS__)
    #define LOG_CORE_DEBUG(...)    ::Arcane::Logger::Get("Core")->debug(__VA_ARGS__)
    #define LOG_CORE_INFO(...)     ::Arcane::Logger::Get("Core")->info(__VA_ARGS__)
    #define LOG_CORE_WARN(...)     ::Arcane::Logger::Get("Core")->warn(__VA_ARGS__)
    #define LOG_CORE_ERROR(...)    ::Arcane::Logger::Get("Core")->error(__VA_ARGS__)
    #define LOG_CORE_CRITICAL(...) ::Arcane::Logger::Get("Core")->critical(__VA_ARGS__)

} // namespace Arcane
