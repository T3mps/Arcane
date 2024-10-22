#include "arcpch.h"
#include "BehaviorTree.h"

ARC::BehaviorTree::Status ARC::BehaviorTree::Sequence::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   std::string key = "SequenceIndex_" + m_uuid.ToString();

   size_t currentIndex = 0;
   if (blackboard.HasValue(key))
   {
      currentIndex = blackboard.GetValue<size_t>(key).value();
   }
   else
   {
      blackboard.SetValue<size_t>(key, currentIndex);
   }

   while (currentIndex < m_children.size())
   {
      Status status = m_children[currentIndex]->Update(scene, entity, blackboard);
      if (status == Status::Running)
      {
         blackboard.SetValue<size_t>(key, currentIndex);
         return Status::Running;
      }
      if (status == Status::Failure)
      {
         blackboard.SetValue<size_t>(key, 0);
         return Status::Failure;
      }
      ++currentIndex;
   }

   blackboard.SetValue<size_t>(key, 0);
   return Status::Success;
}

ARC::BehaviorTree::Status ARC::BehaviorTree::Selector::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   std::string key = "SelectorIndex_" + m_uuid.ToString();
   size_t currentIndex = 0;

   if (blackboard.HasValue(key))
   {
      currentIndex = blackboard.GetValue<size_t>(key).value();
   }
   else
   {
      blackboard.SetValue<size_t>(key, currentIndex);
   }

   while (currentIndex < m_children.size())
   {
      Status status = m_children[currentIndex]->Update(scene, entity, blackboard);
      if (status == Status::Running)
      {
         blackboard.SetValue<size_t>(key, currentIndex);
         return Status::Running;
      }

      if (status == Status::Success)
      {
         blackboard.SetValue<size_t>(key, 0);
         return Status::Success;
      }

      ++currentIndex;
   }

   blackboard.SetValue<size_t>(key, 0);
   return Status::Failure;
}

ARC::BehaviorTree::Status ARC::BehaviorTree::RandomSelector::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   std::string key = "RandomSelectorIndex_" + m_uuid.ToString();

   if (m_children.empty())
      return Status::Failure;

   size_t index;
   if (blackboard.HasValue(key))
   {
      index = blackboard.GetValue<size_t>(key).value();
   }
   else
   {
      std::uniform_int_distribution<> dis(0, static_cast<int32_t>(m_children.size() - 1));
      index = dis(m_gen);
      blackboard.SetValue<size_t>(key, index);
   }

   Status status = m_children[index]->Update(scene, entity, blackboard);

   if (status == Status::Running)
   {
      return Status::Running;
   }
   else
   {
      blackboard.SetValue<size_t>(key, 0);
      return status;
   }
}

ARC::BehaviorTree::Status ARC::BehaviorTree::Parallel::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   size_t successCount = 0;
   size_t failureCount = 0;

   for (auto& child : m_children)
   {
      Status status = child->Update(scene, entity, blackboard);

      if (status == Status::Success)
      {
         ++successCount;
         if (m_successPolicy == Policy::RequireOne)
         {
            return Status::Success;
         }
      }
      else if (status == Status::Failure)
      {
         ++failureCount;
         if (m_failurePolicy == Policy::RequireOne)
         {
            return Status::Failure;
         }
      }
   }

   if (m_successPolicy == Policy::RequireAll && successCount == m_children.size())
      return Status::Success;
   if (m_failurePolicy == Policy::RequireAll && failureCount == m_children.size())
      return Status::Failure;

   return Status::Running;
}

ARC::BehaviorTree::Status ARC::BehaviorTree::Inverter::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   Status status = m_child->Update(scene, entity, blackboard);

   if (status == Status::Success)
      return Status::Failure;
   else if (status == Status::Failure)
      return Status::Success;
   else
      return Status::Running;
}

ARC::BehaviorTree::Status ARC::BehaviorTree::Repeater::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   std::string key = "RepeaterCount_" + m_uuid.ToString();

   int32_t currentCount = 0;
   if (blackboard.HasValue(key))
   {
      currentCount = blackboard.GetValue<int32_t>(key).value();
   }

   if (m_repeatCount == INFINITE_COUNT || currentCount < m_repeatCount)
   {
      Status status = m_child->Update(scene, entity, blackboard);

      if (status == Status::Running)
      {
         return Status::Running;
      }

      blackboard.SetValue<int32_t>(key, ++currentCount);
      return Status::Running;
   }
   else
   {
      blackboard.SetValue<int32_t>(key, 0); // Reset for next time
      return Status::Success;
   }
}

ARC::BehaviorTree::Status ARC::BehaviorTree::RepeaterUntilFailure::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   Status status = m_child->Update(scene, entity, blackboard);

   if (status == Status::Failure)
   {
      return Status::Success;
   }
   else if (status == Status::Running)
   {
      return Status::Running;
   }
   else // Status::Success
   {
      return Status::Running; // Continue repeating
   }
}

ARC::BehaviorTree::Status ARC::BehaviorTree::UntilSuccess::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   Status status;
   do
   {
      status = m_child->Update(scene, entity, blackboard);
   }
   while (status == Status::Failure);

   return status;
}

ARC::BehaviorTree::Status ARC::BehaviorTree::Limiter::Update(Scene& scene, Entity entity, Blackboard& blackboard)
{
   if (m_callCount >= m_maxCalls)
      return Status::Failure;

   ++m_callCount;
   return m_child->Update(scene, entity, blackboard);
}
