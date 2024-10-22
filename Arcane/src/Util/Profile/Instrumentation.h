#pragma once

#include "Instrumentor.h"
#include "Profiler.h"

namespace ARC
{
	class InstrumentationTimer
	{
	public:
		InstrumentationTimer(const char* name);
		~InstrumentationTimer();

		void Stop();

	private:
		const char* m_name;
		std::chrono::time_point<std::chrono::steady_clock> m_startTimepoint;
		bool m_stopped;
	};

	namespace Internal
	{
		template <size_t N>
		struct ChangeResult
		{
			char data[N];
		};

		template <size_t N, size_t K>
		constexpr auto CleanupOutputString(const char(&expr)[N], const char(&remove)[K])
		{
			ChangeResult<N> result = {};

			size_t srcIndex = 0;
			size_t dstIndex = 0;
			while (srcIndex < N)
			{
				size_t matchIndex = 0;
				while (matchIndex < K - 1 && srcIndex + matchIndex < N - 1 && expr[srcIndex + matchIndex] == remove[matchIndex])
					matchIndex++;
				if (matchIndex == K - 1)
					srcIndex += matchIndex;
				result.data[dstIndex++] = expr[srcIndex] == '"' ? '\'' : expr[srcIndex];
				srcIndex++;
			}
			return result;
		}
	} // namespace Internal
} // namespace ARC

#if ARC_PROFILE
	#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__)
		#define ARC_FUNC_SIG __PRETTY_FUNCTION__
	#elif defined(__DMC__) && (__DMC__ >= 0x810)
		#define ARC_FUNC_SIG __PRETTY_FUNCTION__
	#elif (defined(__FUNCSIG__) || (_MSC_VER))
		#define ARC_FUNC_SIG __FUNCSIG__
	#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || (defined(__IBMCPP__) && (__IBMCPP__ >= 500))
		#define ARC_FUNC_SIG __FUNCTION__
	#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)
		#define ARC_FUNC_SIG __FUNC__
	#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
		#define ARC_FUNC_SIG __func__
	#elif defined(__cplusplus) && (__cplusplus >= 201103)
		#define ARC_FUNC_SIG __func__
	#else
		#define ARC_FUNC_SIG "ARC_FUNC_SIG unknown!"
	#endif

	#define ARC_PROFILE_BEGIN_SESSION(name, filepath)	::ARC::Profiler::BeginSession(name, filepath)
	#define ARC_PROFILE_END_SESSION()						::ARC::Profiler::EndSession()

	#define ARC_PROFILE_SCOPE_LINE(name, line)			constexpr auto fixedName##line = ::ARC::Internal::CleanupOutputString(name, "__cdecl "); \
																			::ARC::InstrumentationTimer timer##line(fixedName##line.data)
	#define ARC_PROFILE_SCOPE(name)							ARC_PROFILE_SCOPE_LINE(name, __LINE__)
	#define ARC_PROFILE_FUNCTION()							ARC_PROFILE_SCOPE(ARC_FUNC_SIG)
#else
	#define ARC_PROFILE_BEGIN_SESSION(name, filepath)
	#define ARC_PROFILE_END_SESSION()
	#define ARC_PROFILE_SCOPE(name)
	#define ARC_PROFILE_FUNCTION()
#endif
