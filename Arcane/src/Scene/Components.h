#pragma once

#include "Entity.h"
#include "Math/Vector.h"
#include "Util/UUID.h"

namespace ARC
{
   namespace Components
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
         Vector2f position;

         Transform() = default;
         Transform(const Transform& other) = default;
         Transform(const Vector2f& position) : position(position) {}

         bool operator==(const Transform& other) const { return position == other.position; }
         bool operator!=(const Transform& other) const { return position != other.position; }
      };
   } // namespace Components
} // namespace ARC
