#include "arcpch.h"
#include "Instrumentation.h"

ARC::InstrumentationTimer::InstrumentationTimer(const char* name) :
	m_name(name),
	m_startTimepoint(std::chrono::steady_clock::now()),
	m_stopped(false)
{}

ARC::InstrumentationTimer::~InstrumentationTimer()
{
	if (!m_stopped)
		Stop();
}

void ARC::InstrumentationTimer::Stop()
{
	auto endTimepoint = std::chrono::steady_clock::now();
	auto highResStart = FloatingPointMicroseconds{ m_startTimepoint.time_since_epoch() };
	auto elapsedTime = std::chrono::time_point_cast<std::chrono::microseconds>(endTimepoint).time_since_epoch() - std::chrono::time_point_cast<std::chrono::microseconds>(m_startTimepoint).time_since_epoch();

	Profiler::WriteProfile({ m_name, highResStart, elapsedTime, std::this_thread::get_id() });

	m_stopped = true;
}
