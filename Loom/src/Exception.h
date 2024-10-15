#pragma once

#include <exception>

class Exception : public std::exception
{
public:
   Exception(const std::wstring& message);
   const char* what() const noexcept override;

private:
   std::wstring m_message;
   mutable std::string m_utf8Message;
};

class CommandLineException : public Exception
{
public:
   using Exception::Exception;
};

class DLLLoadException : public Exception
{
public:
   using Exception::Exception;
};

