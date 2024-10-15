#pragma once

namespace ARC
{
   template  <typename GLM_t>
   class GLMType
   {
   public:
      virtual ~GLMType() = default;

      virtual GLM_t& GetGLMType() = 0;
      virtual const GLM_t& GetGLMType() const = 0;
   };
} // namespace ARC
