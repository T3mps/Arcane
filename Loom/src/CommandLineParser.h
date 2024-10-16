#pragma once   

using OptionHandler        = std::function<void(const std::wstring&)>;
using OptionHandlers       = std::unordered_map<std::wstring, OptionHandler>;
using PositionalArguments  = std::stack<std::wstring>;

void AddOption(OptionHandlers& handlers, const std::wstring& option, OptionHandler handler);
void ParseCommandLine(int argc, char* argv[], const OptionHandlers& handlers, PositionalArguments& positionalArgs, std::wstring& errorMsg);
const std::wstring PopPositionalArgument(PositionalArguments& args);
size_t GetPositionalArgumentCount(const PositionalArguments& args);
bool ProcessCommandLine(int argc, char* argv[], OptionHandlers& handlers, PositionalArguments& positionalArgs, std::wstring& errorMsg);
int32_t TryPopPositionalArgument(PositionalArguments& args, std::wstring& resultVar, const wchar_t* errorMsg);
