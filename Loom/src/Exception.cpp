#include "arcpch.h"
#include "Exception.h"

#include "Util/StringUtil.h"

Exception::Exception(const std::wstring& message) : m_message(message) {}

const char* Exception::what() const noexcept
{
   m_utf8Message = ARC::StringUtil::ToString(m_message).c_str();
   return m_utf8Message.c_str();
}
