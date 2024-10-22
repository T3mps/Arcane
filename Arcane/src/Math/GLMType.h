#pragma once

namespace ARC
{
   template  <typename GLM_t>
   class GLMType
   {
   public:
      virtual ~GLMType() = default;

      virtual GLM_t& Data() = 0;
      virtual const GLM_t& Data() const = 0;
   };
} // namespace ARC
