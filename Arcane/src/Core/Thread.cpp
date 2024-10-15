#include "arcpch.h"
#include "Thread.h"

#include "Util/StringUtil.h"

ARC::Thread::Thread(const std::string& name) : m_name(name) {}

void ARC::Thread::Join()
{
   if (m_thread.joinable())
      m_thread.join();
}

std::thread::id ARC::Thread::GetID() const
{
   return m_thread.get_id();
}

void ARC::Thread::SetName(const std::string& name)
{
   HANDLE threadHandle = m_thread.native_handle();

   std::wstring wideName = StringUtil::ToWString(name);

   SetThreadDescription(threadHandle, wideName.c_str());
   SetThreadAffinityMask(threadHandle, 8);
}

ARC::ThreadSignal::ThreadSignal(const std::string& name, bool manualReset)
{
   std::wstring str(name.begin(), name.end());
   m_signalHandle = CreateEvent(NULL, static_cast<BOOL>(manualReset), FALSE, str.c_str());
}

void ARC::ThreadSignal::Wait()
{
	WaitForSingleObject(m_signalHandle, INFINITE);
}

void ARC::ThreadSignal::Notify()
{
	SetEvent(m_signalHandle);
}

void ARC::ThreadSignal::Reset()
{
	ResetEvent(m_signalHandle);
}
