#pragma once

namespace ARC
{
	class Thread
	{
	public:
		Thread(const std::string& name);

		template<typename Fn, typename... Args>
		void Dispatch(Fn&& func, Args&&... args)
		{
			m_thread = std::thread(func, std::forward<Args>(args)...);
			SetName(m_name);
		}

		void Join();

		std::thread::id GetID() const;

		void SetName(const std::string& name);

	private:
		std::string m_name;
		std::thread m_thread;
	};

	class ThreadSignal
	{
	public:
		ThreadSignal(const std::string& name, bool manualReset = false);
		~ThreadSignal() = default;

		void Wait();
		void Notify();
		void Reset();

	private:
		void* m_signalHandle = nullptr;
	};
} // namespace ARC
