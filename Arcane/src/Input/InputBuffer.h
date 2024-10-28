#pragma once

#include "InputActionBase.h"

namespace ARC
{
   template<typename InputType>
   class InputBuffer
   {
   public:
      struct BufferedInput
      {
         std::shared_ptr<InputActionBase> action;
         std::chrono::steady_clock::time_point timestamp;
         bool used;
      };

      void Add(const std::shared_ptr<InputActionBase>& action)
      {
         auto now = std::chrono::steady_clock::now();
         CleanOldInputs(now);

         // Only add if the action just started and it's not already in the recent inputs
         if (action->IsStarted() && !IsRecentlyAdded(action, now))
            m_buffer.push_back({action, now, false});
      }

      void Clear()
      {
         m_buffer.clear();
      }

      bool MatchSequence(const std::vector<std::shared_ptr<InputActionBase>>& sequence,  std::chrono::milliseconds timeout)
      {
         if (m_buffer.empty() || sequence.empty())
            return false;

         auto now = std::chrono::steady_clock::now();
         CleanOldInputs(now);

         if (m_buffer.size() < sequence.size())
            return false;

         // Find a continuous sequence match
         size_t matchStart = m_buffer.size();
         for (size_t i = 0; i <= m_buffer.size() - sequence.size(); ++i)
         {
            if (IsSequenceMatch(i, sequence, timeout))
            {
               matchStart = i;
               break;
            }
         }

         if (matchStart < m_buffer.size())
         {
            for (size_t i = 0; i < sequence.size(); ++i)
            {
               m_buffer[matchStart + i].used = true;
            }

            CleanUsedInputs();
            return true;
         }

         return false;
      }

   private:
      bool IsRecentlyAdded(const std::shared_ptr<InputActionBase>& action, std::chrono::steady_clock::time_point now)
      {
         static const auto duplicateWindow = std::chrono::milliseconds(100);

         for (const auto& input : m_buffer)
         {
            if (input.action.get() == action.get())
            {
               auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - input.timestamp);
               if (age < duplicateWindow)
                  return true;
            }
         }
         return false;
      }

      bool IsSequenceMatch(size_t startIdx, const std::vector<std::shared_ptr<InputActionBase>>& sequence, std::chrono::milliseconds timeout)
      {
         if (startIdx + sequence.size() > m_buffer.size())
            return false;

         for (size_t i = 0; i < sequence.size(); ++i)
         {
            if (m_buffer[startIdx + i].used)
               return false;
         }

         // Check sequence match with timing
         for (size_t i = 0; i < sequence.size(); ++i)
         {
            // Check action match
            if (m_buffer[startIdx + i].action.get() != sequence[i].get())
               return false;

            // Check timing between consecutive inputs
            if (i > 0)
            {
               auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(m_buffer[startIdx + i].timestamp - m_buffer[startIdx + i - 1].timestamp);
               if (timeDiff > timeout)
                  return false;
            }
         }

         return true;
      }

      void CleanOldInputs(const std::chrono::steady_clock::time_point& now)
      {
         static const auto maxBufferAge = std::chrono::milliseconds(500);

         while (!m_buffer.empty())
         {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_buffer.front().timestamp);

            if (age > maxBufferAge)
            {
               m_buffer.pop_front();
               continue;
            }
            break;
         }
      }

      void CleanUsedInputs()
      {
         auto lastUsed = m_buffer.rend();
         for (auto it = m_buffer.rbegin(); it != m_buffer.rend(); ++it)
         {
            if (it->used)
            {
               lastUsed = it;
               break;
            }
         }

         if (lastUsed != m_buffer.rend())
         {
            // Convert reverse iterator to forward iterator and erase up to that point
            m_buffer.erase(m_buffer.begin(), lastUsed.base());
         }
      }

      std::deque<BufferedInput> m_buffer;
   };
}
