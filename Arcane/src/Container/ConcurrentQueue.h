#pragma once

#include <condition_variable>

namespace ARC
{
   template<typename T>
   class ConcurrentQueue
   {
   public:
      ConcurrentQueue(size_t capacity = 1024)
         : m_capacity(capacity), m_buffer(std::make_unique<T[]>(capacity))
      {
         ARC_ASSERT(capacity > 0 && (capacity & (capacity - 1)) == 0, "Capacity must be a power of two");
      }

      ~ConcurrentQueue() = default;

      // Producer: Multiple threads can call this concurrently
      template<typename U>
      void Push(U&& value)
      {
         size_t head = m_head.load(std::memory_order_relaxed);

         size_t next_head = (head + 1) & m_indexMask;

         // Check if the buffer is full
         while (next_head == m_tail.load(std::memory_order_acquire))
         {
            // Queue is full; optionally handle this case
            // For real-time, we might wait or drop the message
            std::this_thread::yield();
         }

         m_buffer[head] = std::forward<U>(value);

         m_head.store(next_head, std::memory_order_release);

         // Notify the consumer
         {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_hasData = true;
         }
         m_conditionVar.notify_one();
      }

      // Consumer: Only the consumer thread should call this
      std::optional<T> TryPop()
      {
         size_t tail = m_tail.load(std::memory_order_relaxed);

         if (tail == m_head.load(std::memory_order_acquire))
         {
            return std::nullopt;
         }

         T value = std::move(m_buffer[tail]);

         m_tail.store((tail + 1) & m_indexMask, std::memory_order_release);

         return value;
      }

      // Consumer waits until data is available
      void WaitForData()
      {
         std::unique_lock<std::mutex> lock(m_mutex);
         m_conditionVar.wait(lock, [this] { return m_hasData; });
         m_hasData = false;
      }

   private:
      const size_t m_capacity;
      const size_t m_indexMask = m_capacity - 1;
      std::unique_ptr<T[]> m_buffer;

      std::atomic<size_t> m_head{0}; // Next position to write (producers)
      std::atomic<size_t> m_tail{0}; // Next position to read (consumer)

      std::mutex m_mutex;
      std::condition_variable m_conditionVar;
      bool m_hasData{false};
   };
} // namespace ARC
