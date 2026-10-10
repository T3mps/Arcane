#pragma once

// Arcane logging system
// Generic engine logging built on spdlog: lazily-created string-keyed named
// loggers over a shared console + optional rotating-file sink stack, plus the
// JsonEscape kernel consumers use to build structured JSON log events.
// Game/service vocabulary (categories, analytics events, log file names)
// lives with the consumer (e.g. the server-side facade in Server/Common).
// Levels, patterns, flush level and file rotation are the log.server.*
// settings (Util/LogServerSettings.hpp; settings arc S6-3), read through
// ArcaneCore exports so this header needs no Astra include path.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Util/LogServerSettingsData.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
        // Initialize the logging system - call once at startup, on the main
        // thread. An empty logFilePath skips the rotating-file sink (console
        // only). Calling Init again with a file path AFTER a console-only
        // (auto) init upgrades in place: the file sink is installed and
        // attached to every already-registered logger, so an early LOG_CORE_*
        // cannot silently lock the process out of its log file.
        //
        // A level left out is log.server.consoleLevel / fileLevel. The Live
        // log.server.* rows follow later publishes: the first Init registers
        // the callbacks (main thread; the auto-init in Get never does, it may
        // run on any thread). A level passed here holds until its setting
        // next changes.
        static void Init(std::optional<Level> consoleLevel = std::nullopt, std::optional<Level> fileLevel = std::nullopt,
                         const std::string& logFilePath = "")
        {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                const LogServerSettings s = PublishedLogServerSettings();
                InitUnlocked(consoleLevel.value_or(ToLevel(s.consoleLevel)), fileLevel.value_or(ToLevel(s.fileLevel)),
                             logFilePath);
            }
            FollowSettings();
        }

        // Shutdown the logging system - call at exit
        static void Shutdown()
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            spdlog::shutdown();
            s_sinks.clear();
            s_initialized = false;
        }

        // Generic named-logger access. Creates the logger on first use with the
        // sinks configured at Init (console always; file sink when Init was given
        // a log file path). Category names are the CONSUMER's vocabulary -- the
        // engine core itself logs only under "Core" (LOG_CORE_* below).
        // Thread-safe: the registry lookup is spdlog-internal-locked; the
        // create-on-miss path is serialized under s_mutex (register_logger
        // throws on a duplicate name, so check-then-register must not race).
        static spdlog::logger* Get(std::string_view name)
        {
            std::string key(name);
            if (auto existing = spdlog::get(key))
                return existing.get();

            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_initialized)
            {
                const LogServerSettings s = PublishedLogServerSettings();
                InitUnlocked(ToLevel(s.consoleLevel), ToLevel(s.fileLevel), "");
            }
            if (auto existing = spdlog::get(key))  // lost the create race: reuse
                return existing.get();
            return CreateLogger(key, s_sinks).get();
        }

        // Set console log level at runtime. All named loggers share the sink
        // objects captured at Init (s_sinks), so setting the shared sink's
        // level covers every logger -- including lazily-created ones.
        static void SetConsoleLevel(Level level)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_sinks.empty())
                s_sinks[0]->set_level(static_cast<spdlog::level::level_enum>(level));
        }

        // Set file log level at runtime (no-op when Init had no file path).
        static void SetFileLevel(Level level)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_sinks.size() > 1)
                s_sinks[1]->set_level(static_cast<spdlog::level::level_enum>(level));
        }

        // Check if initialized
        static bool IsInitialized() { return s_initialized; }

        // E01-4: minimal JSON string-value escaper. Consumers that build
        // structured JSON log events (e.g. a server facade's analytics
        // methods) splice content-derived fields into
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
        // A log.server.* level (0 trace .. 6 off) as a Level.
        static Level ToLevel(std::int32_t level) { return static_cast<Level>(std::clamp(level, 0, 6)); }
        static spdlog::level::level_enum ToSpdLevel(std::int32_t level)
        {
            return static_cast<spdlog::level::level_enum>(ToLevel(level));
        }

        // Re-applies the Live log.server.* rows to the running sinks. These
        // are THIS module's copies (the class is header-only, so each module
        // has its own sinks and spdlog registry), which is why Init registers
        // the callback from the consumer's module rather than ArcaneCore.dll.
        // `user` is the row (a Row value cast to a pointer).
        enum class Row : std::uintptr_t { ConsoleLevel = 1, FileLevel, Pattern, FilePattern, FlushLevel };
        static void OnSettingPublished(CVarHandle, void* user)
        {
            const LogServerSettings s = PublishedLogServerSettings();
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_initialized)
                return;
            switch (static_cast<Row>(reinterpret_cast<std::uintptr_t>(user)))
            {
                case Row::ConsoleLevel: if (!s_sinks.empty())   s_sinks[0]->set_level(ToSpdLevel(s.consoleLevel)); break;
                case Row::FileLevel:    if (s_sinks.size() > 1) s_sinks[1]->set_level(ToSpdLevel(s.fileLevel));    break;
                case Row::Pattern:      if (!s_sinks.empty())   s_sinks[0]->set_pattern(s.pattern);                break;
                case Row::FilePattern:  if (s_sinks.size() > 1) s_sinks[1]->set_pattern(s.filePattern);            break;
                case Row::FlushLevel:   spdlog::flush_on(ToSpdLevel(s.flushLevel));                                break;
            }
        }

        // Once per module. A Dev row that Dist compiles out has no cvar and
        // keeps its default.
        static void FollowSettings()
        {
            if (s_followingSettings)
                return;
            s_followingSettings = true;
            struct Binding { const char* name; Row row; };
            static constexpr Binding kBindings[] = {
                { "log.server.consoleLevel", Row::ConsoleLevel },
                { "log.server.fileLevel",    Row::FileLevel },
                { "log.server.pattern",      Row::Pattern },
                { "log.server.filePattern",  Row::FilePattern },
                { "log.server.flushLevel",   Row::FlushLevel },
            };
            CVarRegistry& reg = CVarRegistry::Get();
            for (const Binding& b : kBindings)
            {
                const CVarHandle h = reg.Find(b.name);
                if (!h.IsStale())
                    reg.AddCallback(h, &OnSettingPublished, reinterpret_cast<void*>(static_cast<std::uintptr_t>(b.row)));
            }
        }

        // Body of Init; caller must hold s_mutex.
        static void InitUnlocked(Level consoleLevel, Level fileLevel, const std::string& logFilePath)
        {
            if (s_initialized)
            {
                if (logFilePath.empty())
                    return;  // idempotent re-init, nothing to add

                if (s_sinks.size() > 1)
                {
                    // A file sink is already installed; keep it rather than
                    // silently splitting the stream across two files.
                    if (auto core = spdlog::get("Core"))
                        core->warn("Logger::Init called again with '{}'; existing file sink kept", logFilePath);
                    return;
                }

                // Late upgrade: something logged before the app's real Init
                // (auto-init installed console only). Install the file sink now
                // and attach it to every already-registered logger so the file
                // stream is not lost for the process lifetime.
                try
                {
                    auto fileSink = MakeFileSink(logFilePath, fileLevel);
                    s_sinks.push_back(fileSink);
                    spdlog::apply_all([&](std::shared_ptr<spdlog::logger> l) {
                        l->sinks().push_back(fileSink);
                    });
                    if (auto core = spdlog::get("Core"))
                        core->info("Logger: file sink '{}' installed late (console-only auto-init preceded Init)", logFilePath);
                }
                catch (const spdlog::spdlog_ex& ex)
                {
                    std::cerr << "Logger late file-sink install failed: " << ex.what() << std::endl;
                }
                return;
            }

            try
            {
                // Create sinks
                // stderr, not stdout: hosts use stdout as a DATA channel (the
                // engine-identity probe prints one line of JSON there for the
                // Arcane Hub to parse). See Arcane/Base/Log.cpp for the full note.
                auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                consoleSink->set_level(static_cast<spdlog::level::level_enum>(consoleLevel));
                consoleSink->set_pattern(PublishedLogServerSettings().pattern);

                std::vector<spdlog::sink_ptr> sinks = { consoleSink };

                if (!logFilePath.empty())
                    sinks.push_back(MakeFileSink(logFilePath, fileLevel));

                // Remember the sink stack so lazily-created named loggers
                // (Get(std::string_view) above) share the same outputs. No
                // named logger is created eagerly -- Get is lazy and the
                // category vocabulary belongs to the consumer.
                s_sinks = sinks;

                // Set global level to trace (individual sinks control filtering)
                spdlog::set_level(spdlog::level::trace);

                // Flush at log.server.flushLevel (info: JSON events are written
                // at once). This reaches loggers that already exist; CreateLogger
                // gives each later one the same level.
                spdlog::flush_on(ToSpdLevel(PublishedLogServerSettings().flushLevel));

                s_initialized = true;
            }
            catch (const spdlog::spdlog_ex& ex)
            {
                std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
            }
        }

        static spdlog::sink_ptr MakeFileSink(const std::string& logFilePath, Level fileLevel)
        {
            // Ensure the log directory exists
            auto logDir = std::filesystem::path(logFilePath).parent_path();
            if (!logDir.empty())
                std::filesystem::create_directories(logDir);

            // log.server.file.* (Restart: read when the sink is made).
            const LogServerFileSettings rotation = PublishedLogServerFileSettings();
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                logFilePath,
                static_cast<std::size_t>(rotation.maxBytes),
                static_cast<std::size_t>(std::max(rotation.maxFiles, 1)));
            fileSink->set_level(static_cast<spdlog::level::level_enum>(fileLevel));
            fileSink->set_pattern(PublishedLogServerSettings().filePattern);
            return fileSink;
        }

        static std::shared_ptr<spdlog::logger> CreateLogger(const std::string& name, const std::vector<spdlog::sink_ptr>& sinks)
        {
            auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
            logger->set_level(spdlog::level::trace);
            // register_logger, unlike initialize_logger, does not apply the
            // registry's flush level: without this a lazily created logger
            // never flushed early, whatever spdlog::flush_on said.
            logger->flush_on(ToSpdLevel(PublishedLogServerSettings().flushLevel));
            spdlog::register_logger(logger);
            return logger;
        }

        static inline bool s_initialized = false;
        static inline bool s_followingSettings = false;
        static inline std::mutex s_mutex;
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
