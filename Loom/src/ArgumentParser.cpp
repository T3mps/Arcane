#include "arcpch.h"
#include "ArgumentParser.h"

#include "Core/Console.h"

void ArgumentParser::Parse(int32_t argc, char* argv[])
{
   for (int32_t i = 1; i < argc; ++i)
   {
      std::string arg = argv[i];

      if (arg[0] == '-') // Flag or option (-flag --option)
      {
         if (arg[1] == '-') // Option (--option)
         {
            if (arg.size() == 2)
            {
               ARC::Log::CoreError("Invalid argument: '--'");
               continue;
            }
            std::string option = arg.substr(2);
            if ((i + 1) < argc && argv[i + 1][0] != '-')
            {
               m_options[option] = argv[++i];
               if (m_optionHandlers.find(option) != m_optionHandlers.end())
               {
                  m_optionHandlers[option](m_options[option]);
               }
            }
            else
            {
               // Flag without value
               m_flags.insert(option);
            }
         }
         else
         {
            // Short flag(s): -f or -abc
            for (size_t j = 1; j < arg.size(); ++j)
            {
               std::string flag(1, arg[j]);
               m_flags.insert(flag);
               if (m_flagHandlers.find(flag) != m_flagHandlers.end())
               {
                  m_flagHandlers[flag](flag);
               }
            }
         }
      }
      else // Positional argument
      {
         m_positionalArgs.push_back(arg);
      }
   }
}

void ArgumentParser::RegisterOption(const std::string& option, const ArgHandler& handler, const std::string& description)
{
   m_optionHandlers[option] = handler;
   m_descriptions[option] = description;
}

void ArgumentParser::RegisterFlag(const std::string& flag, const ArgHandler& handler, const std::string& description)
{
   m_flagHandlers[flag] = handler;
   m_descriptions[flag] = description;
}

void ArgumentParser::UnregisterOptionHandler(const std::string& option)
{
   auto it = m_optionHandlers.find(option);
   if (it != m_optionHandlers.end())
      m_optionHandlers.erase(it);
   else
      ARC::Log::CoreWarn("Attempted to unregister handler for option '" + option + "', but no handler is registered for this option.");
}

void ArgumentParser::UnregisterFlagHandler(const std::string& flag)
{
   auto it = m_flagHandlers.find(flag);
   if (it != m_flagHandlers.end())
      m_flagHandlers.erase(it);
   else
      ARC::Log::CoreWarn("Attempted to unregister handler for flag '" + flag + "', but no handler is registered for this flag.");
}

void ArgumentParser::UnregisterAllOptionHandlers()
{
   m_optionHandlers.clear();
}

void ArgumentParser::UnregisterAllFlagHandlers()
{
   m_flagHandlers.clear();
}

ARC::Result<std::string> ArgumentParser::GetOptionValue(const std::string& option) const
{
   auto it = m_options.find(option);
   if (it != m_options.end())
      return it->second;
   return ARC::Error::Create(ARC::ErrorCode::IndexOutOfBounds, "Attempted to retrieve launch option that was not supplied: " + option);
}

void ArgumentParser::Clear(bool clearHandlers)
{
   if (clearHandlers)
   {
      m_optionHandlers.clear();
      m_flagHandlers.clear();
   }
   m_flags.clear();
   m_options.clear();
   m_positionalArgs.clear();
   m_descriptions.clear();
}

std::string ArgumentParser::BuildHelpMessage() const
{
   size_t maxLength = 0;

   for (const auto& [option, description] : m_descriptions)
   {
      if (m_optionHandlers.find(option) != m_optionHandlers.end())
         maxLength = std::max(maxLength, option.length() + 2); // +2 for -- prefix
   }

   for (const auto& [flag, description] : m_descriptions)
   {
      if (m_flagHandlers.find(flag) != m_flagHandlers.end())
         maxLength = std::max(maxLength, flag.length() + 1);// +1 for - prefix
   }

   constexpr int32_t columnPadding = 4;
   int32_t optionColumnWidth = static_cast<int32_t>(maxLength + columnPadding);

   if (optionColumnWidth == columnPadding)
      return std::string();

   std::ostringstream oss;
   oss << "\n" << LOOM_USEAGE_STRING << "\n\n"
       << "Options:\n";

   for (const auto& [option, description] : m_descriptions)
   {
      if (m_optionHandlers.find(option) != m_optionHandlers.end())
         oss << std::format("{:<{}}- {}\n", "--" + option, optionColumnWidth, description);
   }

   oss << "\nFlags:\n";

   for (const auto& [flag, description] : m_descriptions)
   {
      if (m_flagHandlers.find(flag) != m_flagHandlers.end())
         oss << std::format("{:<{}}- {}\n", "-" + flag, optionColumnWidth, description);
   }

   return oss.str();
}
