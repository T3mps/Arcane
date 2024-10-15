#pragma once

#include "../Singleton.h"
#include "Instrumentor.h"

namespace ARC
{
	class Profiler : public Singleton<Profiler>
	{
	public:
		Profiler(const Profiler&) = delete;
		Profiler(Profiler&&) = delete;

		static void BeginSession(const std::string& name, const std::string& filepath = "results.json");
		static void EndSession();

		static void WriteProfile(const ProfileResult& result);

	private:
		Profiler();
		~Profiler();

		void InternalEndSession();

		void WriteHeader();
		void WriteFooter();

		std::mutex m_mutex;
		std::unique_ptr<InstrumentationSession> m_currentSession;
		std::ofstream m_outputStream;
		bool m_firstProfileWritten = true;

		friend class Singleton<Profiler>;
	};
}
