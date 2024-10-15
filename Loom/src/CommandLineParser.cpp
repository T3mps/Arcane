#include "arcpch.h"
#include "CommandLineParser.h"

#include "Exception.h"

void AddOption(OptionHandlers& handlers, const std::wstring& option, OptionHandler handler)
{
   handlers[option] = handler;
}

void ParseCommandLine(int32_t argc, LPWSTR* argv, const OptionHandlers& handlers, PositionalArguments& positionalArgs, std::wstring& errorMsg)
{
   for (int32_t i = 0; i < argc; ++i)
   {
      std::wstring arg = argv[i];
      if (arg[0] == L'-')
      {
         auto it = handlers.find(arg);
         if (it != handlers.end())
         {
            if (i + 1 < argc)
            {
               it->second(argv[++i]);
            }
            else
            {
               errorMsg = L"Option " + arg + L" requires an argument.";
               return;
            }
         }
         else
         {
            errorMsg = L"Unknown option: " + arg;
            return;
         }
      }
      else
      {
         positionalArgs.push(arg);
         ARC_CORE_INFO(L"Found positional argument '" + arg + L"'");
      }
   }

   if (positionalArgs.empty())
   {
      errorMsg = L"No positional arguments provided.";
      return;
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

bool ProcessCommandLine(LPWSTR lpCmdLine, OptionHandlers& handlers, PositionalArguments& positionalArgs, std::wstring& errorMsg)
{
   int32_t argc;
   LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
   if (!argv)
   {
      errorMsg = L"Failed to parse command line.";
      return false;
   }
   std::unique_ptr<LPWSTR, decltype(&LocalFree)> argvGuard(argv, LocalFree);

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
   ARC_CORE_INFO(L"Found client DLL '" + resultVar + L"'");
   return 0;
}