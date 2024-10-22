#pragma once

#include <condition_variable>

namespace ARC
{
   template<typename T>
   class ConcurrentQueue
   {
   public:
      ConcurrentQueue() = default;
      ~ConcurrentQueue() = default;

      template<typename U>
      void Push(U&& value)
      {
         {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::forward<U>(value));
         }
         m_conditionVar.notify_one();
      }

      std::optional<T> TryPop()
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         if (m_queue.empty()) return std::nullopt;
         T value = std::move(m_queue.front());
         m_queue.pop();
         return value;
      }

      T WaitAndPop()
      {
         std::unique_lock<std::mutex> lock(m_mutex);
         m_conditionVar.wait(lock, [this] { return !m_queue.empty(); });
         T value = std::move(m_queue.front());
         m_queue.pop();
         return value;
      }

      bool Empty() const
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         return m_queue.empty();
      }

   private:
      mutable std::mutex m_mutex;
      std::queue<T> m_queue;
      std::condition_variable m_conditionVar;
   };
} // namespace ARC
