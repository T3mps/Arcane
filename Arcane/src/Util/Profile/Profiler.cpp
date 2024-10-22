#include "arcpch.h"
#include "Profiler.h"

#include "Logging/Logging.h"
#include "Util/Serialization/JSON.h"

ARC::Profiler::Profiler() : m_currentSession(nullptr) {}

ARC::Profiler::~Profiler() { EndSession(); }

void ARC::Profiler::BeginSession(const std::string& name, const std::string& filepath)
{
	auto& instance = GetInstance();
	std::lock_guard lock(instance.m_mutex);

	if (instance.m_currentSession)
	{
		if (LoggingManager::HasCoreLogger()) // Edge case: BeginSession() might be before Log::Init()
		{
			//Log::CoreError("Profiler::BeginSession('{}') when session '{}' already open.", name, instance.m_CurrentSession->name);
			Log::CoreError("A session is already open. End session before starting a new one.");
		}
		instance.InternalEndSession();
	}

	instance.m_outputStream.open(filepath);
	if (instance.m_outputStream.is_open())
	{
		instance.m_currentSession = std::make_unique<InstrumentationSession>(name);
		instance.WriteHeader();
	}
	else if (LoggingManager::HasCoreLogger()) // Edge case: BeginSession() might be before Log::Init()
	{
		//Log::CoreError("Profiler could not open results file '{}'.", filepath);
		Log::CoreError("Profiler could not open results file");
	}
}

void ARC::Profiler::EndSession()
{
	auto& instance = GetInstance();

	std::lock_guard lock(instance.m_mutex);
	instance.InternalEndSession();
}

void ARC::Profiler::WriteProfile(const ProfileResult& result)
{
	auto& instance = GetInstance();
	if (!instance.m_currentSession)
		return;

	ARC::JSON json;
	json.Set("cat", "function");
	json.Set("dur", result.elapsedTime.count());
	json.Set("name", result.name);
	json.Set("ph", "X");
	json.Set("pid", 0);
	json.Set("tid", result.threadID._Get_underlying_id());
	json.Set("ts", result.start.count());

	if (!instance.m_firstProfileWritten)
		instance.m_outputStream << ",";
	else
		instance.m_firstProfileWritten = false;

	instance.m_outputStream << json.ToString();
	instance.m_outputStream.flush();
}

void ARC::Profiler::InternalEndSession()
{
	if (m_currentSession)
	{
		WriteFooter();
		m_outputStream.close();
		m_currentSession.reset();
	}
}

void ARC::Profiler::WriteHeader()
{
	m_outputStream << R"({"otherData": {},"traceEvents":[)";
	m_outputStream.flush();
}

void ARC::Profiler::WriteFooter()
{
	m_outputStream << "]}";
	m_outputStream.flush();
}
