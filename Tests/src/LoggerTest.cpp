// Logger structured-event JSON escaping (E01-4) + baseline coverage (E03-4).
// Each Log*Event method splices content-derived fields into a hand-built JSON
// format string; adversarial values (quotes, backslashes, newlines, tabs,
// control chars) must come out as VALID, correctly-escaped JSON. We capture the
// emitted line via an ostream sink attached to the category logger and re-parse
// it with nlohmann/json: a successful parse whose fields equal the original raw
// inputs proves the escaping is both valid and lossless (no injection).
#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <Json.hpp>
#include <Arcane/Util/Logger.hpp>
#include <spdlog/sinks/ostream_sink.h>

using Arcane::Logger;
using Arcane::LogCategory;

namespace
{
    // Attaches a %v-pattern ostream sink to the given category loggers for the
    // lifetime of the object, then detaches and restores their levels. Works
    // whether or not Logger::Init() has run (Get() returns either the category
    // logger or the default logger; we drive that exact instance).
    struct LogCapture
    {
        std::ostringstream oss;
        spdlog::sink_ptr   sink;
        std::vector<std::shared_ptr<spdlog::logger>> touched;
        std::vector<spdlog::level::level_enum>       savedLevels;

        explicit LogCapture(std::initializer_list<LogCategory> cats)
        {
            sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss, /*force_flush=*/true);
            sink->set_pattern("%v");
            sink->set_level(spdlog::level::trace);
            for (auto cat : cats)
            {
                auto lg = Logger::Get(cat);
                bool already = false;
                for (auto& t : touched) if (t == lg) { already = true; break; }
                if (already) continue;
                touched.push_back(lg);
                savedLevels.push_back(lg->level());
                lg->sinks().push_back(sink);
                lg->set_level(spdlog::level::trace); // LogSessionEvent logs at debug
            }
        }

        ~LogCapture()
        {
            for (std::size_t i = 0; i < touched.size(); ++i)
            {
                auto& sinks = touched[i]->sinks();
                sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
                touched[i]->set_level(savedLevels[i]);
            }
        }

        std::string str() const { return oss.str(); }
    };

    // Split captured output into lines and parse each as JSON. A parse failure
    // (invalid/unescaped JSON) propagates as an exception -> Catch2 fails the test.
    std::vector<nlohmann::json> ParseLines(const std::string& s)
    {
        std::vector<nlohmann::json> out;
        std::istringstream in(s);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            out.push_back(nlohmann::json::parse(line));
        }
        return out;
    }
}

TEST_CASE("Logger: LogPullEvent escapes adversarial fields into valid JSON", "[logger]")
{
    const std::string evilPlayer = "play\"er";
    const std::string evilBanner = "Ban\"ner\\one\nnewline";
    const std::string evilItemId = "item\\id\tTAB";
    const std::string evilName   = "Sw\"ord\\Blade\nline2\x01SOH{brace}";

    LogCapture cap({LogCategory::Gacha});
    Logger::LogPullEvent(evilPlayer, evilBanner, 7, evilItemId, evilName,
                         5, /*pity=*/true, /*guarantee=*/false, /*featured=*/true, /*won5050=*/false);

    auto rows = ParseLines(cap.str());
    REQUIRE(rows.size() == 1);
    const auto& j = rows[0];
    REQUIRE(j.at("event").get<std::string>() == "pull");
    REQUIRE(j.at("player").get<std::string>() == evilPlayer);
    REQUIRE(j.at("banner").get<std::string>() == evilBanner);
    REQUIRE(j.at("item_id").get<std::string>() == evilItemId);
    REQUIRE(j.at("item_name").get<std::string>() == evilName);
    REQUIRE(j.at("pull_num").get<int>() == 7);
    REQUIRE(j.at("rarity").get<int>() == 5);
    REQUIRE(j.at("pity").get<bool>() == true);
    REQUIRE(j.at("guarantee").get<bool>() == false);
    REQUIRE(j.at("featured").get<bool>() == true);
    REQUIRE(j.at("won_5050").get<bool>() == false);
}

TEST_CASE("Logger: LogAuthEvent escapes fields with and without reason", "[logger]")
{
    LogCapture cap({LogCategory::Auth});
    Logger::LogAuthEvent("log\"in", "p\\layer", "1.2.3.4\"x", /*success=*/false, "bad \"pass\"\nword");
    Logger::LogAuthEvent("logout", "player2", "5.6.7.8", /*success=*/true); // empty reason

    auto rows = ParseLines(cap.str());
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].at("event").get<std::string>() == "log\"in");
    REQUIRE(rows[0].at("player").get<std::string>() == "p\\layer");
    REQUIRE(rows[0].at("ip").get<std::string>() == "1.2.3.4\"x");
    REQUIRE(rows[0].at("success").get<bool>() == false);
    REQUIRE(rows[0].at("reason").get<std::string>() == "bad \"pass\"\nword");

    REQUIRE(rows[1].at("event").get<std::string>() == "logout");
    REQUIRE(rows[1].at("success").get<bool>() == true);
    REQUIRE_FALSE(rows[1].contains("reason")); // empty reason omits the field
}

TEST_CASE("Logger: LogSessionEvent escapes fields (event key stays session_*)", "[logger]")
{
    LogCapture cap({LogCategory::Auth});
    Logger::LogSessionEvent("crea\"ted", "pl\\ayer", "some-token-value", "id\"le\nkick");

    auto rows = ParseLines(cap.str());
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].at("event").get<std::string>() == "session_crea\"ted");
    REQUIRE(rows[0].at("player").get<std::string>() == "pl\\ayer");
    REQUIRE(rows[0].at("reason").get<std::string>() == "id\"le\nkick");
    // token_hash is the first 8 hex chars of SHA-256(token) -- present and safe.
    REQUIRE(rows[0].at("token_hash").get<std::string>().size() == 8);
}

TEST_CASE("Logger: LogConnectionEvent escapes fields", "[logger]")
{
    LogCapture cap({LogCategory::Net});
    Logger::LogConnectionEvent("connec\"ted", "9.9.9.9\\x", "re\"ason\nline");
    Logger::LogConnectionEvent("disconnected", "1.1.1.1"); // empty reason

    auto rows = ParseLines(cap.str());
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].at("event").get<std::string>() == "connec\"ted");
    REQUIRE(rows[0].at("ip").get<std::string>() == "9.9.9.9\\x");
    REQUIRE(rows[0].at("reason").get<std::string>() == "re\"ason\nline");
    REQUIRE(rows[1].at("event").get<std::string>() == "disconnected");
    REQUIRE_FALSE(rows[1].contains("reason"));
}

TEST_CASE("Logger: a benign event is unchanged and still valid JSON", "[logger]")
{
    LogCapture cap({LogCategory::Gacha});
    Logger::LogPullEvent("player1", "standard", 1, "char_001", "Aria",
                         5, false, false, true, true);
    auto rows = ParseLines(cap.str());
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].at("player").get<std::string>() == "player1");
    REQUIRE(rows[0].at("item_name").get<std::string>() == "Aria");
    REQUIRE(rows[0].at("banner").get<std::string>() == "standard");
}
