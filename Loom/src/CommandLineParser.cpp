#include "arcpch.h"
#include "CommandLineParser.h"

#include "Exception.h"
#include "Util/StringUtil.h"

void AddOption(OptionHandlers& handlers, const std::wstring& option, OptionHandler handler)
{
   handlers[option] = handler;
}

void ParseCommandLine(int argc, char* argv[], const OptionHandlers& handlers, PositionalArguments& positionalArgs, std::wstring& errorMsg)
{
   for (int i = 1; i < argc; ++i) // Start from 1 to skip argv[0] (program name)
   {
      std::string arg = argv[i];
      std::wstring warg = ARC::StringUtil::ToWString(arg);

      if (warg[0] == L'-')
      {
         auto it = handlers.find(warg);
         if (it != handlers.end())
         {
            if (i + 1 < argc)
            {
               std::string nextArg = argv[++i];
               std::wstring wnextArg = ARC::StringUtil::ToWString(nextArg);
               it->second(wnextArg);
            }
            else
            {
               errorMsg = L"Option " + warg + L" requires an argument.";
               return;
            }
         }
         else
         {
            errorMsg = L"Unknown option: " + warg;
            return;
         }
      }
      else
      {
         positionalArgs.push(warg);
         ARC_CORE_INFO(L"Found positional argument '" + warg + L"'");
      }
   }

   if (positionalArgs.empty())
   {
      errorMsg = L"No positional arguments provided.";
   }
}

const std::wstring PopPositionalArgument(PositionalArguments& args)
{
   if (args.empty())
   {
      const std::wstring errorMsg = L"No more positional arguments.";
      ARC_CORE_ERROR(errorMsg);
      throw CommandLineException(errorMsg);
   }

   std::wstring arg = args.top();
   args.pop();
   return arg;
}

size_t GetPositionalArgumentCount(const PositionalArguments& args)
{
   return args.size();
}

bool ProcessCommandLine(int argc, char* argv[], OptionHandlers& handlers, PositionalArguments& positionalArgs, std::wstring& errorMsg)
{
   ParseCommandLine(argc, argv, handlers, positionalArgs, errorMsg);
   return errorMsg.empty();
}

int32_t TryPopPositionalArgument(PositionalArguments& args, std::wstring& resultVar, const wchar_t* errorMsg)
{
   try
   {
      resultVar = PopPositionalArgument(args);
   }
   catch (const std::out_of_range&)
   {
      ARC_CORE_ERROR(errorMsg);
      return 1;
   }
   ARC_CORE_INFO(L"Found client module '" + resultVar + L"'");
   return 0;
}