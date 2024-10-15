#pragma once

#include "Entity.h"
#include "Scene.h"
#include "System.h"
#include "Util/UUID.h"

namespace ARC
{
   namespace BehaviorTree
   {
      class Blackboard
      {
      public:
         template <typename T>
         void SetValue(const std::string& key, const T& value) { m_data[key] = value; }

         template <typename T>
         std::optional<T> GetValue(const std::string& key) const
         {
            auto it = m_data.find(key);
            if (it != m_data.end())
            {
               try
               {
                  return std::any_cast<T>(it->second);
               }
               catch (const std::bad_any_cast&)
               {
                  // Handle type mismatch
               }
            }
            return std::nullopt;
         }

         bool HasValue(const std::string& key) const { return m_data.find(key) != m_data.end(); }

      private:
         std::unordered_map<std::string, std::any> m_data;
      };

      enum class Status
      {
         Success = 0,
         Failure,
         Running
      };

      class Node
      {
      public:
         Node() = default;
         virtual ~Node() = default;

         virtual Status Update(Scene& scene, Entity entity, Blackboard& blackboard) = 0;

         UUID GetUUID() const { return m_uuid; }

      protected:
         UUID m_uuid;
      };

      class Sequence : public Node
      {
      public:
         Sequence() = default;
         virtual ~Sequence() = default;

         void AddChild(std::shared_ptr<Node> child) { m_children.push_back(child); }

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;

      private:
         std::vector<std::shared_ptr<Node>> m_children;
      };

      class Selector : public Node 
      {
      public:
         Selector() = default;
         virtual ~Selector() = default;

         void AddChild(std::shared_ptr<Node> child) { m_children.push_back(child); }

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;

      private:
         std::vector<std::shared_ptr<Node>> m_children;
      };

      class RandomSelector : public Node
      {
      public:
         RandomSelector() : m_gen(std::random_device{}()) {}

         void AddChild(std::shared_ptr<Node> child) { m_children.push_back(child); }

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;

      private:
         std::vector<std::shared_ptr<Node>> m_children;
         std::mt19937 m_gen;
      };

      class Parallel : public Node
      {
      public:
         enum class Policy
         {
            RequireOne,   // Succeeds if at least one child succeeds
            RequireAll    // Succeeds only if all children succeed
         };

         Parallel(Policy successPolicy, Policy failurePolicy) : m_successPolicy(successPolicy), m_failurePolicy(failurePolicy) {}

         void AddChild(std::shared_ptr<Node> child) { m_children.push_back(child); }

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;

      private:
         std::vector<std::shared_ptr<Node>> m_children;
         Policy m_successPolicy;
         Policy m_failurePolicy;
      };

      class Action : public Node
      {
      public:
         using ActionFunc = std::function<Status(Scene&, Entity, Blackboard&)>;

         Action(ActionFunc func) : m_action(func) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override { return m_action(scene, entity, blackboard); }
      private:
         ActionFunc m_action;
      };

      class Condition : public Node
      {
      public:
         using ConditionFunc = std::function<bool(Scene&, Entity, Blackboard&)>;

         Condition(ConditionFunc func) : m_condition(func) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override { return m_condition(scene, entity, blackboard) ? Status::Success : Status::Failure; }

      private:
         ConditionFunc m_condition;
      };

      class Decorator : public Node
      {
      public:
         Decorator(std::shared_ptr<Node> child) : m_child(child) {}

      protected:
         std::shared_ptr<Node> m_child;
      };

      class Inverter : public Decorator
      {
      public:
         Inverter(std::shared_ptr<Node> child) : Decorator(child) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;
      };

      class Repeater : public Decorator
      { 
      public:
         static const int INFINITE_COUNT = -1;

         Repeater(std::shared_ptr<Node> child, int repeatCount = INFINITE_COUNT) : Decorator(child), m_repeatCount(repeatCount) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;

      private:
         int m_repeatCount;     // -1 for infinite repetition
      };

      class RepeaterUntilFailure : public Decorator
      {
      public:
         RepeaterUntilFailure(std::shared_ptr<Node> child) : Decorator(child) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;
      };

      class Succeeder : public Decorator
      {
      public:
         Succeeder(std::shared_ptr<Node> child) : Decorator(child) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override
         {
            m_child->Update(scene, entity, blackboard);
            return Status::Success;
         }
      };

      class UntilSuccess : public Decorator
      {
      public:
         UntilSuccess(std::shared_ptr<Node> child) : Decorator(child) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;
      };

      class Limiter : public Decorator
      {
      public:
         Limiter(std::shared_ptr<Node> child, int maxCalls) : Decorator(child), m_maxCalls(maxCalls), m_callCount(0) {}

         Status Update(Scene& scene, Entity entity, Blackboard& blackboard) override;

      private:
         int m_maxCalls;
         int m_callCount;
      };
   };
} // namespace ARC