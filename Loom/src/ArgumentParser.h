#pragma once

static constexpr std::string_view LOOM_USEAGE_STRING = "Usage: loom.exe <module> [options] [flags]";

class ArgumentParser
{
public:
   using ArgHandler = std::function<void(const std::string&)>;

   ArgumentParser() = default;
   ~ArgumentParser() = default;

   void Parse(int32_t argc, char* argv[]);

   void RegisterOption(const std::string& option, const ArgHandler& handler, const std::string& description = "");
   void RegisterFlag(const std::string& flag, const ArgHandler& handler, const std::string& description = "");

   void UnregisterOptionHandler(const std::string& option);
   void UnregisterFlagHandler(const std::string& flag);

   void UnregisterAllOptionHandlers();
   void UnregisterAllFlagHandlers();

   [[nodiscard]] bool HasFlag(const std::string& flag) const { return m_flags.find(flag) != m_flags.end(); }
   [[nodiscard]] ARC::Result<std::string> GetOptionValue(const std::string& option) const;
   [[nodiscard]] const std::vector<std::string>& GetPositionalArgs() const { return m_positionalArgs; }

   void Clear(bool clearHandlers = false);

   [[nodiscard]] std::string BuildHelpMessage() const;

private:
   std::unordered_map<std::string, ArgHandler> m_optionHandlers;
   std::unordered_map<std::string, ArgHandler> m_flagHandlers;
   std::unordered_set<std::string> m_flags;
   std::unordered_map<std::string, std::string> m_options;
   std::vector<std::string> m_positionalArgs;

   std::unordered_map<std::string, std::string> m_descriptions;  // Stores descriptions for help
};
