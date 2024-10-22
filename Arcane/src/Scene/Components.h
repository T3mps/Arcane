#pragma once

#include "Entity.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "Util/UUID.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace ARC::Components
{
   struct ID
   {
      UUID value;

      ID() = default;
      ID(const ID& other) = default;
      ID(const UUID& uuid) : value(uuid) {}

      operator UUID() const { return value; }

      bool operator==(const ID& other) const { return value == other.value; }
      bool operator!=(const ID& other) const { return value != other.value; }

      bool operator==(const UUID& other) const { return value == other; }
      bool operator!=(const UUID& other) const { return value != other; }
   };

   struct Tag
   {
      std::string value;

      Tag() = default;
      Tag(const Tag& other) = default;
      Tag(const std::string& tag) : value(tag) {}

      operator std::string() const { return value; }

      bool operator==(const Tag& other) const { return value == other.value; }
      bool operator!=(const Tag& other) const { return value != other.value; }

      bool operator==(const std::string& other) const { return value == other; }
      bool operator!=(const std::string& other) const { return value != other; }
   };

   struct Transform
   {
      Vector3f position;
      Vector3f rotation;
      Vector3f scale;

      Transform() = default;
      Transform(const Transform& other) = default;
      Transform(const Vector3f& position) : position(position) {}

      bool operator==(const Transform& other) const { return position == other.position; }
      bool operator!=(const Transform& other) const { return position != other.position; }

      Matrix4x4f GetTransform() const
      {
         glm::mat4 rotation = glm::toMat4(glm::quat(rotation));

         return Matrix4x4f(glm::translate(glm::mat4(1.0f), position.Data())
            * rotation
            * glm::scale(glm::mat4(1.0f), scale.Data()));
      }
   };
} // namespace ARC
